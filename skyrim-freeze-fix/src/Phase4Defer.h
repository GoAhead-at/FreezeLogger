#pragma once

namespace WorkerSpinLockFix::Phase4Defer {

    // Phase 4 structural fix layered on top of the v1.0.0 runtime
    // breaker. While AcquireHook + WaitGraph + Breaker observe the
    // AB-BA cycle and force-release a lock when one fires, this
    // module STOPS the cycle from forming in the first place by
    // breaking its LA->LB direction.
    //
    // Background
    // ----------
    // Phase 4.1 of the case study (docs/case-study/22) showed:
    //
    //   * The AB-BA deadlock between LockA (id 19369 holds it as
    //     `BSAutoLock<BSSpinLock>`) and LockB (id 40333 / id 40334
    //     acquire it as a non-recursive BSSpinLock) is fully
    //     characterised.
    //   * BOTH directions of the cycle pass through id 36016
    //     (a 96-way event-dispatch switch). The LA->LB direction
    //     fires when id 19369 transitively reaches id 36016 via
    //     id 19371 -> id 35974 -> id 36016. Inside id 36016 there
    //     are exactly two LockB-firing call sites: a direct call
    //     to id 40334 at +0xdcb (gated by a PlayerCharacter check)
    //     and a direct call to id 19372 at +0xfa3, where id 19372
    //     itself directly calls id 40333 at +0x606.
    //   * id 40333 (`AddToTempChangeList`) and id 40334
    //     (`RemoveFromTempChangeList`) are short, well-shaped
    //     functions whose mutations -- the bit-9 toggle of
    //     [actor+0xe0] (Actor::BOOL_BITS::kInTempChangeList), the
    //     ProcessLists bucket-array append/remove at [pl+0x158] /
    //     [pl+0x168], and the private global at 0x2f44db0 -- are
    //     NOT read by the immediate post-call followup at any of
    //     the cycle-firing call sites.
    //
    // Strategy (option C5 from the case study)
    // ----------------------------------------
    // Defer id 40333 and id 40334 calls when the current thread is
    // inside id 19369. Three inline hooks installed via safetyhook:
    //
    //   1. WRAP id 19369: increment a thread-local "LockA depth"
    //      counter on entry, run the original, decrement on return.
    //      When the counter returns to 0, drain the deferred queue.
    //   2. GATE id 40333 entry: if LockA depth > 0, push (pl, actor)
    //      onto the deferred queue and return early; otherwise
    //      tail-call the original.
    //   3. GATE id 40334 entry: same as #2.
    //
    // The drain at LockA-depth-0 is on the same thread that
    // originally queued the call, so per-thread call ordering is
    // preserved. LockB acquires happen normally during the drain
    // because LockA is no longer held.
    //
    // The LB->LA direction (id 40285 / id 36614 / id 38413 ->
    // id 19369) is left entirely alone. Once the LA->LB edge is
    // broken, the AB-BA cycle simply cannot form.
    //
    // Layering with the v1.0.0 runtime breaker
    // ----------------------------------------
    // This module is additive. The runtime breaker (AcquireHook +
    // WaitGraph + Breaker + Reaper) remains installed and active.
    // If for any reason the structural fix here does not break a
    // particular cycle (a path we missed, an edge case, an indirect
    // call we didn't decode), the runtime breaker still catches it
    // and force-releases. The two layers do not interfere: this
    // module operates above the BSSpinLock layer (it never enters
    // BSSpinLock::Acquire while LockA is held) so the
    // entry-point hook on id 12210 sees no interaction.
    //
    // Returns true when all three hooks installed cleanly. Returns
    // false on any partial failure; on partial failure ALL hooks
    // are torn down so the engine runs unmodified.
    bool Install();

}
