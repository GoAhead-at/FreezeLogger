# WorkerSpinLockFix - design

This document describes the architecture of the `WorkerSpinLockFix`
SKSE plugin: what each module does, why it is shaped that way, and the
invariants that hold across the whole system.

> **v2.3.0 architecture change.** The plugin now ships **two
> independent fixes for two *different* freeze classes**, and the
> earlier v1.0 runtime breaker stack has been removed. Sections
> §2.2 (runtime breaker), §3.2-§3.4 (`AcquireHook` / `WaitGraph` /
> `Breaker`), §3.5 (`Reaper`), and §3.6 (`TestMode`) below describe
> code that **no longer exists in the plugin**; they are retained for
> historical reference only. The reason for the removal: field
> telemetry showed `Phase4Defer` prevents the AB-BA cycle outright
> (`cycles_observed=0` across long sessions), so the runtime breaker
> never fired and was pure redundancy. Its reaper backstop (only ever
> useful for a *dead-owner* orphaned lock, a case that does not occur
> for this live-thread AB-BA bug) and the synthetic test harness went
> with it.

The plugin's two current layers:

1. **Layer 1 - AB-BA spinlock prevention (`Phase4Defer`).** One inline
   hook on the LockA acquirer (`id 19369`) plus two surgical
   call-site patches inside the cycle hub (`id 36016+0xdcb` ->
   `id 40334`, `id 19372+0x606` -> `id 40333`) defer the LockB
   acquirers whenever the current thread is inside `id 19369`.
   The deferred calls drain on the same thread when LockA is
   released. The AB-BA cycle cannot form. The function entries
   of `id 40333` / `id 40334` are left pristine so other mods
   that hook those functions cooperate with this plugin. This is now
   the sole fix for the AB-BA inversion (see §2.1, §3.1).
2. **Layer 2 - WaitForJobTask stuck-job recovery (`JobWaitBreaker`,
   new in v2.3.0).** A *separate* engine freeze class (case-study
   27/28): `Main::Update` parks in `WaitForJobTask` on a job-completion
   event that no producer will ever signal, so main sleeps forever. Two
   field-observed shapes: the job entry is torn down to null
   (`freeze_2026-06-15_160413`), or it stays in place but makes zero
   progress (`freeze_2026-06-15_204830`). An inline wrap on
   `WaitForJobTask` (located by a version-independent `.text` body
   signature, with the Singleton-B job slot derived from its
   rip-relative load) tracks when main is parked and derives the exact
   wait handle from the job-pool chain at wait entry; a watchdog detects
   the stuck signature (parked past a dwell threshold with the job
   making no progress across a confirmation window) and, in active mode,
   delivers the missing signal via `SetEvent`. No process-wide hook is
   used. See §3.7 and
   [`../../docs/case-study/28-jobwaitbreaker-design.md`](../../docs/case-study/28-jobwaitbreaker-design.md).
3. **Layer 3 - Site-A worker-ack deadlock recovery (`SiteABreaker`,
   new in v2.4.0).** A *third* engine freeze class (case-study 29):
   `Main::Update` has a second infinite-wait site, the `id 34554` helper
   (reached via `id 35565`, driven by Faster HDT-SMP's per-frame parallel
   dispatch). It publishes work into Singleton-A (`pending=1`, wakes a
   worker via the auto-reset worker-wake event) then blocks `INFINITE` on
   the manual-reset worker-ack event `[+0x60]` for the worker to signal
   completion. When a worker consumes the wake but never signals the ack,
   main sleeps forever. Observed on HDT-SMP 3.1.0.0 *and* 3.2.0.0
   (`freeze_2026-06-16_133223`, `freeze_2026-06-16_152328`), so it is
   independent of the HDT-SMP version. An inline wrap on `id 34554`
   (located by an independent `.text` body signature, with the Singleton-A
   slot derived from its rip-relative load) marks when main entered a
   Site-A wait and captures the worker-ack handle at entry; a watchdog
   confirms the deadlock (parked in the same episode past a dwell
   threshold with `pending=1`, work-id + ack handle unchanged, and the ack
   event still unsignalled across a confirmation window) and, in active
   mode, delivers the missing signal via `SetEvent`. See §3.8 and
   [`../../docs/case-study/29-siteabreaker-plan.md`](../../docs/case-study/29-siteabreaker-plan.md).

The three layers are fully independent: they target different bugs, hook
different functions, and share no state.

For the engine bug being fixed see
[`../../docs/case-study/06-root-cause.md`](../../docs/case-study/06-root-cause.md).
For the engineering lessons that drove the v1.0 architecture (in
particular the constraints on what may and may not happen inside
the `BSSpinLock::Acquire` slow path) see
[`../../docs/case-study/11-worker-spinlockfix-retrospective.md`](../../docs/case-study/11-worker-spinlockfix-retrospective.md).
For the v2.0 cycle-hub characterisation that produced the
structural fix see
[`../../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md`](../../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md).

---

## 1. Bug recap

Skyrim SE 1.5.97 contains a vanilla AB-BA inversion between two
static `BSSpinLock` globals in the worker dispatcher:

- `LockA` at `SkyrimSE+0x2eff8e0`, taken inside `id 19369` (the
  LockA acquirer).
