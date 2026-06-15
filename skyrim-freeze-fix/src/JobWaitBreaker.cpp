#include "PCH.h"
#include "JobWaitBreaker.h"

#include "Config.h"
#include "SkyrimAnchors.h"
#include "Stats.h"

namespace WorkerSpinLockFix::JobWaitBreaker {

    namespace {

        // ----- resolved anchors (copied at Install) -------------------------
        std::uintptr_t   g_singleton_slot{ 0 };  // address of the global ptr

        // ----- hooks --------------------------------------------------------
        // WaitForJobTask(idx, flag). Declared returning uintptr_t (a safe
        // superset for an integer/pointer or void return: a void callee just
        // leaves rax unused by the caller). __fastcall: rcx=idx, rdx=flag.
        using WaitForJobTaskFn =
            std::uintptr_t(__fastcall*)(std::uint32_t, std::uint32_t);
        SafetyHookInline g_hook_wfjt{};

        // WaitForSingleObjectEx -- only installed in active (break) mode, to
        // capture the kernel HANDLE main is about to park on. Pure observer
        // on the hot path: it records the handle for the one matching call
        // and otherwise tail-calls through.
        using WfsoExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
        SafetyHookInline g_hook_wfso{};

        // ----- episode state (single-producer: the main thread) -------------
        std::atomic<DWORD>         g_main_tid{ 0 };
        std::atomic<bool>          g_in_job_wait{ false };
        std::atomic<std::uint64_t> g_episode_seq{ 0 };       // ++ per outer entry
        std::atomic<std::uint64_t> g_episode_start_ms{ 0 };
        std::atomic<std::uint32_t> g_episode_idx{ 0 };       // job index arg
        std::atomic<std::uintptr_t> g_episode_handle{ 0 };   // captured by WFSO wrap

        // ----- watchdog -----------------------------------------------------
        std::thread       g_watchdog;
        std::atomic<bool> g_running{ false };

        // ----- config snapshot ----------------------------------------------
        bool          g_detect_only{ true };
        bool          g_diag{ false };
        std::uint32_t g_dwell_ms{ 3000 };
        std::uint32_t g_poll_ms{ 1000 };
        std::uint32_t g_recheck_ms{ 250 };

        // WaitForJobTask reentrancy depth on the main thread. Only main ever
        // touches this (the wrap guards on tid), so a thread_local int is
        // race-free and lets nested calls keep one episode.
        thread_local int tl_depth{ 0 };

        std::uint64_t NowMs() noexcept { return ::GetTickCount64(); }

