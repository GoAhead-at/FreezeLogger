#pragma once

namespace WorkerSpinLockFix::Config {

    struct Settings {
        bool enabled{ true };

        std::uint32_t stats_interval_s{ 60 };

        // ---- AcquireHook + Breaker -----------------------------------------

        // Emergency kill-switch for the entry-point hook on
        // BSSpinLock::Acquire. If false, the hook is NOT installed and
        // the plugin runs reaper-only. Use this if any AcquireHook-class
        // regression (hot-path overhead, prologue collision with another
        // mod, etc.) makes the game unplayable, without rebuilding or
        // removing the DLL.
        bool acquire_hook_enabled{ true };

        // If false, Breaker logs cycle observations but never force-
        // releases a lock. With the surgical filter (LockA/LockB only)
        // and a short confirmation window, breaks are rare and only
        // occur on the documented engine AB-BA race.
        bool break_enabled{ true };

        // Wall-clock duration a cycle topology must remain observable
        // before the breaker will force-release a lock. The first
        // detector of a signature claims the confirmation flow, sleeps
        // this long, then re-runs WaitGraph::VerifyCycleStillPresent.
        // If the cycle has self-resolved during the window the breaker
        // stands down; otherwise the lock is force-released.
        //
        // A short window is fine because a true deadlock is observed
        // thousands of times per millisecond, while a transient near-
        // cycle resolves itself in microseconds.
        std::uint32_t confirmation_window_ms{ 2 };

        // If true, log every distinct cycle signature first observation
        // and every confirmation crossing. Recommended on so any break
        // that actually fires can be audited from the log.
        bool log_cycle_events{ true };

        // ---- Phase 4 structural defer (Phase4Defer.cpp) --------------------

        // Phase 4 layered structural fix on top of the v1.0.0 runtime
        // breaker. Wraps id 19369 (the LockA acquirer) and gates id
        // 40333 / id 40334 (the two LockB acquirers) so that calls
        // into the LockB acquirers are deferred when the current
        // thread is inside the LockA acquirer. This breaks the LA->LB
        // edge of the AB-BA cycle structurally; the runtime breaker
        // remains installed as defence-in-depth.
        //
        // See docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md
        // for the full design and correctness audit.
        //
        // Default ON: the structural fix is the cleaner mechanism;
        // the runtime breaker becomes the safety net rather than the
        // primary detection layer. Set to false to revert to the
        // v1.0.0 runtime-breaker-only configuration without
        // rebuilding or removing the DLL.
        bool phase4_defer_enabled{ true };

        // ---- Backstop: stale-owner reaper ----------------------------------

        // The reaper runs as a safety net for cases the entry-point
        // hook misses (threads that died holding a lock, indirect calls
        // our hook never sees, etc.). It is the only part of the plugin
        // that suspends engine threads + makes Psapi calls + allocates,
        // so it is also the only part that can plausibly cause load-time
        // stalls on heavy modlists. Disabled by default; the surgical
        // AcquireHook + Breaker pipeline is sufficient for the
        // documented engine bug.
        bool reaper_enabled{ false };

        // Reaper poll interval. Stale-owner deadlocks last forever, so
        // long latency between scans is acceptable; the steady-state
        // cost of a full thread enumeration + per-thread
        // SuspendThread/GetThreadContext at this interval is negligible.
        std::uint32_t reaper_interval_ms{ 30000 };

        // ---- Test mode (synthetic AB-BA validation) ------------------------

        // If true, after kDataLoaded the plugin spawns two threads that
        // deliberately AB-BA two heap-allocated test BSSpinLocks routed
        // through the real BSSpinLock::Acquire (id 12210) so they go
        // through the surgical hook. Without the breaker the threads
        // would deadlock forever; with the breaker the time-based
        // confirmation flow detects the cycle, sleeps the confirmation
        // window, verifies the cycle is still present, force-releases
        // one test lock, and both test threads complete.
        //
        // The test does NOT touch the engine's BSSpinLocks. It allocates
        // its own pair of BSSpinLock objects and registers them with
        // AcquireHook::AddTestLocks. The breaker writes only into those
        // two pointers; engine state is never modified.
        //
        // Default OFF. Enable manually in the TOML to validate the
        // breaker, then disable again before regular play. The test
        // produces a clear [TEST] SUCCESS or [TEST] FAILURE line in the
        // log within ~5 seconds of kDataLoaded.
        bool test_mode_enabled{ false };
    };

    void Init();
    const Settings& Get();

    std::filesystem::path ConfigPath();

}
