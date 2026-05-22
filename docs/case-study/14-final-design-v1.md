# 14 - Final Design and Outcome (WorkerSpinLockFix v1.0)

**Date:** 2026-05-21
**Status:** Implemented and validated. v1.0.0 shipped this design
on 2026-05-21. Superseded as the primary fix on 2026-05-22 by the
v2.0 structural fix (see [`22-v2-phase4-1-cycle-hub-characterisation.md`](22-v2-phase4-1-cycle-hub-characterisation.md)
and [`23-v2-release.md`](23-v2-release.md)). The architecture
documented here is **still installed** in v2.0.0 as defence-in-depth
beneath the structural fix - the runtime breaker remains the
backstop for cycle paths the structural fix does not preempt.
**Predecessor docs:**
- `06-root-cause.md` — AB-BA evidence between LockA and LockB.
- `11-worker-spinlockfix-retrospective.md` — failure modes of the
  function-entry serialisation attempts.
- `12-engine-fix-mod-audit.md` — `safetyhook` adoption rationale.
- `13-rethought-solution.md` — the redesign proposal that became
  this document's implementation.
**Successor docs:**
- `15-v2-structural-strategy.md` — strategy that drove the v2.0
  work.
- `16` through `22` — Phase 1 / 1.5 / 2 / 3 / 3.5 / 4-prep / 4.1
  outputs that produced the structural fix.
- `23-v2-release.md` — v2.0.0 release note and what changed in
  the plugin between v1.0.0 and v2.0.0.

This document records the v1.0 architecture as actually shipped,
the two regressions discovered during bring-up that are not in the
retrospective, and the synthetic test that proves the breaker works
end-to-end without waiting for a real engine cycle. The v1.0
architecture below is also one of the two layers shipped in v2.0.0
(the runtime-breaker layer); none of the §1-§7 content has been
revised post-v1.0 - the v2.0 structural fix layered on top is
documented separately in docs 22 and 23.

---

## 1. The architecture that shipped

A single `safetyhook::create_inline` hook on
`BSSpinLock::Acquire (id 12210)`. The detour applies a four-pointer
**surgical filter**: only the two engine BSSpinLocks involved in the
documented AB-BA (LockA at `+0x2eff8e0`, LockB at `+0x2f3b8e8`) — and
two optional test-mode locks — flow through the slow path. Every
other `BSSpinLock` pays one pointer compare and a tail-call.

For target locks the slow path:

1. Marks the current thread as waiting on the target in a fixed-size
   lock-free `WaitGraph` (64 cache-aligned slots).
2. Walks the wait-for chain via `target->owner` → owner's slot →
   `slot.waiting_on` → repeat. Bounded by 16 hops, writes the chain
   into a stack buffer.
3. Reports any chain that closes back to the current thread to
   `Breaker::OnCycleDetected`.
4. Falls through into the engine's normal spin loop via
   `safetyhook::InlineHook::unsafe_call`.

The breaker uses **time-based confirmation**:

- The first thread to detect a given signature claims the slot.
- It sleeps `confirmation_window_ms` (default 2 ms) outside the
  recent-cycle mutex.
- After the window, `WaitGraph::VerifyCycleStillPresent` re-reads
  every chain participant's `waiting_on`/`owner`. If anything has
  shifted, the breaker stands down.
- Otherwise the lock at `chain[0].waiting_on` has its `state` field
  CAS-released from `1` to `0`. The detector acquires it on its next
  spin iteration.

The thread that thought it owned the released lock continues to
execute its critical section without protection until it next
releases. This matches the engine's own release behaviour (which
also does not always clear `owner`) and is acceptable because the
alternative is a permanent freeze.

A `Reaper` module remains as an optional stale-owner backstop for
cases the entry-point hook cannot observe. It is disabled by default;
the hook + breaker pipeline is sufficient for the documented bug.

The full design is documented in
[`../../skyrim-freeze-fix/docs/design.md`](../../skyrim-freeze-fix/docs/design.md).

---

## 2. Two new lessons learned during bring-up

The retrospective doc 11 catalogued failures of function-entry
serialisation. The hook-based design hit two more lock-ordering
regressions that doc 11 does not cover. They are recorded here so
they are not repeated.

### 2.1. Heap allocation inside `BSSpinLock::Acquire`'s detour

