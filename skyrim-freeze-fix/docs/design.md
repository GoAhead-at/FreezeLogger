# WorkerSpinLockFix - design

This document describes the architecture of the `WorkerSpinLockFix`
SKSE plugin: what each module does, why it is shaped that way, and
the invariants that hold across the whole system.

For the engine bug being fixed see
[`../../docs/case-study/06-root-cause.md`](../../docs/case-study/06-root-cause.md).
For the engineering lessons that drove this design (in particular the
constraints on what may and may not happen inside the
`BSSpinLock::Acquire` slow path) see
[`../../docs/case-study/11-worker-spinlockfix-retrospective.md`](../../docs/case-study/11-worker-spinlockfix-retrospective.md).

---

## 1. Bug recap

Skyrim SE 1.5.97 contains a vanilla AB-BA inversion between two
static `BSSpinLock` globals in the worker dispatcher:

- `LockA` at `SkyrimSE+0x2eff8e0`, taken inside `id 19369`.
- `LockB` at `SkyrimSE+0x2f3b8e8`, taken inside `id 40706`
  (via `[arg+0x150]`) and inside `id 40333`.

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

The plugin is **detect-and-break at the spinlock level**, not at the
engine-function level:

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
heap `CRITICAL_SECTION` or against the engine's own `BSSpinLock`s –
see `11-worker-spinlockfix-retrospective.md` for the full set of
lessons.

The current design is **invisible in normal play** (one pointer
compare per `BSSpinLock::Acquire` for non-target locks) and acts only
when a real AB-BA cycle is observed, confirmed, and re-verified.

---

## 3. Module breakdown

### 3.1. `AcquireHook`

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

### 3.2. `WaitGraph`

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

### 3.3. `Breaker`

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

### 3.4. `Reaper`

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

### 3.5. `TestMode`

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
5. `Hooks::Install()`:
   - `WaitGraph::Init()` (registry storage).
   - `Breaker::Init()` (recent-cycles map).
   - `AcquireHook::ResolveSpinRetryAddress()` and
     `AcquireHook::ResolveLockPointers()` unconditionally (the
     reaper depends on the spin-retry RVA even when the entry-point
     hook is disabled).
   - `AcquireHook::Install()` (entry-point inline hook), gated by
     `acquire_hook.enabled`.
   - `Reaper::Install()` if `reaper.enabled = true`.
6. `Stats::StartPeriodicDump()`.
7. Register the SKSE message listener.

### SKSE messages

- `kPostLoad`: log only.
- `kDataLoaded`: log; if `test_mode.enabled = true`, kick off the
  synthetic AB-BA harness.

### Periodic stats dump

Every `log.stats_interval_s` seconds, one info-level line with all
counters (acquire-slow, cycles observed/confirmed, breaks done/
raced/suppressed, plus reaper counters).

---

## 7. What success looks like

On a healthy install:

1. Splash and main-menu come up cleanly.
2. The plugin's banner appears in the log followed by either an
   `AcquireHook` install line or a config-driven kill-switch
   message.
3. During gameplay `acq_slow` rises steadily (a few hundred per
   minute under load) reflecting normal worker-pool activity on
   LockA/LockB.
4. `cycles_observed`, `cycles_confirmed`, and `breaks_done` are
   all `0` unless the AB-BA race actually fires.
5. If the race does fire, the next stats line shows
   `cycles_observed=1, cycles_confirmed=1, breaks_done=1` and the
   game keeps running. A full `[CYCLE] / [BREAK]` block is logged
   when `log_cycle_events = true`.
6. No game lag, no splash freeze, no save corruption.

If something other than this is observed, the recovery path is the
emergency kill-switch chain:

- `acquire_hook.enabled = false` → reaper-only (or completely idle
  if the reaper is also off).
- `plugin.enabled = false` → plugin loads but installs nothing.
- Remove the DLL → engine runs entirely unmodified.

Each of these is a TOML edit + game restart, no rebuild needed.
