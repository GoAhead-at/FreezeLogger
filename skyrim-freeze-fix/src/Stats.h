#pragma once

namespace WorkerSpinLockFix::Stats {

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

    // ---- JobWaitBreaker (WaitForJobTask lost-wakeup recovery) ----------
    // The lost-wakeup signature was confirmed (main parked in
    // WaitForJobTask past the dwell threshold on a torn-down job).
    void OnJobWaitStuck() noexcept;
    // Active mode delivered the missing signal (SetEvent) to release main.
    void OnJobWaitReleased() noexcept;

    void StartPeriodicDump();
    void Stop();

}
