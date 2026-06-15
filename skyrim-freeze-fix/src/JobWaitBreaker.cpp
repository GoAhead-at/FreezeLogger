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

        // ----- episode state (single-producer: the main thread) -------------
        std::atomic<DWORD>         g_main_tid{ 0 };
        std::atomic<bool>          g_in_job_wait{ false };
        std::atomic<std::uint64_t> g_episode_seq{ 0 };       // ++ per outer entry
        std::atomic<std::uint64_t> g_episode_start_ms{ 0 };
        std::atomic<std::uint32_t> g_episode_idx{ 0 };       // job index arg (idx0)
        std::atomic<std::uintptr_t> g_episode_handle{ 0 };   // derived at wait entry

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

        // State of the job-pool chain for the entry main (idx) is parked on.
        enum class ChainState {
            Error,      // could not read memory -> never act
            TornDown,   // chain collapsed to null: the producer cleared the
                        // queue after main parked (the original case-study 28
                        // lost-wakeup variant, freeze_2026-06-15_160413).
            Resolved,   // entry still present; a_out holds its 8 header qwords.
        };

        // Sample the chain: instance = *(slot); subArray = *(instance+8);
        // element = *(subArray + idx*8). On Resolved, copy the entry's first
        // 8 qwords (its handle-table pointer + the outstanding-job counters
        // the engine decrements as jobs complete) into a_out so the watchdog
        // can tell whether the job is making ANY progress between samples.
        // SEH-guarded: on any fault we report Error and the watchdog stands
        // down (never act on memory we could not read).
        ChainState SampleChain(std::uint32_t a_idx,
                               std::array<std::uintptr_t, 8>& a_out) noexcept {
            __try {
                const auto instance =
                    *reinterpret_cast<volatile std::uintptr_t*>(g_singleton_slot);
                if (instance == 0) return ChainState::TornDown;
                const auto subArray =
                    *reinterpret_cast<volatile std::uintptr_t*>(instance + 8);
                if (subArray == 0) return ChainState::TornDown;
                const auto element = *reinterpret_cast<volatile std::uintptr_t*>(
                    subArray + static_cast<std::uintptr_t>(a_idx) * 8);
                if (element == 0) return ChainState::TornDown;
                for (std::size_t i = 0; i < a_out.size(); ++i) {
                    a_out[i] = *reinterpret_cast<volatile std::uintptr_t*>(
                        element + i * 8);
                }
                return ChainState::Resolved;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return ChainState::Error;
            }
        }

        // Derive the kernel HANDLE main is about to park on, the same way
        // WaitForJobTask does internally: the maintainer's note (case-study
        // 27 §0) describes the wrapper walking (*S)[+8][idx0]->vtable[idx1]
        // and tail-jumping to WaitForSingleObject. That resolves to:
        //   instance     = *(slot)
        //   subArray     = *(instance + 8)
        //   element      = *(subArray + idx0*8)
        //   handle_table = *(element + 0)
        //   handle       = *(handle_table + idx1*8)
        // The Layer-4 dump in freeze_2026-06-15_204830 confirmed this exactly:
        // entry[0].handle_table[1] == 0x257c == the HANDLE main was parked on.
        // We call this at wait ENTRY (chain still intact) and on the same
        // thread that is about to wait, so no other code can mutate the chain
        // between our read and the engine's -- we get the identical handle.
        // Capturing at entry is also what makes the torn-down variant
        // recoverable: by the time the watchdog sees the chain nulled, this
        // handle has long since been saved. SEH-guarded; returns 0 on any
        // fault or null link (the watchdog then logs "handle not captured").
        std::uintptr_t DeriveWaitHandle(std::uint32_t a_idx0,
                                        std::uint32_t a_idx1) noexcept {
            __try {
                const auto instance =
                    *reinterpret_cast<volatile std::uintptr_t*>(g_singleton_slot);
                if (instance == 0) return 0;
                const auto subArray =
                    *reinterpret_cast<volatile std::uintptr_t*>(instance + 8);
                if (subArray == 0) return 0;
                const auto element = *reinterpret_cast<volatile std::uintptr_t*>(
                    subArray + static_cast<std::uintptr_t>(a_idx0) * 8);
                if (element == 0) return 0;
                const auto handleTable =
                    *reinterpret_cast<volatile std::uintptr_t*>(element);
                if (handleTable == 0) return 0;
                return *reinterpret_cast<volatile std::uintptr_t*>(
                    handleTable + static_cast<std::uintptr_t>(a_idx1) * 8);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // ----- WaitForJobTask wrap ------------------------------------------
        // __fastcall: rcx = idx0 (job-list index), rdx = idx1 (handle index
        // within the list). Main::Update calls this with (0, 1).
        std::uintptr_t __fastcall Detour_WaitForJobTask(
            std::uint32_t a_idx0, std::uint32_t a_idx1)
        {
            const DWORD tid = ::GetCurrentThreadId();

            // WaitForJobTask is a Main::Update call; the first thread we see
            // here is the main thread. Pin it once.
            DWORD expected = 0;
            g_main_tid.compare_exchange_strong(expected, tid,
                std::memory_order_relaxed);

            const bool isMain = (tid == g_main_tid.load(std::memory_order_relaxed));

            if (isMain && tl_depth++ == 0) {
                // Derive and stash the wait handle here, at entry, while the
                // chain is still intact. Ordered before the g_in_job_wait
                // release store so the watchdog always reads a handle that
                // belongs to this episode.
                g_episode_idx.store(a_idx0, std::memory_order_relaxed);
                g_episode_handle.store(DeriveWaitHandle(a_idx0, a_idx1),
                    std::memory_order_relaxed);
                g_episode_start_ms.store(NowMs(), std::memory_order_relaxed);
                g_episode_seq.fetch_add(1, std::memory_order_relaxed);
                g_in_job_wait.store(true, std::memory_order_release);
            }

            // unsafe_call (no mutex): WaitForJobTask blocks for the wait and
            // call<>'s mutex would be held across it. Only main calls this, so
            // it is harmless today, but unsafe_call removes any
            // held-across-a-blocking-wait hazard.
            const auto ret =
                g_hook_wfjt.unsafe_call<std::uintptr_t>(a_idx0, a_idx1);

            if (isMain && --tl_depth == 0) {
                g_in_job_wait.store(false, std::memory_order_release);
            }
            return ret;
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

                // First sample: still in the same episode, snapshot the job
                // entry main is parked on.
                if (!g_in_job_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                std::array<std::uintptr_t, 8> fp1{};
                const auto s1 = SampleChain(idx, fp1);
                if (s1 == ChainState::Error) continue;  // unreadable: stand down

                // Confirmation window. To act we require the job to make ZERO
                // progress across it: either the chain stays torn down to
                // null, or the entry's bytes are byte-for-byte identical. A
                // genuinely long-but-progressing job mutates its outstanding-
                // job counters, so its fingerprint changes and we stand down.
                std::this_thread::sleep_for(std::chrono::milliseconds(g_recheck_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;
                if (!g_in_job_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                std::array<std::uintptr_t, 8> fp2{};
                const auto s2 = SampleChain(idx, fp2);
                if (s2 == ChainState::Error) continue;

                const bool tornDown =
                    (s1 == ChainState::TornDown && s2 == ChainState::TornDown);
                const bool stalledIntact =
                    (s1 == ChainState::Resolved && s2 == ChainState::Resolved &&
                     fp1 == fp2);

                if (!tornDown && !stalledIntact) {
                    if (g_diag) {
                        logs::info(
                            "[JobWaitBreaker.diag] main in WaitForJobTask "
                            "idx={} for {} ms but the job entry is still "
                            "changing (s1={}, s2={}); job is progressing, "
                            "standing by.",
                            idx, NowMs() - start,
                            static_cast<int>(s1), static_cast<int>(s2));
                    }
                    continue;
                }

                // ---- stuck job confirmed (no progress across the window) ----
                lastHandledSeq = seq;
                Stats::OnJobWaitStuck();
                const auto dwell = NowMs() - start;
                const char* kind = tornDown ? "torn-down (chain->null)"
                                            : "stalled-intact (zero progress)";

                if (g_detect_only) {
                    logs::warn(
                        "[JobWaitBreaker] STUCK job-wait detected (detect_only): "
                        "main parked in WaitForJobTask idx={} for {} ms with "
                        "the job making zero progress [{}]. No producer will "
                        "signal it -- this is the case-study 27/28 freeze. "
                        "WOULD release here; set [job_wait_breaker] "
                        "detect_only=false to actually recover.",
                        idx, dwell, kind);
                    continue;
                }

                const auto h = reinterpret_cast<HANDLE>(
                    g_episode_handle.load(std::memory_order_relaxed));
                if (h == nullptr) {
                    logs::warn(
                        "[JobWaitBreaker] STUCK job-wait confirmed (idx={}, "
                        "{} ms, [{}]) but the wait handle was not captured; "
                        "cannot release this episode. (The handle is derived "
                        "from the job-pool chain at wait entry; a 0 here means "
                        "the chain walk faulted or hit a null link.)",
                        idx, dwell, kind);
                    continue;
                }

                ::SetEvent(h);
                Stats::OnJobWaitReleased();
                logs::warn(
                    "[JobWaitBreaker] STUCK job-wait RELEASED: main was parked "
                    "in WaitForJobTask idx={} for {} ms on a job making zero "
                    "progress [{}]; delivered the missing signal via "
                    "SetEvent(0x{:x}). Main should resume this frame.",
                    idx, dwell, kind, reinterpret_cast<std::uintptr_t>(h));
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

        // The wait handle is derived from the job-pool chain at wait entry
        // (DeriveWaitHandle, inside the WaitForJobTask wrap). No process-wide
        // WaitForSingleObjectEx hook is needed any more, so active mode adds
        // no global hot-path cost beyond the once-per-frame WaitForJobTask
        // wrap that detect-only already installs.

        g_running.store(true, std::memory_order_relaxed);
        g_watchdog = std::thread(WatchdogLoop);
        g_watchdog.detach();

        logs::info(
            "[JobWaitBreaker] armed. WaitForJobTask @0x{:x} (+0x{:x}), "
            "Singleton-B slot @0x{:x}. mode={}, dwell_ms={}, poll_ms={}, "
            "recheck_ms={}, handle_capture=chain-derived, diag={}.",
            a.waitForJobTask, a.waitForJobTaskRVA, a.singletonBSlot,
            g_detect_only ? "DETECT-ONLY" : "ACTIVE (will SetEvent)",
            g_dwell_ms, g_poll_ms, g_recheck_ms,
            g_diag ? "ON" : "OFF");
        return true;
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
    }

}
