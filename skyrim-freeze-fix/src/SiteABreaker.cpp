#include "PCH.h"
#include "SiteABreaker.h"

#include "Config.h"
#include "SkyrimAnchors.h"
#include "Stats.h"

namespace WorkerSpinLockFix::SiteABreaker {

    namespace {

        // ----- Singleton-A field offsets (SE 1.5.97, see case-study 29
        // and FreezeLogger's MainWaitProbe) ---------------------------------
        constexpr std::uintptr_t kOffWake    = 0x58;  // worker-wake (auto)
        constexpr std::uintptr_t kOffAck     = 0x60;  // worker-ack  (manual)
        constexpr std::uintptr_t kOffWorkId  = 0x68;  // work-id (0 while waiting)
        constexpr std::uintptr_t kOffPending = 0x6c;  // 1 == wait scheduled

        // ----- resolved anchors (copied at Install) -------------------------
        std::uintptr_t g_singleton_slot{ 0 };  // address of the Singleton-A ptr

        // ----- hook ---------------------------------------------------------
        // id 34554: a tiny void-ish helper that reads everything it needs
        // from the rip-relative Singleton-A (not from register args) and
        // tail-blocks in WaitForSingleObjectEx. We forward the first four
        // integer registers verbatim so the wrap is transparent regardless
        // of the engine's true arity, and forward rax back to the caller.
        SafetyHookInline g_hook_sa{};

        // ----- episode state (single-producer: the main thread) -------------
        std::atomic<DWORD>          g_main_tid{ 0 };
        std::atomic<bool>           g_in_wait{ false };
        std::atomic<std::uint64_t>  g_episode_seq{ 0 };       // ++ per outer entry
        std::atomic<std::uint64_t>  g_episode_start_ms{ 0 };
        std::atomic<std::uintptr_t> g_episode_ack{ 0 };       // worker-ack handle
        std::atomic<std::uint32_t>  g_episode_workid{ 0 };

        // ----- watchdog -----------------------------------------------------
        std::thread       g_watchdog;
        std::atomic<bool> g_running{ false };

        // ----- config snapshot ----------------------------------------------
        bool          g_detect_only{ true };
        bool          g_diag{ false };
        std::uint32_t g_dwell_ms{ 5000 };
        std::uint32_t g_poll_ms{ 1000 };
        std::uint32_t g_recheck_ms{ 1500 };

        // id 34554 reentrancy depth on the main thread. Only main ever
        // touches this (the wrap guards on tid), so a thread_local int is
        // race-free and keeps nested calls in one episode.
        thread_local int tl_depth{ 0 };

        std::uint64_t NowMs() noexcept { return ::GetTickCount64(); }

        // ----- ntdll!NtQueryEvent (read-only event-state probe) ------------
        // Reads an event's signaled state WITHOUT consuming a signal, so we
        // can confirm the worker-ack is still unsignaled before delivering
        // one (and stand down if it just got signaled, i.e. main is waking).
        struct EVENT_BASIC_INFORMATION_ {
            LONG EventType;   // 0 = NotificationEvent, 1 = SynchronizationEvent
            LONG EventState;  // 0 = not signaled, 1 = signaled
        };
        using NtQueryEventFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        NtQueryEventFn LoadNtQueryEvent() noexcept {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (!ntdll) return nullptr;
            return reinterpret_cast<NtQueryEventFn>(
                ::GetProcAddress(ntdll, "NtQueryEvent"));
        }

        // -1 = unknown / not an event, 0 = not signaled, 1 = signaled.
        int QueryEventState(std::uintptr_t a_handle) noexcept {
            if (a_handle == 0) return -1;
            static const auto pNtQueryEvent = LoadNtQueryEvent();
            if (!pNtQueryEvent) return -1;
            EVENT_BASIC_INFORMATION_ info{};
            ULONG returned = 0;
            const auto status = pNtQueryEvent(
                reinterpret_cast<HANDLE>(a_handle), /*EventBasicInformation=*/0,
                &info, sizeof(info), &returned);
            if (status != 0) return -1;
            return info.EventState ? 1 : 0;
        }

        // ----- SEH-guarded Singleton-A snapshot ----------------------------
        // POD-only so the noexcept SEH frame holds no objects with
        // destructors. On any fault, ok stays false and the watchdog stands
        // down (never act on memory we could not read).
        struct SiteASnap {
            bool           ok;
            std::uintptr_t singleton;
            std::uintptr_t ack;
            std::uintptr_t wake;
            std::uint32_t  workid;
            std::uint32_t  pending;
        };