- `LockB` at `SkyrimSE+0x2f3b8e8`, taken inside three non-virtual
  `RE::ProcessLists` methods:
  - `id 40285` - `TransferBetweenTempChangeLists`-style traverser
    that dispatches into form code at six vtable slots.
  - `id 40333` - `AddToTempChangeList`. Acquires LockB, appends
    the actor to a bucket array, sets `kInTempChangeList` (bit 9)
    of `Actor::boolBits` at `[actor+0xe0]`.
  - `id 40334` - `RemoveFromTempChangeList`. Acquires LockB, finds
    and removes the actor, clears `kInTempChangeList`, clears the
    process-private global at `0x2f44db0`.

Phase 1 misclassified `id 40706` as a LockB acquirer; Phase 1.5
(see [`../../docs/case-study/17-v2-phase1-5-findings.md`](../../docs/case-study/17-v2-phase1-5-findings.md))
established that `id 40706` takes a per-instance lock at
`[obj+0x150]` of a different class. Phase 4 prep (see
[`../../docs/case-study/21-v2-phase4-prep-dispatch-decode.md`](../../docs/case-study/21-v2-phase4-prep-dispatch-decode.md))
reclassified `id 40335` as `BSSpinLock::Unlock` for LockB, not an
acquirer. The acquirer set is exactly `{id 40285, id 40333,
id 40334}`.

Two worker threads can each hold one lock and spin on the other.
Once both threads are caught in the cycle, neither makes progress,
the worker-ack event the main thread is waiting on is never
signalled, and the game freezes. Full evidence with stack traces is
in `06-root-cause.md`.

The race is rare per-session but cumulative on long play. It was
reproduced in nine independent freeze captures with `FreezeLogger`,
all with the same two-lock topology.

---

## 2. Solution shape

### 2.1 Layer 1 - Structural fix (Phase4Defer)

`Phase4Defer` installs one `safetyhook::create_inline` hook on the
LockA acquirer plus two `Trampoline::write_call<5>` patches at
specific call sites inside the cycle hub:

```
                 thread enters id 19369 (LockA acquirer)
                 ─────────────────────────────────────────
                  ++tl_lockA_depth
                  bool result =
                      unsafe_call<bool>(original)   // returns bool!
                  --tl_lockA_depth
                  if (tl_lockA_depth == 0) DrainDeferred()
                  return result               // propagate to caller


                 thread reaches id 36016+0xdcb  (call to id 40334)
                 thread reaches id 19372+0x606  (call to id 40333)
                 (the two call sites are the ONLY paths in the
                  binary through which id 40333 / id 40334 are
                  reached while LockA is held; verified during
                  Phase 4.1 cycle-hub characterisation)
                 ─────────────────────────────────────────
                  if (tl_lockA_depth > 0) {
                      // Defensive synchronous half:
                      // toggle kInTempChangeList atomically so any
                      // reader of bit 9 inside the LockA scope sees
                      // the post-call state. Idempotent w.r.t. the
                      // drain's later call to the original.
                      actor->boolBits.fetch_or  / fetch_and(mask)

                      // Deferred half:
                      tl_deferred.push_back({kind, pl, actor});
                      return;            // skip LockB acquire
                  }
                  // pass-through: call the unmodified entry of
                  // id 40333 / id 40334 directly. Other mods that
                  // hook those functions still see this call.
                  g_orig_id4033X(pl, actor);
```

Per-thread state is a single `int tl_lockA_depth` and a small
`std::vector<DeferredCall> tl_deferred` reserved up-front so the hot
path never allocates.

**Why call-site patches and not function-wraps.** v2.0.1's first
cut wrapped the entries of `id 40333` and `id 40334` as full
function inline hooks. That worked but had two costs: (1) every
engine call into either function paid the gate's TLS load, even
though the deferral path only ever fires on the cycle path; (2)
any other mod that hooks `id 40333` or `id 40334` competes with
our hook for the prologue. The call-site refactor narrows the
gate to exactly the two call instructions inside the cycle hub
that reach `id 40333` / `id 40334` while LockA is held; the
function entries stay pristine, other mods cooperate, and the
hot-path overhead drops by orders of magnitude. See
[`../../docs/case-study/25-v2-0-1-callsite-refactor.md`](../../docs/case-study/25-v2-0-1-callsite-refactor.md)
for the full rationale and the comparison with GarrixWong's
`skyrim-freeze-fix` that motivated the change.

Pre-patch the install path verifies each 5-byte CALL site
contains `E8` and a rel32 pointing at the expected
address-library entry of `id 40333` / `id 40334`. Mismatch
triggers a clean abort: the plugin downgrades to v1.0
runtime-breaker mode and logs the actual bytes so the
disagreement is visible. This is how we cooperate with another
mod that has already redirected the same call site.

The drain runs on the same thread that originally queued each call,
so per-thread call ordering is preserved. The drain runs after
LockA is released, so each replayed `id 40333` / `id 40334` call
takes LockB normally. The AB-BA cycle - which required *holding*
LockA *while* taking LockB - is structurally impossible.

The LB->LA direction (`id 40285` -> `id 36614` -> `id 38413` ->
`id 19369`) is intentionally left untouched. Once the LA->LB edge
is broken, the cycle cannot close in either direction.

**Wrap signature matters.** `id 19369` is **not** a 4-arg
function. The body reads stack args 5 and 6 at `[rbp+0x77]`
(dword) and `[rbp+0x7f]` (byte) at six call sites. The function
also returns `bool` (`movzx eax, bl; ret`). The current wrap
matches all of that:

```cpp
bool __fastcall HookedLockAAcquirer(
    void* rcx, void* rdx, std::uint8_t r8b, std::uintptr_t r9,
    std::uint32_t stack_arg5, std::uint8_t stack_arg6);
```

