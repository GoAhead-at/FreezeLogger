#pragma once

namespace WorkerSpinLockFix::SiteABreaker {

    // Runtime recovery for the Skyrim main-thread Site-A worker-ack
    // deadlock -- the THIRD hard-freeze class, distinct from both the
    // AB-BA BSSpinLock inversion that Phase4Defer prevents and the
    // WaitForJobTask lost-wakeup hang that JobWaitBreaker recovers. See
    // ../../docs/case-study/29-siteabreaker-plan.md.
    //
    // Mechanism of the bug
    // --------------------
    // Main::Update has a SECOND infinite-wait site (id 34554, reached via
    // id 35565, driven from Faster HDT-SMP's per-frame parallel dispatch).
    // A caller publishes work into Singleton-A: it sets the work-pending
    // flag [+0x6c]=1 and wakes a worker via the auto-reset worker-wake
    // event [+0x58], then blocks INFINITE on the manual-reset worker-ack
    // event [+0x60] until the worker signals completion. When a worker
    // consumes the wake, takes the job, and never signals the ack (it
    // died, was lost, or is stuck elsewhere), main sleeps forever and the
    // game freezes. Two field captures (freeze_2026-06-16_133223 on
    // HDT-SMP 3.2.0.0, freeze_2026-06-16_152328 on 3.1.0.0) show the exact
    // state: pending=1, worker-wake NOT signaled (already consumed),
    // worker-ack NOT signaled, no BSSpinLock cycle -- so it is independent
    // of the HDT-SMP version.
    //
    // How this module recovers
    // ------------------------
    //   1. An inline wrap on id 34554 (resolved version-independently via
    //      SkyrimAnchors' Site-A signature scan) marks "main entered a
    //      Site-A wait since T" and captures the worker-ack HANDLE
    //      (Singleton-A[+0x60]) at entry, while the fields are coherent.
    //   2. A watchdog thread polls. When main has been parked in the SAME
    //      Site-A episode past a dwell threshold AND is still parked after
    //      a re-check window (zero progress: same episode, pending still 1,
    //      work-id unchanged, ack event still NOT signaled), it is the
    //      worker-ack deadlock signature -- a binary wait that, having not
    //      completed in seconds, will not complete.
    //   3. In detect-only mode (default) it only logs. In active mode it
    //      delivers the missing signal (SetEvent on the captured ack
    //      handle) so main wakes and the frame proceeds as if the worker
    //      had signaled completion.
    //
    // Safety: the dwell + zero-progress re-check guarantees no healthy
    // frame is ever touched (a Site-A wait that is going to complete does
    // so in well under the dwell threshold). The stuck work is HDT-SMP
    // cloth/physics -- per-frame, visual, self-correcting -- so the worst
    // case of releasing on a half-written result is a one-frame cosmetic
    // glitch versus a permanent freeze + force-kill. id 34554 is main-only
    // (the render thread uses a different Singleton-A pair, id 34557/34567),
    // so the wrap never runs off the main thread. This module never
    // suspends an engine thread or reads thread contexts.
    //
    // Returns true if armed (Site-A anchors resolved + id 34554 wrap
    // installed + watchdog started). Best-effort: any failure leaves the
    // engine unmodified and the plugin running.
    bool Install();

    void Stop();

}