        SiteASnap ReadSiteA() noexcept {
            SiteASnap s{};
            __try {
                const auto inst =
                    *reinterpret_cast<volatile std::uintptr_t*>(g_singleton_slot);
                if (inst == 0) return s;  // ok == false
                s.singleton = inst;
                s.wake    = *reinterpret_cast<volatile std::uintptr_t*>(inst + kOffWake);
                s.ack     = *reinterpret_cast<volatile std::uintptr_t*>(inst + kOffAck);
                s.workid  = *reinterpret_cast<volatile std::uint32_t*>(inst + kOffWorkId);
                s.pending = *reinterpret_cast<volatile std::uint32_t*>(inst + kOffPending);
                s.ok = true;
                return s;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                s.ok = false;
                return s;
            }
        }

        // ----- id 34554 wrap ------------------------------------------------
        std::uintptr_t __fastcall Detour_SiteA(
            std::uintptr_t a1, std::uintptr_t a2,
            std::uintptr_t a3, std::uintptr_t a4)
        {
            const DWORD tid = ::GetCurrentThreadId();

            // id 34554 is a Main::Update call; the first thread we see here
            // is the main thread. Pin it once. (The render thread uses a
            // different Singleton-A helper, id 34557, so this never fires
            // off the main thread -- but the tid pin makes that explicit.)
            DWORD expected = 0;
            g_main_tid.compare_exchange_strong(expected, tid,
                std::memory_order_relaxed);

            const bool isMain = (tid == g_main_tid.load(std::memory_order_relaxed));

            if (isMain && tl_depth++ == 0) {
                // Capture the worker-ack handle here, at entry, while the
                // singleton fields are coherent. Only open an episode if a
                // wait is actually scheduled (pending==1 with a non-null
                // ack handle); otherwise the helper returns without blocking
                // and there is nothing to watch. Ordered before the
                // g_in_wait release store so the watchdog always reads a
                // handle that belongs to this episode.
                const auto snap = ReadSiteA();
                if (snap.ok && snap.pending == 1 && snap.ack != 0) {
                    g_episode_ack.store(snap.ack, std::memory_order_relaxed);
                    g_episode_workid.store(snap.workid, std::memory_order_relaxed);
                    g_episode_start_ms.store(NowMs(), std::memory_order_relaxed);
                    g_episode_seq.fetch_add(1, std::memory_order_relaxed);
                    g_in_wait.store(true, std::memory_order_release);
                }
            }

            // unsafe_call (no mutex): the helper blocks INFINITE in
            // WaitForSingleObjectEx and call<>'s internal mutex would be
            // held across that wait, serialising every other hooked wait.
            const auto ret =
                g_hook_sa.unsafe_call<std::uintptr_t>(a1, a2, a3, a4);

            if (isMain && --tl_depth == 0) {
                g_in_wait.store(false, std::memory_order_release);
            }
            return ret;
        }