with `unsafe_call<bool>(...all 6 args...)` to forward verbatim
through the trampoline.

v2.0.0 declared this with only 4 register args and a `void`
return. Three independent failure modes followed:

1. The trampoline read garbage for stack args 5 and 6 every
   invocation, so `id 19369` ran with corrupted arguments.
2. The trampoline's `bool` result was discarded; callers
   reading `eax` after the wrap saw whatever
   `--tl_lockA_depth` left there.
3. Doc 22 §4's audit didn't catch either: it focused only on
   call-site followup state, not on the wrap's calling
   convention.

The 6-arg `bool`-returning wrap fixes all three. The
synchronous `kInTempChangeList` bit toggle is kept as
defensive scaffolding (idempotent w.r.t. the drain; eliminates
a stale-flag window for any future intra-LockA-scope reader of
bit 9; cost is one atomic op per gate hit, no lock contention).

The corrected audit methodology is documented in doc 24 §6:
**read the full prologue AND epilogue AND every `[rbp+offset]`
access in the body before declaring any wrap.** Full
retrospective in
`../../docs/case-study/24-v2-0-1-skyshard-regression-fix.md`.

### 2.2 Layer 2 - Runtime breaker

The plugin retains v1.0's **detect-and-break at the spinlock level**.
Since **v2.1.0** the detector is a `safetyhook` **mid-hook at
`BSSpinLock::Acquire +0x8a`** (id 12210) rather than an inline hook on
the function entry. `+0x8a` is the retry point inside the engine's
*outer* backoff spin loop, immediately after its yield call and just
before the retry CAS:

```
                                  ┌──────────────┐
                                  │ BSSpinLock   │
                                  │ ::Acquire    │   uncontended / recursive /
                                  │ (id 12210)   │   inner-pause-spin success
                                  └──────┬───────┘   ── all return NATIVE,
                                         │              never reaching +0x8a
                          outer backoff loop (genuinely stuck)
                                         │ mid-hook @ +0x8a
                                         ▼
              ┌─────────────────── OnSpinRetry(ctx) ──────────────────┐
              │  self = ctx.rdi   me = GetCurrentThreadId()           │
              │                                                       │
              │  surgical filter:                                     │
              │    self == LockA/LockB (or test locks)? ── no ──→ return
              │       │                                               │
              │      yes                                              │
              │       ▼                                               │
              │  WaitGraph::EnterSlow(me, self)   // publish+refresh   │
              │  WaitGraph::WouldFormCycle(me, self, …)               │
              │       │                                               │
              │       └── chain >= 2 ───▶ Breaker::OnCycleDetected    │
              │                                                       │
              │  (return; engine's own spin loop continues natively)  │
              └───────────────────────────────────────────────────────┘
```

