# WorkerSpinLockFix - design

This document describes the architecture of the `WorkerSpinLockFix`
SKSE plugin **as of v2.0.0**: what each module does, why it is
shaped that way, and the invariants that hold across the whole
system.

The plugin layers two independent fixes for the same engine bug:

1. **Layer 1 - Structural fix (`Phase4Defer`, v2.0).** One inline
   hook on the LockA acquirer (`id 19369`) plus two surgical
   call-site patches inside the cycle hub (`id 36016+0xdcb` ->
   `id 40334`, `id 19372+0x606` -> `id 40333`) defer the LockB
   acquirers whenever the current thread is inside `id 19369`.
   The deferred calls drain on the same thread when LockA is
   released. The AB-BA cycle cannot form. The function entries
   of `id 40333` / `id 40334` are left pristine so other mods
   that hook those functions cooperate with this plugin.
2. **Layer 2 - Runtime breaker (`AcquireHook` + `WaitGraph` +
   `Breaker`, v1.0).** A surgical inline hook on
   `BSSpinLock::Acquire (id 12210)` detects cycles that form
   anyway (cycle paths the structural fix misses) and force-
   releases one lock so the spinning thread proceeds.

Layer 1 is the primary fix. Layer 2 is defence-in-depth: with the
structural fix active and healthy it should never fire.

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

The plugin retains v1.0's **detect-and-break at the spinlock level**:

```
                                  ┌──────────────┐
                                  │ BSSpinLock   │
                                  │ ::Acquire    │
                                  │ (id 12210)   │
                                  └──────┬───────┘
                                         │ inline hook
                                         ▼
              ┌─────────────────── HookedAcquire ────────────────────┐
              │                                                       │
              │  surgical filter:                                     │
              │    self == LockA || self == LockB? ──── no ──→ trampoline
              │       │                                               │
              │      yes                                              │
              │       ▼                                               │
              │  state == 0?  ─── yes (uncontended) ──→ trampoline    │
              │       │                                               │
              │      no (contended)                                   │
              │       ▼                                               │
              │  WaitGraph::EnterSlow(me, self)                       │
              │  WaitGraph::WouldFormCycle(me, self, …)               │
              │       │                                               │
              │       ├── chain >= 2 ───▶ Breaker::OnCycleDetected    │
              │       │                                               │
              │       ▼                                               │
              │     trampoline (engine spin loop)                     │
              │       │                                               │
              │       ▼                                               │
              │  WaitGraph::ExitSlow(me)                              │
              └───────────────────────────────────────────────────────┘
```

We did not pursue function-entry serialisation (gating the engine
functions that take LockA/LockB behind a plugin mutex) because every
attempt at that produced a new lock-ordering deadlock against the
heap `CRITICAL_SECTION` or against the engine's own `BSSpinLock`s -
see `11-worker-spinlockfix-retrospective.md` for the full set of
lessons.

The Layer 2 design is **invisible in normal play** (one pointer
compare per `BSSpinLock::Acquire` for non-target locks) and acts
only when a real AB-BA cycle is observed, confirmed, and
re-verified. The Layer 1 design is similarly cheap: one
thread-local read per `id 40333` / `id 40334` entry on the common
(pass-through) path, plus one increment/decrement per `id 19369`
entry/exit.

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

Installs a single `safetyhook::create_inline` hook on
`BSSpinLock::Acquire` (`id 12210`).

The detour applies a four-pointer **surgical filter**:

```cpp
if (self != g_lockA && self != g_lockB &&
    self != g_test_lockA && self != g_test_lockB) {
    g_acquire_hook.unsafe_call<void>(self);   // tail-call
    return;
}
```

`g_lockA` and `g_lockB` are resolved at install time from the
SkyrimSE.exe module base + the documented RVAs. `g_test_lockA` and
`g_test_lockB` are normally `nullptr` (so they always compare unequal
to any real BSSpinLock pointer); they only become non-null when the
optional test mode is enabled.

For a target lock the detour follows the rules below:

- `state` (`+0x4`) is authoritative for "held". `0` = free,
  `1` = held. `owner` (`+0x0`) is **not** authoritative because the
  engine does not always clear it on release.
- Fast paths (uncontended free + recursive same-thread acquire)
  tail-call the trampoline immediately.
- The slow path runs only for contended LockA / LockB and calls into
  `WaitGraph` and `Breaker`.

`safetyhook::InlineHook::unsafe_call` is used everywhere the
trampoline is invoked. The library's `call<>` variant takes an
internal `std::recursive_mutex` for thread-safe install/uninstall;
with ~300 engine threads each routing through the hook, that mutex
would serialise every acquire across all threads. We never
uninstall the hook at runtime, so we cannot race with installation
or destruction either way.

### 3.3. `WaitGraph`

A fixed-size, lock-free wait-for graph. Each thread that enters the
slow path claims one of 64 cache-aligned slots (lazily, from a
thread-local cache) and writes its `waiting_on` pointer into that
slot:

```cpp
struct alignas(64) ThreadSlot {
    std::atomic<DWORD>  tid;
    std::atomic<Lock*>  waiting_on;
};

std::array<ThreadSlot, 64> g_slots;
```

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

Optional stale-owner backstop. Disabled by default. Periodically:

- Enumerates all process threads via Toolhelp32.
- Identifies threads spinning in `BSSpinLock::Acquire` by stack
  pattern (the spin-retry RVA, computed at install time as
  `id 12210 + 0x8a`).
- Collects plausible BSSpinLock candidates from registers and stack
  windows.
- If an observed lock is owned by a TID that is no longer alive,
  force-releases the lock via `CAS(state -> 0)`.

The reaper does no live-cycle detection; that is the
`AcquireHook + Breaker` path's job. It exists for cases the entry-
point hook cannot observe: threads that died holding a lock,
indirect dispatches the hook never sees.

It is the only part of the plugin that suspends engine threads,
makes Psapi calls, or allocates from those code paths, so it is
also the only part that can plausibly cause load-time stalls on
heavy modlists. That is why it is off by default.

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

---

## 4. Slow-path invariants

These rules govern any code that runs inside `HookedAcquire`'s slow
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
     `AcquireHook::ResolveLockPointers()` unconditionally (the
     reaper depends on the spin-retry RVA even when the entry-point
     hook is disabled).
   - `AcquireHook::Install()` (Layer 2 entry-point inline hook),
     gated by `acquire_hook.enabled`.
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
