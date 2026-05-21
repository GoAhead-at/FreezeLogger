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
