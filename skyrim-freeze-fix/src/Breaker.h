#pragma once

#include "WaitGraph.h"

namespace WorkerSpinLockFix::Breaker {

    // Called by AcquireHook's slow path immediately after WaitGraph reports
    // a chain that closes back to `me`. Drives the confirmation gate: an
    // identical cycle signature must be observed continuously for at least
    // Config::confirmation_window_ms before the break primitive runs.
    //
    // The cycle is passed as a pointer + length to a caller-owned stack
    // buffer. Heap-alloc-free on the slow path so the AcquireHook does not
    // introduce a (heap CRITICAL_SECTION -> BSSpinLock) lock-order edge.
    //
    // With break_enabled=false this function counts cycles but never
    // force-releases anything. Logging is gated by config and is off
    // the slow path's hot loop.
    void OnCycleDetected(
        DWORD                           me,
        WaitGraph::Lock*                target,
        const WaitGraph::CycleParticipant* cycle,
        int                             cycle_len);

    void Init();

}