The first hooking implementation used `std::vector::reserve` inside
the slow path to size a chain buffer. This created a new
`(heap CRITICAL_SECTION → BSSpinLock)` lock-order edge:

> Thread T1 contends on LockA → enters our detour → calls into the
> heap allocator → blocks on the heap `CRITICAL_SECTION`.
> Thread T2 holds the heap `CRITICAL_SECTION` (allocating something
> elsewhere) → tries to acquire LockA via some other engine path →
> spins waiting for T1.

Both threads deadlock through the heap rather than through LockB.

**Fix:** every chain buffer is stack-allocated (`std::array`) or
fixed-size static storage. No heap allocation can happen inside the
slow path.

### 2.2. `safetyhook::InlineHook::call<>` takes a recursive mutex

`safetyhook`'s `call<>` template is implemented as:

```cpp
template <typename ...Args>
auto call(Args&&... args) {
    std::scoped_lock lock(m_mutex);   // <-- std::recursive_mutex
    return reinterpret_cast<Fn*>(m_target)(std::forward<Args>(args)...);
}
```

The mutex is there to make install/uninstall thread-safe. With ~300
engine threads each routing every `BSSpinLock::Acquire` through the
hook, that mutex serialises every acquire across all threads. It
also creates a new `(SRWLock → BSSpinLock)` lock-order edge that is
sufficient to freeze early engine initialisation — captured in a
live process dump as `WorkerSpinLockFix!mtx_do_lock` →
`RtlpAcquireSRWLockExclusiveContended` while a thread held an engine
BSSpinLock.

**Fix:** every trampoline invocation in `HookedAcquire` uses
`safetyhook::InlineHook::unsafe_call` instead. That variant skips
the mutex and tail-calls the trampoline directly. The hook is never
uninstalled at runtime so we cannot race with installation or
destruction either.

These two fixes plus the lessons from doc 11 form the slow-path
invariants documented in `design.md` §4.

---

## 3. Time-based confirmation (the production gap fix)

A naive observation-counting approach (confirm once a signature has
been seen N times within a window) works for engine deadlocks where
4+ worker threads typically pile in on LockA/LockB and each generates
an independent slow-path entry. But it **fails for a clean 2-thread
AB-BA**: each thread enters `BSSpinLock::Acquire` exactly once and
then stays inside the engine's internal spin/`SleepEx` loop forever.
Only one observation arrives. The cycle is detected but never
confirmed and never broken.

This was discovered during the design of the synthetic test harness:
the test deliberately constructs the minimal 2-thread AB-BA and would
have hung silently with the old observation-counting flow.

The shipping breaker replaces observation-counting with **time-based
confirmation**: the first detector sleeps the configured window,
then re-runs `VerifyCycleStillPresent`. This works for any cycle
size including the 2-thread minimum. Concurrent observers of the
same signature still feed the observation counter for diagnostics
but do not run the confirmation flow themselves; only the first
claimer can break the cycle.

This is a real production fix, not just a test enabler. A 2-thread
AB-BA in vanilla Skyrim would have been detected but never broken
under observation-counting; it is broken under time-based
confirmation.

---

## 4. Synthetic AB-BA validation

The plugin ships an optional `[test_mode]` section in the TOML.
When enabled, after `kDataLoaded` the plugin spawns two threads that
deliberately AB-BA two heap-allocated test BSSpinLocks via the real
`BSSpinLock::Acquire (id 12210)`. The test locks are registered with
`AcquireHook::AddTestLocks` so they flow through the surgical filter
exactly like the engine LockA/LockB.

The test does **not** touch the engine's BSSpinLocks. It allocates
its own pair of `BSSpinLock` objects and only the breaker writes
into those two pointers; engine state is never modified.

A 10-second coordinator timeout manually clears the test locks if
the breaker fails to fire, so the worker threads drain instead of
spinning forever.

### 4.1 First successful run

