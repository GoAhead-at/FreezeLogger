#pragma once

namespace WorkerSpinLockFix::WaitGraph {

    // BSSpinLock layout, derived from the static analysis recorded in
    // docs/case-study/06-root-cause.md and confirmed in
    // docs/case-study/11-worker-spinlockfix-retrospective.md.
    //
    //   uint32 owner; // +0x0  current/last owner TID; not authoritative on
    //                 //       its own because the engine does not always
    //                 //       clear it on release.
    //   uint32 state; // +0x4  0 = free, 1 = held (observed). state is
    //                 //       authoritative for "held".
    //
    // We treat this as a flat POD struct of two uint32 fields. We never
    // dereference any deeper engine fields through this view.
    struct Lock {
        std::uint32_t owner;
        std::uint32_t state;
    };
    static_assert(sizeof(Lock) == 8, "BSSpinLock view must be exactly 8 bytes");
    static_assert(offsetof(Lock, owner) == 0, "owner must be at +0");
    static_assert(offsetof(Lock, state) == 4, "state must be at +4");

    // One participant in a wait cycle: the thread `waiter` is currently
    // waiting on `waiting_on`, which is owned by `owner`.
    struct CycleParticipant {
        DWORD          waiter{ 0 };
        Lock*          waiting_on{ nullptr };
        DWORD          owner{ 0 };
    };

    // Hard cap on chain length. Real deadlocks are 2-cycles. Capped to
    // bound the walk under adversarial races and to size stack buffers in
    // the AcquireHook slow path.
    inline constexpr int kMaxHops = 16;

    // Publish (or refresh) the fact that `tid` is currently spinning on
    // `target`, and stamp the edge's liveness time. `target` must point to
    // a Lock view.
    //
    // Called from the BSSpinLock::Acquire spin-retry hook on every outer
    // backoff iteration, so it doubles as "establish the edge" and "keep
    // the edge alive". Returns true ONLY on the transition from "not
    // waiting" / "waiting on a different lock" to "waiting on target", so
    // callers can count distinct stuck episodes without inflating a stat on
    // every backoff iteration.
    //
    // The spin-retry hook has no natural "stopped waiting" callback -- a
    // thread that finally acquires the lock simply stops hitting the retry
    // site -- so edges are SELF-EXPIRING: a published edge is honoured by
    // the readers below only for a short window after its last refresh
    // (see kEdgeStaleMs in WaitGraph.cpp). This replaces the exact
    // EnterSlow/ExitSlow bracket the old entry-point hook provided.
    //
    // noexcept and heap-alloc-free: callable from inside a synchronization
    // primitive's acquire path without coupling that primitive to the heap
    // CRITICAL_SECTION.
    bool EnterSlow(DWORD tid, Lock* target) noexcept;

    // Proactively mark this thread as no longer waiting. Retained for
    // completeness and for callers that DO have a clean release point; the
    // spin-retry hook does not call it (it relies on self-expiry instead).
    void ExitSlow(DWORD tid) noexcept;

    // Walk the wait-for chain starting at `target`. If `me` (the thread
    // currently entering Acquire on `target`) would close a cycle, write
    // the cycle into `out` and return its length (>= 2). Otherwise return
    // 0.
    //
    // The walk is bounded by kMaxHops (16) and reads only `target->owner`
    // and other threads' `waiting_on` fields. It does not lock anything
    // and does not allocate. Writes into a caller-provided buffer so the
    // hook's slow path does not introduce a (heap CRITICAL_SECTION ->
    // BSSpinLock) lock-order edge.
    int WouldFormCycle(
        DWORD               me,
        Lock*               target,
        CycleParticipant*   out,
        int                 max_hops) noexcept;

    // Re-verify after suspending the cycle members that the same chain is
    // still intact. Used as a second filter inside Breaker before any
    // force-release. Stack-buffer based; does not allocate.
    bool VerifyCycleStillPresent(
        const CycleParticipant* cycle, int cycle_len) noexcept;

    // One observed wait edge: thread `waiter` is currently sitting in the
    // AcquireHook slow path on `waiting_on`. Reaper consumes batches of
    // these to find stale-owner edges without needing to suspend any
    // engine thread.
    struct EdgeView {
        DWORD waiter{ 0 };
        Lock* waiting_on{ nullptr };
    };

    // Copy the currently-active wait edges into `out`, capped at `cap`.
    // Returns the number of edges written. Lock-free, allocation-free,
    // safe to call from any thread. Skips slots whose `tid` is zero, whose
    // `waiting_on` is null, or whose edge has expired (not refreshed within
    // kEdgeStaleMs) -- so a thread that acquired its lock and moved on no
    // longer appears here.
    //
    // The snapshot is racy by design: a slot's `waiting_on` may be
    // cleared by the owning thread between the load here and the
    // caller's read of the lock fields, in which case the caller will
    // see the lock as "not held" and discard the edge. That race is
    // benign because the only consumer (Reaper) treats observed edges
    // as candidates that must remain stable for `kStaleStableMs`
    // before any action is taken.
    int SnapshotEdges(EdgeView* out, int cap) noexcept;

    // Idempotent. Sets up registry storage. Must be called before EnterSlow
    // is invoked from any thread.
    void Init();

}
