#pragma once

namespace WorkerSpinLockFix::Phase4Defer {

    // The structural fix for the engine's AB-BA BSSpinLock inversion.
    // It STOPS the cycle from forming in the first place by breaking
    // its LA->LB direction.
    //
    // (Historically this was Layer 1 on top of a v1.0 runtime breaker
    // that detected and force-released live cycles. As of v2.3.0 the
    // runtime breaker and its reaper backstop have been removed: field
    // telemetry showed the structural fix prevents every cycle
    // (cycles_observed=0), so the breaker never fired and was redundant.
    // This module is now the sole AB-BA fix.)
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
    // Returns true when all hooks installed cleanly. Returns false on
    // any partial failure. On failure the LockA wrap is torn down, so
    // tl_lockA_depth can never leave 0; the two cycle-hub call-site
    // gates (g_orig_* are assigned before either gate is patched) then
    // always take their pass-through branch and invoke the saved
    // original id 40333 / id 40334. The engine therefore runs with its
    // original behaviour plus a harmless extra indirection, never a
    // half-applied deferral.
    bool Install();

}