Why the mid-hook: the old entry-point inline hook routed **every**
`BSSpinLock::Acquire` (across ~300 engine threads, millions of calls
per second) through a detour + filter + trampoline tail-call. That
fixed per-acquire cost was invisible at a 60 fps cap but became
measurable when stacked under framerate-amplifying mods (e.g.
PureDark's upscaler) that multiply acquire throughput and push the
build CPU-bound. `+0x8a` is reached **only** by a thread that has
exhausted the inner pause spin and returned from a yield -- the
genuinely-stuck population -- so uncontended and recursive acquires
now run **fully native with zero added cost**.

Lifecycle without a bracket: a mid-hook has no "stopped waiting"
callback (a thread that finally acquires the lock simply stops hitting
`+0x8a`). So `WaitGraph` edges are **self-expiring**: `EnterSlow`
stamps a refresh time on every backoff iteration, and all readers
(`WouldFormCycle`, `VerifyCycleStillPresent`, `SnapshotEdges`) honour
an edge only within `kEdgeStaleMs` (50 ms) of its last refresh. To keep
this from ever causing a *spurious* break, the breaker's
`confirmation_window_ms` is raised to **120 ms (> the staleness
window)**: any participant that stops refreshing during confirmation
(it acquired the lock, or the "cycle" was a phantom from a lingering
stale edge) has its edge expire before `VerifyCycleStillPresent` runs,
so the break is suppressed. A real deadlock keeps every edge fresh and
breaks ~120 ms after detection.

We did not pursue function-entry serialisation (gating the engine
functions that take LockA/LockB behind a plugin mutex) because every
attempt at that produced a new lock-ordering deadlock against the
heap `CRITICAL_SECTION` or against the engine's own `BSSpinLock`s -
see `11-worker-spinlockfix-retrospective.md` for the full set of
lessons.

The Layer 2 design is **invisible in normal play** (the hook fires only
inside the backoff loop of one of the two watched locks) and acts only
when a real AB-BA cycle is observed, confirmed, and re-verified. The
Layer 1 design is similarly cheap: one thread-local read per `id 40333`
/ `id 40334` entry on the common (pass-through) path, plus one
increment/decrement per `id 19369` entry/exit.

---

## 3. Module breakdown

### 3.1. `Phase4Defer` (v2.0 structural fix)

The Layer 1 module. One `safetyhook::create_inline` hook on the
LockA acquirer plus two `Trampoline::write_call<5>` patches at
the cycle-hub call sites, plus a pair of thread-local globals.
RVAs are resolved at install time via
`REL::Relocation<>{REL::ID(N)}`, the same pattern `AcquireHook`
uses.

```cpp
thread_local int tl_lockA_depth = 0;
thread_local std::vector<DeferredCall> tl_deferred;   // reserved 8

// Wrap of id 19369. 6 args matching the engine signature
// (rcx, rdx, r8b, r9 + dword stack arg5 + byte stack arg6).
// Returns bool. unsafe_call<bool> forwards all 6 args to the
// trampoline and propagates the bool result back to the caller.
bool __fastcall HookedLockAAcquirer(
    void* rcx, void* rdx, std::uint8_t r8b, std::uintptr_t r9,
    std::uint32_t stack_arg5, std::uint8_t stack_arg6)
{
    ++tl_lockA_depth;
    const bool result = g_hook_lockA_acquirer.unsafe_call<bool>(
        rcx, rdx, r8b, r9, stack_arg5, stack_arg6);
    if (--tl_lockA_depth == 0) DrainDeferredOnExit();
    return result;
}

// Call-site gate at id 36016+0xdcb (replaces direct call id 40334).
// Synchronous half (added in v2.0.1): toggle kInTempChangeList
// atomically inside the gate. Single fetch_and on the actor's own
// boolBits word; no LockB needed. Downstream readers observe the
// new state immediately. Idempotent w.r.t. the drain's later call
// to the original.
void __fastcall HookedRemoveAtCycleHub(void* pl, void* actor) {
    if (tl_lockA_depth > 0) {
        if (actor) {
            BoolBitsAtomic(actor)->fetch_and(
                ~kInTempChangeListMask,
                std::memory_order_acq_rel);
        }
        tl_deferred.push_back({DeferKind::kRemove, pl, actor});
        Stats::OnPhase4Queued();
        return;
    }
    Stats::OnPhase4PassThrough();
    g_orig_id40334(pl, actor);   // unmodified function entry
}

// Call-site gate at id 19372+0x606 (replaces inner call id 40333).
void __fastcall HookedAddInsideAddWrapper(void* pl, void* actor) {
    if (tl_lockA_depth > 0) {
        if (actor) {
            BoolBitsAtomic(actor)->fetch_or(
                kInTempChangeListMask,
                std::memory_order_acq_rel);
        }
        tl_deferred.push_back({DeferKind::kAdd, pl, actor});
        Stats::OnPhase4Queued();
        return;
    }
    Stats::OnPhase4PassThrough();
    g_orig_id40333(pl, actor);   // unmodified function entry
}
```

`HookedLockAAcquirer`'s 6-argument `__fastcall bool` signature
matches `id 19369`'s observed prologue and epilogue. The wrap
forwards every register and stack arg verbatim through the
trampoline and propagates the function's `bool` return back to
its caller (essential for scripted-animation activators -- see
doc 24).

`g_orig_id40333` and `g_orig_id40334` are plain function pointers
to the unmodified entry points (resolved via address library at
install time). The call-site patches rewrite only the 5-byte CALL
inside the cycle hub; the function bodies and prologues are not
touched.

`DrainDeferredOnExit` walks `tl_deferred` to-completion replaying
each `(kind, pl, actor)` triple via `g_orig_id40333` /
`g_orig_id40334`. The drain happens on the same thread as the
original queueing, so per-thread call ordering is preserved.
Because the drain calls the unmodified function entries, any
other mod's inline hook on those functions runs during the
drain too.

The reserve-up-front policy on `tl_deferred` (8 entries by default,
which is well above any observed in-cycle queue depth) keeps the
hot path allocation-free. If the queue grows past the reserve the
underlying `std::vector` reallocates - this is observable in the
debugger via heap activity but does not produce a hot-path
allocation under normal load.

### 3.2. `AcquireHook`

Installs a single `safetyhook::create_mid` hook at
`BSSpinLock::Acquire +0x8a` (`id 12210`). `+0x8a` was fixed by
disassembling the function (see `analysis/dump_acquire.py`): it is the
instruction after the engine's backoff-yield call and before the retry
CAS, inside the *outer* spin loop. The uncontended fast path, the
recursive re-entry path, and the inner `_mm_pause` spin all return
before `+0x8a`, so **only a genuinely-stuck thread reaches the hook**.

The callback reads `self` from `ctx.rdi` (the lock pointer, callee-saved
for the whole function) and `me` from `GetCurrentThreadId()`, then
applies a four-pointer **surgical filter**:

```cpp
void OnSpinRetry(SafetyHookContext& ctx) {
    auto* self = reinterpret_cast<WaitGraph::Lock*>(ctx.rdi);
    if (self != g_lockA && self != g_lockB &&
        self != g_test_lockA && self != g_test_lockB) {
        return;                              // not a watched lock
    }
    const DWORD me = ::GetCurrentThreadId();
    if (WaitGraph::EnterSlow(me, self)) {    // publish + refresh edge
        Stats::OnAcquireSlow();              // count distinct episodes
    }
    // ... WouldFormCycle -> Breaker::OnCycleDetected if chain >= 2
}
```

`g_lockA` and `g_lockB` are resolved at install time from the
SkyrimSE.exe module base + the documented RVAs. `g_test_lockA` and
`g_test_lockB` are normally `nullptr` (so they always compare unequal
to any real BSSpinLock pointer); they only become non-null when the
optional test mode is enabled.

Key properties of the mid-hook approach:

- **No trampoline.** A mid-hook is observational: after the callback
  returns, `safetyhook` re-executes the displaced instructions and the
  engine's own spin loop continues. The breaker unsticks a confirmed
  cycle by CASing the lock's `state` `1->0`, which lets the engine's
  own retry CAS at `+0x8c` succeed. There is no `unsafe_call`/`call<>`
  trampoline and therefore none of the per-acquire serialisation the
  old entry-point hook had to reason about.
- **Zero cost off the contended path.** Acquires that do not seriously
  contend never reach `+0x8a`, so the common case runs entirely native.
- **`state` (`+0x4`) is authoritative for "held"** (`0` = free,
  `1` = held); `owner` (`+0x0`) is not, because the engine does not
  always clear it on release.
- **Lock-order safety.** At `+0x8a` the thread holds no BSSpinLock (it
  is trying to acquire one), so the SRWLock/sleep taken inside
  `Breaker::OnCycleDetected` (only when a cycle is found) cannot create
  a `BSSpinLock -> SRWLock` edge.

### 3.3. `WaitGraph`

A fixed-size, lock-free wait-for graph. Each thread that enters the
slow path claims one of 64 cache-aligned slots (lazily, from a
thread-local cache) and writes its `waiting_on` pointer plus a refresh
timestamp into that slot:

```cpp
struct alignas(64) ThreadSlot {
    std::atomic<DWORD>          tid;
    std::atomic<Lock*>          waiting_on;
    std::atomic<std::uint64_t>  last_seen_ms;   // self-expiry (v2.1.0)
};

std::array<ThreadSlot, 64> g_slots;
```

Because the spin-retry mid-hook has no "stopped waiting" callback,
`EnterSlow` re-stamps `last_seen_ms` on every backoff iteration and all
readers ignore an edge older than `kEdgeStaleMs` (50 ms). A thread that
acquires its lock stops refreshing and drops out of the graph within
that window; a still-spinning thread stays visible.

`WouldFormCycle` walks the wait-for chain by reading `target->owner`,
looking up that owner's slot, reading `slot.waiting_on`, and
repeating. The walk is bounded by `kMaxHops` (16) and writes the
chain into a caller-provided buffer so the slow path never allocates.

`VerifyCycleStillPresent` re-reads the same fields after the
confirmation window to validate the cycle has not self-resolved.

The graph is invisible to threads that never enter the slow path –
fast-path acquires never touch any of this storage.

### 3.4. `Breaker`

Drives the confirmation gate and (when enabled) the force-release.

Cycle signatures are sorted `(waiter, lock_addr)` pairs, stable
across whichever thread happened to detect the cycle. A small
fixed-size map (32 slots, mutex-protected) deduplicates concurrent
observations of the same cycle.

The confirmation flow is **time-based**:

1. The first thread to detect a given signature claims the slot
   (`breaker_claimed = true`).
2. The claimer sleeps `confirmation_window_ms` outside the recent-
   cycle mutex. Concurrent observers can still bump observation
   counters during the sleep but never claim the breaker role.
3. After the window the claimer calls
   `WaitGraph::VerifyCycleStillPresent`. If the cycle has resolved
   the breaker stands down (`breaks_raced` increments).
4. Otherwise the lock at `chain[0].waiting_on` (the lock the
   detector was about to spin on) has its `state` field force-
   released via `InterlockedCompareExchange(state, 0, 1)`. CAS
   races (state was no longer `1`) are counted as `breaks_raced`,
   not `breaks_done`.

Time-based confirmation works for any cycle topology including a
clean 2-thread AB-BA, where each thread enters
`BSSpinLock::Acquire` exactly once and then stays inside the
trampoline's spin loop. Observation counting alone could not
confirm such a cycle because only one observation ever arrives.

### 3.5. `Reaper`

Optional stale-owner backstop. Disabled by default. v2.0.3
redesign. Periodically:

- Calls `WaitGraph::SnapshotEdges` to copy the currently-active
  `(waiter_tid, waiting_on)` pairs into a 64-slot stack buffer.
- For each edge, dereferences `waiting_on` and reads the
  `BSSpinLock`'s `(owner, state)` pair via SEH-guarded loads.
- Filters out transient races (`state == 0`, `owner == 0`,
  `owner == waiter`) and folds the remaining edges into a moving
  window keyed on `(waiter, lock, owner)`.
- For any edge stable for at least `kStaleStableMs` (2 s) whose
  owner thread has died (`OpenThread + GetExitCodeThread != STILL_ACTIVE`),
  force-releases the lock via `CAS(state -> 0)`.
- For any edge stable for at least `kLiveProbeMs` (5 s) whose
  owner is still alive, emits a `LIVE-OWNER WAIT` diagnostic line
  every `kLiveProbeRepeat` (30 s).

The reaper does no live-cycle detection; that is the
`AcquireHook + Breaker` path's job. It exists for cases the entry-
point hook cannot observe at runtime: threads that died holding a
lock between the slow-path entry and the cycle check.

**No `SuspendThread`, `GetThreadContext`, `ResumeThread`,
`Toolhelp32` enumeration, register scanning, or stack scanning on
the runtime path.** The pre-v2.0.3 design used all of those to
discover BSSpinLock candidates; that design was retired because
`GetThreadContext` is not bounded under adversarial kernel states
and every safe mitigation we considered carried hazards strictly
worse than the hypothetical hang it would mitigate. The full
trade-off analysis is in
`docs/case-study/26-reaper-snapshot-removed.md`.

Coverage trade-off: the reaper now only sees threads that
traversed the AcquireHook slow path (i.e. threads whose `id 12210`
call hit `state != 0`). Phase 1.5 confirmed all six known
acquirers go through `id 12210`, so against the bug this plugin
targets the loss of coverage is theoretical. Practically, the
reaper requires `[acquire_hook] enabled = true` to do anything;
with both disabled the plugin is effectively idle.

### 3.6. `TestMode`

Optional synthetic AB-BA validation harness. Disabled by default.
When enabled, after `kDataLoaded` the plugin spawns two threads that
deliberately AB-BA two heap-allocated test `BSSpinLock`s via the
real `BSSpinLock::Acquire (id 12210)`. The two test locks are
registered with `AcquireHook::AddTestLocks` so they flow through the
surgical filter exactly like the engine LockA/LockB.

If the breaker pipeline is healthy, both threads complete and the
log emits `[TEST] SUCCESS`. If anything fails, a 10-second
coordinator timeout manually clears the test locks so the workers
drain, and the log emits `[TEST] FAILURE`.

The harness exists primarily so future regressions in the breaker
can be caught immediately on the first launch without waiting for a
real engine cycle to fire (which is rare and timing-dependent).

### 3.7. `SkyrimAnchors` + `JobWaitBreaker` (v2.3.0, current)

The Layer 2 modules. They target the **second** freeze class -- the
Skyrim main-thread `WaitForJobTask` lost-wakeup hang -- which is
unrelated to the AB-BA spinlock bug Layer 1 prevents. Full design and
evidence: [`../../docs/case-study/28-jobwaitbreaker-design.md`](../../docs/case-study/28-jobwaitbreaker-design.md).

**`SkyrimAnchors`** resolves two engine anchors at load, version-
independently (SE 1.5.97 / AE 1.6.x / VR), with no Address Library
dependency: it scans the loaded module's `.text` for the 15-byte
`WaitForJobTask` body signature and derives the Singleton-B job-pool
slot from the `mov rax,[rip+disp]` prologue immediately above it.
Ported from the FreezeLogger diagnostics plugin, where the same scan
drives the Site-B freeze probe. If the signature is not found,
`Available()` stays false and `JobWaitBreaker` does not arm.

**`JobWaitBreaker`** has three pieces:

1. **`WaitForJobTask` inline wrap.** On the main thread (pinned on
   first call), the outermost entry records "in a job-wait since `T`,
   on job index `N`", bumps an episode sequence, and derives the exact
   wait handle from the job-pool chain
   (`*(singleton) -> [+8] -> [idx0*8] -> [0] -> [idx1*8]`, SEH-guarded)
   while the chain is still intact; the matching exit clears the flag. A
   `thread_local` depth counter keeps nested calls in one episode. The
   wrap returns the original's value (declared `uintptr_t`, a safe
   superset). Capturing the handle here (rather than via a process-wide
   `WaitForSingleObjectEx` hook) is what makes the torn-down variant
   recoverable -- the handle is saved before any teardown -- and removes
   all global hot-path cost.