```
[TEST] starting synthetic AB-BA validation. test_lockA=0x7fff354ce600, test_lockB=0x7fff354ce640, timeout=10000ms.
[TEST] worker B (TID 23700) acquiring test_lockB (0x7fff354ce640).
[TEST] worker A (TID 27376) acquiring test_lockA (0x7fff354ce600).
[TEST] worker A (TID 27376) acquiring test_lockB (this is the AB half; will spin until breaker fires).
[TEST] worker B (TID 23700) acquiring test_lockA (this is the BA half; will spin until breaker fires).
[CYCLE] first observation (length=2, age=0ms, observations=1):
[CYCLE]   TID 23700 waits on lock 0x7fff354ce600 (owner TID 27376)
[CYCLE]   TID 27376 waits on lock 0x7fff354ce640 (owner TID 23700)
[CYCLE] confirmed (will break) (length=2, age=0ms, observations=1):
[CYCLE]   TID 23700 waits on lock 0x7fff354ce600 (owner TID 27376)
[CYCLE]   TID 27376 waits on lock 0x7fff354ce640 (owner TID 23700)
[BREAK] force-released BSSpinLock 0x7fff354ce600 (observed owner TID 27376, state 1->0). Detector TID 23700 should now acquire on next spin.
[TEST] worker B (TID 23700) completed; both test locks released.
[TEST] worker A (TID 27376) completed; both test locks released.
[TEST] SUCCESS - both workers completed. The breaker detected the synthetic AB-BA, confirmed it via the time-based flow, and force-released one test lock. End-to-end cycle break is proven.
```

Stats line one minute later confirmed exactly one detection, one
confirmation, one successful CAS, and zero races:

```
stats: acq_slow=407 cycles_observed=1 cycles_confirmed=1
       breaks_done=1 breaks_raced=0 breaks_suppressed=0
```

The harness exists primarily so future regressions in the breaker
pipeline can be caught immediately on the first launch without
waiting for a real engine cycle to fire. Keep it disabled for normal
play.

---

## 5. What v1.0 proves

1. **The hook routes calls correctly.** Every contended LockA/LockB
   acquire enters the slow path; every other `BSSpinLock` bypasses
   it.
2. **`WaitGraph::WouldFormCycle` correctly detects 2-thread
   AB-BA.** Verified end-to-end via the synthetic test.
3. **Time-based confirmation works for any cycle size including the
   2-thread minimum.** A real production gap that observation-
   counting could not have closed.
4. **`VerifyCycleStillPresent` correctly distinguishes a real
   stuck cycle from a transient near-cycle.** Confirmed by the
   `breaks_raced=0` counter under stress.
5. **`InterlockedCompareExchange` force-release frees the spinning
   detector.** The detecting thread re-enters its trampoline spin
   loop after the break and acquires on the next iteration.
6. **The released-but-still-believed-owned thread continues to run
   without crashing.** The engine's invariants on the released
   critical section tolerate brief unguarded execution. No save
   corruption observed.
7. **No detectable steady-state cost.** Under 16 minutes of active
   gameplay the surgical filter handled ~3,500 contended LockA/LockB
   acquires with no perceptible lag (compared to the v0.15-style
   reaper-only baseline that produced visible script-tick lag at
   1 Hz).

This is the strongest evidence available short of waiting for a
real production cycle to fire — which we cannot force, only soak
for.

---

## 6. What v1.0 does NOT prove

The synthetic test exercises the exact same code path a real
engine cycle would take through `HookedAcquire`, but it does not
stress some adjacent considerations:

- **Multiple concurrent cycles** with disjoint signatures. The
  recent-cycle map has 32 slots and an LRU eviction policy; that
  has been built but never exercised by more than one cycle at a
  time.
- **CAS race outcomes.** The synthetic test's CAS always wins on
  the first try because the cycle has been stable for the entire
  confirmation window. Under genuine engine load the CAS could
  race with the engine's own release and lose; that path is
  exercised by the `breaks_raced` counter and the log path but
  has not been observed in practice.
- **The reaper's interaction with the hook.** Each was tested in
  isolation. With both enabled, the reaper's stack-pattern scan
  must not interfere with a thread the breaker is already operating
  on. The two operate on disjoint failure modes (live cycle vs
  stale owner) so this should compose, but the test plan for the
  composed case is a soak rather than a synthetic harness.

These are residual unknowns, not failure modes. They are listed for
completeness in case a future regression turns out to live in one
of these gaps.

---

## 7. Where the work continues

The reactive track now has a working v1.0. The structural research
track (replacing the locked engine data structure with a lock-free
equivalent, along the lines of `form_caching`) was scoped in doc 13
§7 and remains in reserve as a possible v2.0. It is no longer on the
critical path: v1.0 already keeps the game running through any
LockA/LockB cycle, and the structural fix would be an optimisation
rather than a correctness fix.
