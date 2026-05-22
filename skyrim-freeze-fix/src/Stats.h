#pragma once

namespace WorkerSpinLockFix::Stats {

    // ---- AcquireHook counters -------------------------------------------
    // We deliberately do NOT count fast-path acquires. A contended atomic
    // on the uncontended hot path of BSSpinLock::Acquire is itself a
    // freeze-class regression. The slow-path counter is enough to verify
    // the hook is being invoked.
    //
    // Slow path: contended acquire (state != 0, owner != me). Triggers
    //            WaitGraph + Breaker work. Rare in normal play.
    void OnAcquireSlow() noexcept;

    // ---- Breaker counters -----------------------------------------------
    // First observation of a new cycle signature.
    void OnCycleObserved() noexcept;
    // Cycle signature has now persisted >= confirmation_window_ms.
    void OnCycleConfirmed() noexcept;
    // Break disabled by config (detect-only mode).
    void OnBreakSuppressed() noexcept;
    // CAS state->0 succeeded; we force-released a deadlocked BSSpinLock.
    void OnBreakDone() noexcept;
    // Cycle vanished or CAS lost the race; no break performed.
    void OnBreakRaced() noexcept;

    // ---- Phase 4 structural defer (Phase4Defer.cpp) --------------------
    // A LockB-acquirer call (id 40333 or id 40334) was intercepted with
    // LockA already held by the current thread; the call was pushed
    // onto the thread-local deferred queue instead of being executed.
    void OnPhase4Queued() noexcept;
    // A queued LockB-acquirer call was executed during the drain that
    // runs when a thread's LockA depth returns to 0.
    void OnPhase4Drained() noexcept;
    // A LockB-acquirer call hit the gate without LockA held and was
    // tail-called through to the original (the common, no-deadlock
    // path). Counts the work the structural fix did NOT need to defer.
    void OnPhase4PassThrough() noexcept;

    // ---- Reaper counters (stale-owner backstop) ------------------------
    void OnReaperScan(
        std::size_t threads,
        std::size_t spinners,
        std::size_t candidates,
        std::size_t stable_edges) noexcept;
    void OnStaleReaped() noexcept;
    void OnForceRace()  noexcept;

    void StartPeriodicDump();
    void Stop();

}
