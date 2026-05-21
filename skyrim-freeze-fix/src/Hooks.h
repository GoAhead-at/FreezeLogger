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
    //   4. Reaper::Install() (optional)     — stale-owner backstop for
    //                                         lock owners that died
    //                                         while holding the lock.
    //
    // Returns true if the AcquireHook was installed. The reaper is best
    // effort: a reaper failure does not fail Install() since the
    // AcquireHook + WaitGraph + Breaker pipeline is the primary
    // mechanism.
    bool Install();

}
