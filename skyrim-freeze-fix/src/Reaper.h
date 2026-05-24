#pragma once

namespace WorkerSpinLockFix::Reaper {

    // Stale-owner reaper (v2.0.3 redesign). Periodically:
    //   - reads a snapshot of WaitGraph wait edges,
    //   - dereferences each edge's `waiting_on` lock to inspect
    //     `(owner, state)`,
    //   - if an observed lock is held by a TID that is no longer alive
    //     and the edge has been stable for kStaleStableMs (2 s),
    //     force-releases it via CAS(state -> 0).
    //
    // Read-only with respect to the engine: NO SuspendThread,
    // GetThreadContext, ResumeThread, Toolhelp32 enumeration, or stack
    // scanning on the runtime path. The previous suspension-based
    // design was retired in v2.0.3; see
    // docs/case-study/26-reaper-snapshot-removed.md for the rationale.
    //
    // Coverage: only sees threads that traversed the AcquireHook slow
    // path. Requires `[acquire_hook] enabled = true` to do anything;
    // with AcquireHook disabled the WaitGraph is empty and the reaper
    // is a no-op every tick.
    //
    // Poll interval is configurable; see Config::reaper_interval_ms.
    bool Install();

    void Stop();

}