2. **Watchdog thread.** Polls at `poll_interval_ms`. When main has been
   parked longer than `dwell_threshold_ms`, it snapshots the job entry
   main is waiting on (`*(singleton) -> [+8] -> [idx0*8]`, then its 8
   header qwords, SEH-guarded), waits `recheck_window_ms`, and
   re-samples. It acts only if the job made **zero progress** across the
   window: either the chain stayed torn down to null
   (`freeze_2026-06-15_160413`), or the entry's bytes are byte-for-byte
   identical (`freeze_2026-06-15_204830`). A long job that is *still
   progressing* mutates its counters within the window, changes the
   fingerprint, and is left alone.
3. **Release (active mode only).** `SetEvent` on the handle derived in
   step 1. Because the gate requires the job to be provably making no
   progress (and no producer is coming), waking main is equivalent to
   the signal that was lost.

Defaults: `enabled=true`, `dwell_threshold_ms=5000`,
`recheck_window_ms=1500` (the main conservatism knob -- a job that
changes within it is treated as live). The module never suspends an
engine thread or reads a thread context -- a deliberate contrast with
the retired reaper (§3.5), and it installs no process-wide hook.

### 3.8. `SiteABreaker` (v2.4.0, current)

The Layer 3 module. It targets the **third** freeze class -- the Skyrim
main-thread **Site-A worker-ack deadlock** (`id 34554` / Singleton-A) --
distinct from both the AB-BA spinlock bug (Layer 1) and the
`WaitForJobTask` lost-wakeup hang (Layer 2). Full design and evidence:
[`../../docs/case-study/29-siteabreaker-plan.md`](../../docs/case-study/29-siteabreaker-plan.md).

