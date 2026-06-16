#pragma once

namespace WorkerSpinLockFix::Hooks {

    // Bring-up orchestrator. Initialises, in order:
    //
    //   1. SkyrimAnchors::Init()      — version-independent signature scan
    //                                   for WaitForJobTask + the Singleton-B
    //                                   job-pool slot.
    //   2. Phase4Defer::Install()     — Layer 1: structural prevention of
    //                                   the AB-BA BSSpinLock inversion. One
    //                                   inline wrap on the LockA acquirer
    //                                   (id 19369) plus two cycle-hub
    //                                   call-site patches defer LockB
    //                                   acquires while LockA is held.
    //   3. JobWaitBreaker::Install()  — Layer 2: recovery for the Skyrim
    //                                   main-thread WaitForJobTask
    //                                   lost-wakeup hang (case-study 28).
    //   4. SiteABreaker::Install()    — Layer 3: recovery for the Skyrim
    //                                   main-thread Site-A worker-ack
    //                                   deadlock (id 34554, case-study 29).
    //
    // Each layer is independently config-gated and best-effort. Returns
    // true if at least one layer armed.
    bool Install();

}