        // Walk the job-pool chain for `idx` and report whether the slot main
        // is waiting on has been torn down to null (the job is gone). At
        // wait-entry the engine only blocks when the element is non-null, so
        // a null here means a producer cleared the queue after main parked --
        // the lost-wakeup signature. SEH-guarded: if we cannot prove
        // teardown, we report false and never act.
        bool JobTornDown(std::uint32_t a_idx) noexcept {
            __try {
                const auto instance =
                    *reinterpret_cast<volatile std::uintptr_t*>(g_singleton_slot);
                if (instance == 0) return true;
                const auto subArray =
                    *reinterpret_cast<volatile std::uintptr_t*>(instance + 8);
                if (subArray == 0) return true;
                const auto element = *reinterpret_cast<volatile std::uintptr_t*>(
                    subArray + static_cast<std::uintptr_t>(a_idx) * 8);
                return element == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ----- WaitForJobTask wrap ------------------------------------------
        std::uintptr_t __fastcall Detour_WaitForJobTask(
            std::uint32_t a_idx, std::uint32_t a_flag)
        {
            const DWORD tid = ::GetCurrentThreadId();

            // WaitForJobTask is a Main::Update call; the first thread we see
            // here is the main thread. Pin it once.
            DWORD expected = 0;
            g_main_tid.compare_exchange_strong(expected, tid,
                std::memory_order_relaxed);

            const bool isMain = (tid == g_main_tid.load(std::memory_order_relaxed));

            if (isMain && tl_depth++ == 0) {
                g_episode_idx.store(a_idx, std::memory_order_relaxed);
                g_episode_handle.store(0, std::memory_order_relaxed);
                g_episode_start_ms.store(NowMs(), std::memory_order_relaxed);
                g_episode_seq.fetch_add(1, std::memory_order_relaxed);
                g_in_job_wait.store(true, std::memory_order_release);
            }

            const auto ret = g_hook_wfjt.call<std::uintptr_t>(a_idx, a_flag);

            if (isMain && --tl_depth == 0) {
                g_in_job_wait.store(false, std::memory_order_release);
            }
            return ret;
        }

        // ----- WaitForSingleObjectEx wrap (active mode only) ----------------
        DWORD WINAPI Detour_Wfso(HANDLE a_handle, DWORD a_ms, BOOL a_alertable) {
            if (a_ms == INFINITE &&
                g_in_job_wait.load(std::memory_order_acquire) &&
                ::GetCurrentThreadId() ==
                    g_main_tid.load(std::memory_order_relaxed)) {
                g_episode_handle.store(
                    reinterpret_cast<std::uintptr_t>(a_handle),
                    std::memory_order_relaxed);
            }
            return g_hook_wfso.call<DWORD>(a_handle, a_ms, a_alertable);
        }

        // ----- watchdog -----------------------------------------------------
        void WatchdogLoop() {
            std::uint64_t lastHandledSeq = 0;

            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_poll_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;

                if (!g_in_job_wait.load(std::memory_order_acquire)) continue;

                const auto seq   = g_episode_seq.load(std::memory_order_relaxed);
                const auto start = g_episode_start_ms.load(std::memory_order_relaxed);
                const auto idx   = g_episode_idx.load(std::memory_order_relaxed);

                if (seq == lastHandledSeq) continue;  // already handled
                if (NowMs() - start < g_dwell_ms) continue;

                // First gate: still in the same episode and job is gone.
                if (!g_in_job_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                if (!JobTornDown(idx)) {
                    // Job still in flight: a genuinely long job/IO stall, not
                    // a lost wakeup. Leave it alone.
                    if (g_diag) {
                        logs::info(
                            "[JobWaitBreaker.diag] main in WaitForJobTask "
                            "idx={} for {} ms but job slot still populated; "
                            "not a lost-wakeup, standing by.",
                            idx, NowMs() - start);
                    }
                    continue;
                }

                // Confirmation re-check: let the state settle, then re-verify.
                std::this_thread::sleep_for(std::chrono::milliseconds(g_recheck_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;
                if (!g_in_job_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                if (!JobTornDown(idx)) continue;

                // ---- lost-wakeup confirmed ----
                lastHandledSeq = seq;
                Stats::OnJobWaitStuck();
                const auto dwell = NowMs() - start;

                if (g_detect_only) {
                    logs::warn(
                        "[JobWaitBreaker] LOST-WAKEUP detected (detect_only): "
                        "main parked in WaitForJobTask idx={} for {} ms; the "
                        "job slot has been torn down to null and no producer "
                        "will signal it. This is the case-study 28 freeze. "
                        "WOULD release here -- set [job_wait_breaker] "
                        "detect_only=false to actually recover.",
                        idx, dwell);
                    continue;
                }

                const auto h = reinterpret_cast<HANDLE>(
                    g_episode_handle.load(std::memory_order_relaxed));
                if (h == nullptr) {
                    logs::warn(
                        "[JobWaitBreaker] LOST-WAKEUP confirmed (idx={}, "
                        "{} ms) but the wait handle was not captured; cannot "
                        "release this episode. (Capture happens in the "
                        "WaitForSingleObjectEx wrap; if this recurs the wrap "
                        "may not be installed.)",
                        idx, dwell);
                    continue;
                }

                ::SetEvent(h);
                Stats::OnJobWaitReleased();
                logs::warn(
                    "[JobWaitBreaker] LOST-WAKEUP RELEASED: main was parked in "
                    "WaitForJobTask idx={} for {} ms on a torn-down job; "
                    "delivered the missing signal via SetEvent(0x{:x}). Main "
                    "should resume this frame.",
                    idx, dwell, reinterpret_cast<std::uintptr_t>(h));
            }
        }

    } // namespace

    bool Install() {
        const auto& cfg = Config::Get();
        if (!cfg.jwb_enabled) {
            logs::info(
                "[JobWaitBreaker] disabled by config "
                "(job_wait_breaker.enabled = false).");
            return false;
        }

        if (!SkyrimAnchors::Available()) {
            logs::warn(
                "[JobWaitBreaker] WaitForJobTask/Singleton-B anchors not "
                "resolved ({}); module will NOT arm.",
                SkyrimAnchors::DiagnosticString());
            return false;
        }

        const auto& a = SkyrimAnchors::Get();
        g_singleton_slot = a.singletonBSlot;

        g_detect_only = cfg.jwb_detect_only;
        g_diag        = cfg.jwb_diagnostic_logging;
        g_dwell_ms    = cfg.jwb_dwell_threshold_ms;
        g_poll_ms     = (cfg.jwb_poll_interval_ms == 0) ? 1000
                                                        : cfg.jwb_poll_interval_ms;
        g_recheck_ms  = cfg.jwb_recheck_window_ms;

        try {
            auto h = safetyhook::create_inline(
                reinterpret_cast<void*>(a.waitForJobTask),
                reinterpret_cast<void*>(&Detour_WaitForJobTask));
            if (!h) {
                logs::critical(
                    "[JobWaitBreaker] safetyhook::create_inline FAILED on "
                    "WaitForJobTask at 0x{:x}; module not armed.",
                    a.waitForJobTask);
                return false;
            }
            g_hook_wfjt = std::move(h);
        } catch (const std::exception& e) {
            logs::critical("[JobWaitBreaker] install threw: {}", e.what());
            g_hook_wfjt = {};
            return false;
        }

        // Active mode also needs to capture the wait handle so the watchdog
        // can deliver the missing signal. detect-only never releases, so it
        // does not install the (process-wide) WaitForSingleObjectEx wrap.
        bool wfso_armed = false;
        if (!g_detect_only) {
            HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
            if (kb == nullptr) kb = ::GetModuleHandleW(L"kernel32.dll");
            auto* wfso = kb ? reinterpret_cast<void*>(
                                  ::GetProcAddress(kb, "WaitForSingleObjectEx"))
                            : nullptr;
            if (wfso != nullptr) {
                try {
                    auto h = safetyhook::create_inline(
                        wfso, reinterpret_cast<void*>(&Detour_Wfso));
                    if (h) {
                        g_hook_wfso = std::move(h);
                        wfso_armed = true;
                    }
                } catch (const std::exception& e) {
                    logs::warn("[JobWaitBreaker] WFSO wrap threw: {}", e.what());
                }
            }
            if (!wfso_armed) {
                logs::warn(
                    "[JobWaitBreaker] could not install the "
                    "WaitForSingleObjectEx handle-capture wrap; active-mode "
                    "release will be unable to obtain the wait handle. "
                    "Consider running detect_only=true.");
            }
        }

        g_running.store(true, std::memory_order_relaxed);
        g_watchdog = std::thread(WatchdogLoop);
        g_watchdog.detach();

        logs::info(
            "[JobWaitBreaker] armed. WaitForJobTask @0x{:x} (+0x{:x}), "
            "Singleton-B slot @0x{:x}. mode={}, dwell_ms={}, poll_ms={}, "
            "recheck_ms={}, handle_capture={}, diag={}.",
            a.waitForJobTask, a.waitForJobTaskRVA, a.singletonBSlot,
            g_detect_only ? "DETECT-ONLY" : "ACTIVE (will SetEvent)",
            g_dwell_ms, g_poll_ms, g_recheck_ms,
            g_detect_only ? "n/a" : (wfso_armed ? "ON" : "FAILED"),
            g_diag ? "ON" : "OFF");
        return true;
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
    }

}