**Anchoring.** `SkyrimAnchors` gained a second, independent body-signature
scan for the `id 34554` helper: it matches the distinctive
`cmp [rbx+0x6c],1` / `mov rcx,[rbx+0x60]` / `mov [rbx+0x68],0` body,
derives the Singleton-A slot from the `mov rbx,[rip+disp]` load in the 7
bytes immediately before it, and walks the `push rbx; sub rsp,0x20`
prologue backward to find the function entry. It is resolved separately
from the Site-B anchor, so either can arm without the other
(`AvailableSiteA()` gates this module).

**`SiteABreaker`** has three pieces, mirroring `JobWaitBreaker`:

1. **`id 34554` inline wrap.** On the main thread (pinned on first call),
   the outermost entry reads Singleton-A (SEH-guarded) and -- only if a
   wait is actually scheduled (`pending [+0x6c]==1` with a non-null
   worker-ack handle `[+0x60]`) -- opens an episode: it captures the ack
   handle and work-id, records the start time, bumps an episode sequence,
   and sets the in-wait flag. The matching exit clears the flag. A
   `thread_local` depth counter keeps nested calls in one episode, and the
   first four integer registers + the return value are forwarded verbatim
   so the wrap is transparent. `unsafe_call` (no mutex) is used because the
   helper blocks `INFINITE` inside.