        // ----- watchdog -----------------------------------------------------
        void WatchdogLoop() {
            std::uint64_t lastHandledSeq = 0;

            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_poll_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;

                if (!g_in_wait.load(std::memory_order_acquire)) continue;

                const auto seq     = g_episode_seq.load(std::memory_order_relaxed);
                const auto start   = g_episode_start_ms.load(std::memory_order_relaxed);
                const auto ackEntry = g_episode_ack.load(std::memory_order_relaxed);
                const auto widEntry  = g_episode_workid.load(std::memory_order_relaxed);

                if (seq == lastHandledSeq) continue;          // already handled
                if (NowMs() - start < g_dwell_ms) continue;   // not parked long enough

                // First sample: still parked in the same episode.
                if (!g_in_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                const auto s1 = ReadSiteA();
                if (!s1.ok) continue;             // unreadable: stand down
                if (s1.pending != 1) continue;    // not actually parked: race

                // Confirmation window. To act we require ZERO progress: main
                // still in the SAME episode (seq unchanged + still parked),
                // pending still 1, and the work-id + ack handle unchanged.
                // A Site-A wait that is going to complete clears pending and
                // ends the episode well inside this window; a stuck one does
                // not. (Unlike Site-B there are no decrementing counters --
                // the wait is binary, so "no progress" == "still parked".)
                std::this_thread::sleep_for(std::chrono::milliseconds(g_recheck_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;
                if (!g_in_wait.load(std::memory_order_acquire)) continue;
                if (g_episode_seq.load(std::memory_order_relaxed) != seq) continue;
                const auto s2 = ReadSiteA();
                if (!s2.ok) continue;
                if (s2.pending != 1) continue;

                const bool zeroProgress =
                    s1.workid == s2.workid &&
                    s1.ack    == s2.ack    &&
                    s2.ack    == ackEntry  &&
                    s2.workid == widEntry;
                if (!zeroProgress) {
                    if (g_diag) {
                        logs::info(
                            "[SiteABreaker.diag] main parked in id 34554 for "
                            "{} ms but Singleton-A changed across the window "
                            "(workid {}->{}, ack 0x{:x}->0x{:x}); not the "
                            "stuck signature, standing by.",
                            NowMs() - start, s1.workid, s2.workid, s1.ack, s2.ack);
                    }
                    continue;
                }

                // Defensive: if the worker-ack just got signaled, main is
                // about to wake on its own -- do not double-signal.
                const int ackState = QueryEventState(s2.ack);
                if (ackState == 1) {
                    if (g_diag) {
                        logs::info(
                            "[SiteABreaker.diag] worker-ack 0x{:x} is signaled "
                            "while main still parked; main is waking, standing "
                            "by.", s2.ack);
                    }
                    continue;
                }

                // ---- stuck worker-ack wait confirmed ----
                lastHandledSeq = seq;
                Stats::OnSiteAStuck();
                const auto dwell = NowMs() - start;

                if (g_detect_only) {
                    logs::warn(
                        "[SiteABreaker] STUCK Site-A worker-ack wait detected "
                        "(detect_only): main parked in id 34554 for {} ms with "
                        "Singleton-A pending=1, work-id={}, worker-ack 0x{:x} "
                        "NOT signaled (state={}) and zero progress across the "
                        "confirmation window. No worker will signal it -- this "
                        "is the case-study 29 freeze. WOULD release here; set "
                        "[site_a_breaker] detect_only=false to actually "
                        "recover.",
                        dwell, s2.workid, s2.ack, ackState);
                    continue;
                }

                const auto h = reinterpret_cast<HANDLE>(ackEntry);
                if (h == nullptr) {
                    logs::warn(
                        "[SiteABreaker] STUCK Site-A wait confirmed ({} ms) but "
                        "the worker-ack handle was not captured; cannot release "
                        "this episode.",
                        dwell);
                    continue;
                }

                ::SetEvent(h);
                Stats::OnSiteAReleased();
                logs::warn(
                    "[SiteABreaker] STUCK Site-A worker-ack wait RELEASED: main "
                    "was parked in id 34554 for {} ms (work-id={}) with no "
                    "worker to signal completion; delivered the missing signal "
                    "via SetEvent(0x{:x}). Main should resume this frame.",
                    dwell, s2.workid, reinterpret_cast<std::uintptr_t>(h));
            }
        }

    } // namespace

    bool Install() {
        const auto& cfg = Config::Get();
        if (!cfg.sab_enabled) {
            logs::info(
                "[SiteABreaker] disabled by config "
                "(site_a_breaker.enabled = false).");
            return false;
        }

        if (!SkyrimAnchors::AvailableSiteA()) {
            logs::warn(
                "[SiteABreaker] Site-A (id 34554 / Singleton-A) anchors not "
                "resolved ({}); module will NOT arm.",
                SkyrimAnchors::DiagnosticStringSiteA());
            return false;
        }

        const auto& a = SkyrimAnchors::Get();
        g_singleton_slot = a.singletonASlot;

        g_detect_only = cfg.sab_detect_only;
        g_diag        = cfg.sab_diagnostic_logging;
        g_dwell_ms    = cfg.sab_dwell_threshold_ms;
        g_poll_ms     = (cfg.sab_poll_interval_ms == 0) ? 1000
                                                        : cfg.sab_poll_interval_ms;
        g_recheck_ms  = cfg.sab_recheck_window_ms;

        try {
            auto h = safetyhook::create_inline(
                reinterpret_cast<void*>(a.siteALockFn),
                reinterpret_cast<void*>(&Detour_SiteA));
            if (!h) {
                logs::critical(
                    "[SiteABreaker] safetyhook::create_inline FAILED on "
                    "id 34554 at 0x{:x}; module not armed.",
                    a.siteALockFn);
                return false;
            }
            g_hook_sa = std::move(h);
        } catch (const std::exception& e) {
            logs::critical("[SiteABreaker] install threw: {}", e.what());
            g_hook_sa = {};
            return false;
        }

        g_running.store(true, std::memory_order_relaxed);
        g_watchdog = std::thread(WatchdogLoop);
        g_watchdog.detach();

        logs::info(
            "[SiteABreaker] armed. id 34554 @0x{:x} (+0x{:x}), Singleton-A "
            "slot @0x{:x}. mode={}, dwell_ms={}, poll_ms={}, recheck_ms={}, "
            "handle_capture=ack@+0x60-at-entry, diag={}.",
            a.siteALockFn, a.siteALockFnRVA, a.singletonASlot,
            g_detect_only ? "DETECT-ONLY" : "ACTIVE (will SetEvent)",
            g_dwell_ms, g_poll_ms, g_recheck_ms,
            g_diag ? "ON" : "OFF");
        return true;
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
    }

}
