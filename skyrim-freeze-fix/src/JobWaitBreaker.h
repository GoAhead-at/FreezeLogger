#pragma once

namespace WorkerSpinLockFix::JobWaitBreaker {

    // Runtime recovery for the Skyrim main-thread WaitForJobTask
    // lost-wakeup hang -- the SECOND hard-freeze class, distinct from the
    // AB-BA BSSpinLock inversion that Phase4Defer prevents. See
    // ../../docs/case-study/28-jobwaitbreaker-design.md and case-study 27.
    //
    // Mechanism of the bug
    // --------------------
    // Main::Update periodically blocks in WaitForJobTask on a per-job
    // completion event until in-flight jobs drain. Under heavy
    // parallelism a publish-then-tear-down race can lose the wakeup:
    // main reads the job chain as non-null, commits to
    // WaitForSingleObject(handle, INFINITE) on a manual-reset
    // NotificationEvent, then AFTER main is asleep another thread clears
    // the job sub-array WITHOUT signalling main's event. No thread is
    // left to signal it, so main sleeps forever and the game freezes.
    //
    // How this module recovers
    // ------------------------
    //   1. An inline wrap on WaitForJobTask (resolved version-
    //      independently via SkyrimAnchors) marks "main is in a job-wait
    //      since T, on job index N" and records the Singleton-B job slot
    //      it is waiting on.
    //   2. A watchdog thread polls. When main has been parked longer than
    //      a dwell threshold AND the job slot has been torn down to null
    //      (the job is provably gone) AND the state survives a re-check
    //      window, it is the lost-wakeup signature.
    //   3. In detect-only mode (default) it only logs. In active mode it
    //      delivers the missing signal (SetEvent on the handle captured
    //      by the WaitForSingleObjectEx wrap) so main wakes and the frame
    //      proceeds exactly as if the lost completion signal had arrived.
    //
    // Safety: the break gate requires the job to already be gone, so
    // waking main is equivalent to the signal that was lost (worst case a
    // one-frame glitch vs a permanent freeze). SetEvent on a manual-reset
    // event is idempotent. The dwell threshold + re-check guarantee no
    // healthy frame is ever touched. This module never suspends an engine
    // thread or reads thread contexts.
    //
    // Returns true if armed (anchors resolved + WaitForJobTask wrap
    // installed + watchdog started). Best-effort: any failure leaves the
    // engine unmodified and the plugin running.
    bool Install();

    void Stop();

}