2. **Watchdog thread.** Polls at `poll_interval_ms`. When main has been
   parked in the same episode longer than `dwell_threshold_ms`, it samples
   Singleton-A, waits `recheck_window_ms`, and re-samples. Unlike Site-B
   there are no decrementing counters -- the wait is binary -- so "zero
   progress" is: still the same episode (sequence unchanged + still
   parked), `pending` still 1, and the work-id + ack handle unchanged. A
   wait that is going to complete clears `pending` and ends the episode
   well inside the window. As a final guard it reads the worker-ack event
   state via `NtQueryEvent` and stands down if it just got signalled (main
   is already waking).
3. **Release (active mode only).** `SetEvent` on the worker-ack handle
   captured at entry. Because the worker is provably not progressing,
   delivering the ack is equivalent to the completion signal that was lost.

Defaults: `enabled=true`, `detect_only=true`, `dwell_threshold_ms=5000`,
`recheck_window_ms=1500`. Like Layer 2 it never suspends an engine thread
or reads a thread context and installs no process-wide hook; `id 34554`
is main-only (the render thread uses a different Singleton-A pair,
`id 34557/34567`), which Layer 4 covers.

### 3.9 Layer 4 — render-side Site-A worker-ack recovery (`SiteARenderBreaker`, new in v2.5.0)

The render-thread counterpart of Layer 3, documented in
[case-study 29 §6](../../docs/case-study/29-siteabreaker-plan.md) and first
captured in `freeze_2026-06-24_053440`. Faster HDT-SMP's per-frame parallel
cloth dispatch runs a worker loop (`id 34567`) on the render thread that, per
work item, calls a per-task join function (`id 34557`) which waits `INFINITE`
on each dispatched sub-task before the loop signals Singleton-A's worker-ack
`[+0x60]`. When a sub-task worker is stuck or gone (the same "0 idle" worker
pool that produces the main-side variant), the render thread parks forever
inside `id 34557` and never reaches the ack `SetEvent`, so any consumer
waiting on that ack hangs. In the capture the render thread was parked here
(RBP = the Singleton-A instance; frames `id 34557+0x7d` / `id 34567+0x86`)
while main spun on a heap `BSSpinLock` held by the same stuck pool.

`id 34567` reads the **same** Singleton-A field layout as main's `id 34554`
(`[+0x58]` worker-wake, `[+0x60]` worker-ack, `[+0x6c]` pending) — confirmed
by disassembly — so the recovery mirrors `SiteABreaker` exactly, with two
differences:

- The wrap is installed on **`id 34557`** (located by a byte-unique `.text`
  entry signature `40 53 56 57 48 83 EC 30 C6 41 71 01` =
  `push rbx; push rsi; push rdi; sub rsp,0x30; mov byte [rcx+0x71],1`), because
  that is the function the render thread is actually parked inside while stuck.
- `id 34557` receives its Singleton-A in **`rcx`** (it does `mov rbp,rcx` at
  entry) rather than via a rip-relative slot, so the breaker captures the
  singleton from the first argument at wrap entry rather than from a derived
  slot. The worker-ack is read as `[singleton+0x60]`.

The episode gate is "a non-null worker-ack handle at entry" (every `id 34557`
call is a real dispatched work item — the loop only calls it after a
worker-wake — so unlike the render worker-wake wait there is no idle
false-positive to guard against). The watchdog's zero-progress signature is:
same episode (sequence unchanged + still parked) with the work-id + ack handle
unchanged across the re-check window and the ack event still unsignalled.
`id 34557` normally returns every frame (~16 ms), so a join still in the same
episode after the dwell + re-check window is genuinely stuck.

`SetEvent` on the worker-ack publishes the completion ack the stuck join would
have raised (releasing the waiter); it does **not** itself unblock the deepest
per-sub-task wait inside `id 34557`. That semantic is why Layer 4 ships
detect-only by default until a field capture (FreezeLogger v0.9.0's
render-side probe in `MainWaitProbe`) confirms the release path. Defaults:
`enabled=true`, `detect_only=true`, `dwell_threshold_ms=5000`,
`poll_interval_ms=1000`, `recheck_window_ms=1500`. Config block
`[site_a_render_breaker]`. `AvailableSiteARender()` gates the module. Like the
other layers it never suspends an engine thread or reads a thread context.

---

## 4. Slow-path invariants

These rules govern any code that runs inside `OnSpinRetry`'s detection
path (when `WouldFormCycle` returns ≥ 2 and `Breaker::OnCycleDetected`
is called). Every iteration of this plugin that violated one of them
produced a freeze.

1. **No heap allocation.** Calling into the C runtime allocator
   from inside a `BSSpinLock::Acquire` detour puts the heap
   `CRITICAL_SECTION` on the BSSpinLock lock-order graph and
   creates a deadlock against legitimate engine paths. All cycle-
   tracking buffers are stack-resident (`std::array`) or fixed-size
   static storage.
2. **No `std::mutex` on the contended uncontended hot path.** SRWLocks
   under `std::mutex` create the same `(SRWLock → BSSpinLock)`
   lock-order edge. The breaker takes a `std::mutex` only when a
   cycle is observed (a rare event) and never on the hot path.
