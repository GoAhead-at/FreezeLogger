#pragma once

namespace WorkerSpinLockFix::Hooks {

    // Bring-up orchestrator. Initialises in order:
    //
    //   1. WaitGraph::Init()                — registry storage for
    //                                         per-thread waiting_on
    //                                         markers used by cycle
    //                                         detection.
    //   2. Breaker::Init()                  — recent-cycles map for
    //                                         signature deduplication
    //                                         and confirmation tracking.
    //   3. AcquireHook::Install()           — entry-point inline hook on
    //                                         BSSpinLock::Acquire
    //                                         (id 12210) via
    //                                         safetyhook::create_inline.
    //   4. Phase4Defer::Install() (optional)— structural fix that
    //                                         breaks the AB-BA cycle's
    //                                         LA->LB direction by
    //                                         deferring LockB acquires
    //                                         (id 40333 / id 40334)
    //                                         while the current thread
    //                                         is inside the LockA
    //                                         acquirer (id 19369). See
    //                                         Phase4Defer.h.
    //   5. Reaper::Install() (optional)     — stale-owner backstop for
    //                                         lock owners that died
    //                                         while holding the lock.
    //
    // Returns true if the AcquireHook was installed. Phase4Defer and
    // the reaper are best effort: a failure in either does not fail
    // Install() since the AcquireHook + WaitGraph + Breaker pipeline
    // is sufficient by itself for the documented engine bug.
    bool Install();

}