3. **No `safetyhook::InlineHook::call<>`.** That variant takes a
   `std::recursive_mutex` (`m_mutex`) for thread-safe install/
   uninstall. We use `unsafe_call` which tail-calls the trampoline
   directly.
4. **No spdlog calls.** Logging is gated to actual cycle
   observations only, which run rarely. Logging on every contended
   acquire would itself create a serialisation point.
5. **`state` is authoritative for "held", not `owner`.** The engine
   does not always clear `owner` on release. Reading `owner` to
   decide "is this lock free" causes the slow path to fire
   pathologically often.

---

## 5. Force-release semantics

When the breaker decides to force-release a lock, it CAS-flips the
`state` field from `1` to `0`. The `owner` field is left intact:
this matches the engine's own release behaviour (which also does not
always clear `owner`).

The thread that thought it owned the released lock keeps running its
critical section without protection until it next releases the lock
(which becomes a no-op, since `state` is already `0`). This is
acceptable because:

- The alternative is a permanent freeze.
- The same race happens transiently in vanilla Skyrim every time
  the engine releases a contended lock; the engine's invariants
  tolerate brief unguarded execution of these critical sections.
- No save corruption has been observed from confirmed breaks in
  testing.

The breaker only ever releases the lock at `chain[0].waiting_on`
(the lock the detecting thread was about to spin on). Choosing a
different victim would change which thread proceeds first but not
the outcome (both threads drain shortly after the release).

---

## 6. Lifecycle

### `SKSEPlugin_Load` (called once, at game startup)

1. Init logger.
2. Verify runtime is exactly SE 1.5.97; otherwise stay loaded but
   inert.
3. Read `WorkerSpinLockFix.toml`.
4. If `plugin.enabled = false`, stay inert.
5. `SKSE::AllocTrampoline(64)` -- reserves the SKSE trampoline pool
   that `Phase4Defer` uses for its two call-site patches (14 bytes
   each, plus headroom). `AcquireHook` uses safetyhook's own
   per-hook trampoline and does not consume from this pool.
6. `Hooks::Install()`:
   - `WaitGraph::Init()` (registry storage).
   - `Breaker::Init()` (recent-cycles map).
   - `AcquireHook::ResolveSpinRetryAddress()` and
     `AcquireHook::ResolveLockPointers()` unconditionally. The
     v2.0.3 reaper no longer reads the spin-retry RVA (it consumes
     `WaitGraph` edges directly), but resolution stays
     unconditional so toggling the entry-point hook on at runtime
     has zero resolution cost.
   - `AcquireHook::Install()` (Layer 2 spin-retry mid-hook at
     `id 12210 +0x8a`), gated by `acquire_hook.enabled`.
   - `Phase4Defer::Install()` (Layer 1 structural fix: one
     inline hook on `id 19369` plus two call-site patches at
     `id 36016+0xdcb` and `id 19372+0x606`), gated by
     `phase4_defer.enabled`. Fail-soft: a `Phase4Defer` install
     failure (including a refused call-site verification) is
     logged but leaves the v1.0 runtime breaker active.
   - `Reaper::Install()` if `reaper.enabled = true`.
7. `Stats::StartPeriodicDump()`.
8. Register the SKSE message listener.

### SKSE messages

- `kPostLoad`: log only.
- `kDataLoaded`: log; if `test_mode.enabled = true`, kick off the
  synthetic AB-BA harness.

### Periodic stats dump

Every `log.stats_interval_s` seconds, one info-level line with all
counters (acquire-slow, cycles observed/confirmed, breaks done/
raced/suppressed, `Phase4Defer` queued/drained/passthrough, plus
reaper counters).

---

## 7. What success looks like

On a healthy install:

1. Splash and main-menu come up cleanly.
2. The plugin's banner appears in the log; the
   `phase4_active=true` flag confirms Layer 1 is armed and the
   `acquire_hook_active=true` flag confirms Layer 2 is armed.
3. During gameplay `acq_slow` rises steadily (a few hundred per
   minute under load) reflecting normal worker-pool activity on
   LockA / LockB. `phase4_passthrough` rises only when execution
   reaches one of the two patched cycle-hub call sites without
   LockA being held -- much rarer than v2.0.1's first cut, since
   that earlier design intercepted *every* engine call into
   `id 40333` / `id 40334` regardless of cycle relevance.
4. `phase4_queued` rises whenever the LA->LB cycle would have
   fired and was preempted. `phase4_drained` matches
   `phase4_queued` over long horizons (every queued call is
   eventually replayed on the same thread).
5. `cycles_observed`, `cycles_confirmed`, and `breaks_done` stay
   at `0`: the structural fix preempts every cycle path the
   investigation identified, so the runtime breaker has nothing
   to break.
6. If the runtime breaker does fire (`breaks_done > 0`), that
   indicates a cycle path the structural fix missed and the
   runtime breaker still caught it - the game keeps running but
   the case is worth investigating to widen the structural fix.
7. No game lag, no splash freeze, no save corruption.

If something other than this is observed, the recovery path is the
emergency kill-switch chain:

- `phase4_defer.enabled = false` -> Layer 1 off, Layer 2 still
  installed (back to v1.0 behaviour).
- `acquire_hook.enabled = false` -> Layer 2 off (reaper-only or
  completely idle if the reaper is also off).
- `plugin.enabled = false` -> plugin loads but installs nothing.
- Remove the DLL -> engine runs entirely unmodified.

Each of these is a TOML edit + game restart, no rebuild needed.
