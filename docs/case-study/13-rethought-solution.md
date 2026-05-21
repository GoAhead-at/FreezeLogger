# 13 - Rethought Solution

**Date:** 2026-05-21
**Inputs synthesized:**
- `06-root-cause.md` — AB-BA evidence between LockA and LockB.
- `11-worker-spinlockfix-retrospective.md` — failure modes of v0.1 - v0.15.
- `12-engine-fix-mod-audit.md` — `safetyhook` / `tbb` library findings;
  confirmed no public solution exists.

**Status:** design proposal, not yet implemented. This document defines the
target architecture for `WorkerSpinLockFix v1.0` and the phased path to it.

---

## 1. Why we are redesigning

Across fifteen iterations we converged on three hard constraints that any
working solution must respect:

1. **Acquisition refusal is impossible.** `BSSpinLock::Acquire` returns
   `void`. The caller's compiled code assumes that on return, the lock is
   held. There is no error path. Any "prevent" strategy that relies on
   refusing the acquisition corrupts protected state.
2. **The set of LockB acquirers cannot be enumerated.** v0.10 - v0.14
   demonstrated empirically: every time we extended the serialization
   surface to one more identified path, the next freeze exposed yet
   another path we missed. There are too many call sites and indirect
   dispatches to bound statically.
3. **`BSSpinLock::Release` is heavily inlined.** Call-site hooking
   misses most releases. Per-thread "held lock set" maintenance via
   Acquire+Release pairs is therefore not a viable foundation.

What we now also know, from the audit of `EngineFixesSkyrim64` and
`po3-Tweaks`:

4. **No public reference solution exists.** Neither aers nor powerof3
   has touched `BSSpinLock`, the dispatcher, or the worker subsystem.
   Any prevention-grade fix is original engineering.
5. **`safetyhook` solves entry-point hook coexistence.** The single
   biggest tooling shortcoming of v0.9 / v0.9.1 — collision with
   `skyrim-freeze-fix.dll`'s prologue patch — is solved by adopting
   `safetyhook::create_inline`.
6. **`tbb::concurrent_hash_map` and the `form_caching` precedent
   suggest a structural option** — replace the locked data structure
   with a lock-free one — but this requires knowing what data the
   engine locks protect, which is unknown.

The redesigned solution is built on these six facts.

## 2. Strategic framing

We commit to a hybrid approach:

- **Reactive (ship target).** Detect cycles at the moment of formation
  and break them with the smallest possible corruption window.
  Sub-millisecond detection. ID-independent. Designed never to enumerate
  engine functions.
- **Structural (research track, parallel).** Reverse engineer what
  LockA and LockB actually protect. If the protected data is amenable to
  a lock-free replacement (along the lines of `form_caching`), build
  that as a v2.0 follow-up. Held in reserve; not on the critical path
  to a working v1.0.

The reactive track is what the rest of this document specifies.

## 3. Architecture: WorkerSpinLockFix v1.0

The working name for the next iteration. It is not a fork of v0.15; it is
a redesign that retains v0.15's stale-owner reaper as a safety net and
replaces v0.15's stack-walking watchdog with an entry-point hook.

### 3.1 Component overview

```
+-----------------------------------------------------------+
|                  WorkerSpinLockFix v1.0                   |
+-----------------------------------------------------------+
|                                                           |
|  +-----------------------+   +-------------------------+  |
|  |  AcquireHook          |   |  WaitGraph              |  |
|  |  (safetyhook on       |-->|  - per-thread tls       |  |
|  |   BSSpinLock::Acquire |   |    waiting_on           |  |
|  |   id 12210)           |   |  - lock.owner is        |  |
|  |  Fast-path: pass-     |   |    authoritative for    |  |
|  |   through if owner==0 |   |    "held by"            |  |
|  |   or owner==me        |   |  - chain walk for       |  |
|  |  Slow-path:           |   |    cycle detection      |  |
|  |   set tls.waiting_on, |   |    (no held-set needed) |  |
|  |   ask WaitGraph,      |   +-------------------------+  |
|  |   call original,      |              |                 |
|  |   clear tls.waiting_on|              v                 |
|  +-----------------------+   +-------------------------+  |
|                              |  Breaker                |  |
|                              |  - cycle detected       |  |
|  +-----------------------+   |  - choose victim lock   |  |
|  |  StaleOwnerReaper     |   |  - suspend cycle        |  |
|  |  (from v0.15)         |   |    members; CAS clear   |  |
|  |  background thread    |   |    state; resume        |  |
|  |  250 ms safety net    |   +-------------------------+  |
|  +-----------------------+                                |
|                                                           |
|  +-----------------------+                                |
|  |  Telemetry            |   - all detection events,     |
|  |  (replaces v0.15      |     with thread topology,     |
|  |   Stats.cpp)          |     break choice, latency.    |
|  +-----------------------+                                |
|                                                           |
+-----------------------------------------------------------+
```

### 3.2 Why this design satisfies the constraints

| Constraint | How v1.0 satisfies it |
|---|---|
| Cannot refuse acquisition | We never refuse. We let `original_acquire` run; if it would deadlock, we break the cycle elsewhere first. |
| Cannot enumerate LockB acquirers | Hook is on `BSSpinLock::Acquire` itself. Every acquisition is observed regardless of the calling function. ID independence is structural, not heuristic. |
| Release is inlined | Not needed. The lock's own `state` / `owner` field is the source of truth for "held by whom"; we do not maintain a held-set. |
| No public precedent | We acknowledge this. The design is conservative and the change is verified via instrumentation before any break is enabled. |
| Must coexist with `skyrim-freeze-fix.dll` | `safetyhook::create_inline` chains onto existing prologue patches. |
| Cannot lock-bypass without semantic knowledge | Held in reserve as v2.0. v1.0 does not depend on it. |

### 3.3 Data model

```cpp
struct alignas(64) ThreadState {
    BSSpinLock* waiting_on = nullptr;  // null when not in slow path
    std::atomic<std::uint64_t> waiting_since_tsc{0};
};

// Sparse map indexed by Skyrim worker TID. Sized once at first observation
// of each thread; never resized while in use. Threads we never see have no
// entry.
class WaitGraph {
    std::unordered_map<DWORD, ThreadState*> threads_;
    std::shared_mutex registration_lock_; // only on first registration
public:
    ThreadState& self();                  // tls fast-path accessor
    bool would_form_cycle(DWORD me, BSSpinLock* target) const;
    BreakDecision choose_victim(DWORD me, BSSpinLock* target) const;
};
```

`ThreadState` is owned per-thread (created lazily on first slow-path entry,
freed on thread exit via FLS callback) and pointed to from the global
`WaitGraph` only for cross-thread reads of `waiting_on`. The waiting_on
write is the only shared write per slow-path call.

### 3.4 Hook fast path

Pseudocode for the `safetyhook::create_inline` payload on
`BSSpinLock::Acquire` (`id 12210`):

```cpp
extern "C" void* __fastcall hooked_acquire(BSSpinLock* self) {
    const DWORD me = ::GetCurrentThreadId();
    const DWORD owner = self->owner;

    // Fast paths: not contended, or already ours.
    if (owner == 0 || owner == me) {
        return g_original_acquire(self);
    }

    return slow_path_acquire(self, me);
}
```

Two reads, one branch. Verified via micro-benchmark before deployment to
ensure overhead at the uncontended fast path is below ~5 ns. If the engine
calls `Acquire` ~10 M times per second across all threads, the budget is
~50 ms of CPU per second of game time, ~5 % of one core — within
acceptable range. Real measurement gates this.

### 3.5 Slow path and cycle detection

```cpp
void* slow_path_acquire(BSSpinLock* self, DWORD me) {
    auto& tls = WaitGraph::self();
    tls.waiting_on = self;
    tls.waiting_since_tsc.store(__rdtsc(), std::memory_order_release);

    if (g_break_enabled && g_graph.would_form_cycle(me, self)) {
        Breaker::break_cycle(me, self);  // see 3.6
    }

    void* result = g_original_acquire(self);   // engine spins normally

    tls.waiting_on = nullptr;
    return result;
}
```

`would_form_cycle` walks the wait-for chain forward without touching
held-sets:

```cpp
bool WaitGraph::would_form_cycle(DWORD me, BSSpinLock* target) const {
    DWORD owner = target->owner;
    int hops = 0;
    while (owner != 0 && owner != me && hops++ < kMaxHops) {
        const auto* state = lookup(owner);
        if (!state) return false;             // owner not registered
        BSSpinLock* next = state->waiting_on;
        if (!next) return false;              // owner not waiting
        owner = next->owner;
    }
    return owner == me;                       // closed the loop
}
```

Properties:
- O(cycle length) — typically 2 for AB-BA, bounded by `kMaxHops` (e.g. 16).
- No held-sets, no inlined-Release dependence.
- Authoritative: every read is from a memory location the engine itself
  maintains (`lock->owner`, `tls.waiting_on`).
- Safe under races: a concurrent state change in the middle of the walk
  yields either a false negative (we miss this cycle and v0.15's
  stale-owner reaper picks it up later) or a false positive
  (`would_form_cycle` returns true, but on closer inspection in
  `Breaker::break_cycle` the cycle is no longer present, so we abort the
  break). False positives are not corruption-causing because the break
  step re-checks atomically.

### 3.6 Breaker — the unavoidable corruption-window question

When a cycle is detected we must release one lock in the cycle. There is
no third option: we cannot refuse the acquisition (constraint 1) and
sleeping does not help (the holders are themselves cycle-locked).

The choice of which lock to release determines the corruption window. We
adopt the heuristic **"force-release the most-recently-acquired lock in
the cycle"**, on the rationale that the most recently acquired lock is
most likely still in the lock-acquisition phase rather than mid-write to
protected state. This requires a `last_acquired_tsc` field per lock,
maintained on slow-path completion.

#### Confirmation window before break

We do not break on first detection. Spinning threads re-enter
`hooked_acquire`'s slow path on every cycle of their spin loop, so true
deadlocks produce rapid repeated detections of the same cycle topology,
while transient near-cycles disappear after one or two detections.
Exploiting this, we gate every break behind a confirmation window:

- Each detected cycle is reduced to a **cycle signature**: the sorted set
  of `(thread_id, BSSpinLock*)` pairs in the chain. Two detections of
  the same wait topology produce the same signature regardless of which
  thread observed it.
- A small bounded LRU map (`g_recent_cycles`, ~32 entries) records each
  signature's `first_seen_tsc` and `last_seen_tsc`.
- When a signature is observed for the first time, we record it and
  return without breaking.
- On subsequent observations of the same signature, we update
  `last_seen_tsc` and check
  `now - first_seen_tsc >= confirmation_window`.
- Only after the confirmation window has elapsed do we proceed into
  `Breaker::break_cycle`. Entries that have not been observed for
  `confirmation_window * 4` are evicted as stale.

The confirmation window is configurable via the plugin's `.toml`:

```toml
[breaker]
# wall-clock duration a cycle topology must persist before we break it.
# 0 disables the gate (break on first detection).
confirmation_window_ms = 2
```

Default `2 ms`. Rationale: a true AB-BA deadlock is permanent, so any
wall-clock value below the user-perceptible hitch threshold (~16 ms at
60 fps, ~8 ms at 120 fps) is acceptable; small values still drastically
reduce phantom breaks while keeping the break decision orders of
magnitude faster than v0.15's 750 ms (3 × 250 ms reaper) confirmation.

#### Break sequence

```cpp
void Breaker::break_cycle(DWORD me, BSSpinLock* target) {
    auto victim = WaitGraph::choose_victim(me, target);  // most recent acq
    if (!victim.lock) return;        // race: cycle already gone

    SuspendedSet members = suspend_cycle_members(victim);
    if (!verify_cycle_still_present(victim, members)) {
        resume_all(members);
        Telemetry::cycle_evaporated();
        return;
    }

    DWORD victim_owner = victim.lock->owner;
    if (atomic_cas(&victim.lock->state, /*expected*/1, /*new*/0)) {
        victim.lock->owner = 0;     // best-effort, owner is non-authoritative
        Telemetry::cycle_broken(victim, victim_owner);
    } else {
        Telemetry::break_cas_failed(victim, victim_owner);
    }
    resume_all(members);
}
```

Four observations:

- The confirmation window is the **first** filter against false positives
  and is the safety property the user explicitly requested.
- Suspending threads only after detection limits the suspend window to
  the duration of the break itself (microseconds). v0.15's reaper does
  this routinely; we reuse its suspend/resume scaffolding.
- `verify_cycle_still_present` re-reads `lock->owner` and
  `tls.waiting_on` post-suspension. This is the **second** filter that
  converts surviving false positives into no-ops.
- The `victim_owner` thread will, after resume, eventually return from
  its lock-acquisition loop or call its inlined release; if it writes
  data under the assumption it still holds the victim lock, the
  corruption window is from victim release to its next lock-acquisition
  call. With the confirmation window the corruption window is
  guarded by the empirical evidence that the cycle was truly stuck for
  ≥ confirmation_window milliseconds, not a transient. v0.15 has the
  same hard risk profile with a much weaker confirmation; v1.0 strictly
  dominates on safety while improving detection latency by two orders of
  magnitude.

### 3.7 Telemetry

Every cycle detection event logs:

- timestamp,
- the chain (TIDs and lock addresses),
- victim chosen and rationale (most-recent acquisition timestamp),
- whether the cycle evaporated before break,
- detection latency (slow-path entry → break decision),
- engine telemetry around the event (frame index, recent
  `OnFrameUpdate`).

This is the validation surface. Without a deterministic repro we cannot
manufacture cycles, but every real cycle the engine produces becomes a
high-fidelity log entry against which we audit behaviour.

## 4. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Hook overhead at fast path is too high | Medium | Micro-benchmark before deployment; back off to a sampled subset of locks if needed (e.g. only LockA / LockB and contended locks). |
| `safetyhook` chain interaction with `skyrim-freeze-fix.dll` is broken in some load orders | Medium | Phase-2 deployment is detect-only (no break). Verify chain compatibility with `skyrim-freeze-fix.dll` enabled and disabled before enabling break. |
| Force-release corrupts protected state visibly | Medium | (a) Same as v0.15's existing risk profile, which has not produced observed corruption in shipped runs. (b) Choose most-recently-acquired victim. (c) Telemetry on every break — if a crash follows a break event, we have the topology. |
| Hot lock starves slow-path and breaks game timing (v0.12 class) | Low-Medium | Only the slow path runs new work; fast path is unchanged from engine. Detection adds work only when contended (rare). Re-validate by soak-testing detection-only mode for at least 24 hours of play time. |
| TLS exhaustion on engine threads we never see | Low | Lazy registration, FLS callback for cleanup, no allocation on fast path. |
| Race in `would_form_cycle` produces phantom break | Low | Re-verify under suspension before clearing `state` (3.6). |

## 5. Validation strategy without a deterministic repro

We never get a deterministic repro. Validation must therefore be
statistical and stress-based.

### 5.1 Synthetic deadlock harness

Build a small test SKSE plugin (`ABBAStress`) that intentionally creates
AB-BA between two `BSSpinLock`-shaped objects on two background threads.
Run it inside the same process, with `WorkerSpinLockFix v1.0` loaded.

- The harness deliberately creates cycles every N seconds.
- v1.0 should detect each cycle with sub-millisecond latency.
- v1.0 should break each cycle without harness state corruption (the
  harness verifies its own protected counter).

This is the only way to verify the break primitive works correctly without
waiting for a real cycle.

### 5.2 Detection-only soak

Phase 2 (see 6) ships detection-only: log every cycle that would be
broken, but do not break. Compare detection events against the v0.15
watchdog detections in parallel runs. We expect:

- v1.0 detections include all v0.15 detections (it must, since v0.15
  was post-hoc).
- v1.0 detections include cycles that resolved themselves before v0.15
  could observe them — these are the previously-invisible near-misses.
- No false positives caused by cross-thread races (filtered by 3.5 / 3.6).

### 5.3 Performance regression

Frame-time histogram before and after, on a fixed save and a fixed scripted
walk-around. Acceptable: median frame-time delta within ±2 %, p99
within ±5 %. If exceeded, fall back to the sampling design noted in the
risk register.

### 5.4 Multi-mod compatibility matrix

Verify safetyhook chain on the matrix `{ skyrim-freeze-fix.dll, *, none }
× { v1.0 first, v1.0 last }`. Failure mode here is the v0.9 / v0.9.1
crash class; we close it before enabling break.

## 6. Phased delivery

Each phase produces a runnable plugin that can be soaked. No phase
attempts to break cycles until the previous phase has soaked clean.

Per the user's decision (2026-05-21), **the original Phase 1 (tooling
refactor as a standalone v0.16 ship) and Phase 2 (observability,
detect-only) are merged into a single Phase 1 deliverable**. The numbering
below reflects this choice; documents 11 and 12 still refer to the
original five-phase split for historical context.

### Phase 1 — Tooling + Observability (detect-only)

Goal: install the new plumbing **and** the new entry-point hook +
WaitGraph in one ship target. Cycles are detected and logged; nothing
is broken.

- Add `safetyhook` to `vcpkg.json`.
- Audit `CommonLibSSE-NG`'s `BSSpinLock` definition against our observed
  layout (`owner` at +0, `state` at +4) and adopt the canonical layout
  if it matches.
- Replace any remaining raw trampoline scaffolding from v0.15 with
  `safetyhook` equivalents.
- Implement `AcquireHook` (entry-point hook on `BSSpinLock::Acquire`,
  `id 12210`), `WaitGraph`, `would_form_cycle`, `choose_victim`,
  cycle-signature `recent_cycles` map.
- Implement `Telemetry::cycle_would_break(...)` and the per-event log
  artefact.
- Leave `g_break_enabled = false`. v0.15's stale-owner reaper remains
  active as it is.
- Run `ABBAStress` to verify detection works.
- Soak with real gameplay for at least one full session.

Exit criterion: v0.17 in detect-only mode for ≥ 24 h of real play with
- zero stability regressions vs v0.15,
- detection events strictly include the v0.15 watchdog's events,
- multi-mod compatibility matrix green.

### Phase 2 — Intervention

Goal: enable `g_break_enabled`, replacing v0.15's watchdog as the primary
intervention.

- `Breaker::break_cycle` enabled, gated by the confirmation window
  (default 2 ms) defined in 3.6.
- v0.15's stack-walking watchdog disabled (replaced).
- v0.15's stale-owner reaper retained as safety net at longer interval
  (e.g. 1 s instead of 250 ms; it is now backstop, not primary).
- Soak with real gameplay; confirm break events do not produce crashes
  in the next ≥ 24 h of play.

Exit criterion: v1.0 ships when (a) at least one real cycle has been
broken successfully without follow-on crash, and (b) frame-time
regression budget is not exceeded.

### Phase 3 — Hardening

Goal: durability and operability.

- Configurable detection / break enable from the `.toml`.
- Per-cycle log artefact for offline review.
- `ABBAStress` integrated into a periodic CI job (still no deterministic
  repro, but stress harness regressions are caught).

### Phase 4 — Structural research (parallel, unbounded)

Goal: identify what data LockA and LockB protect.

- Use Phase-2 break events: snapshot the call stack of each lock holder
  when a cycle fires. Over many events, the set of in-flight functions
  under each lock will narrow.
- Cross-reference with disassembly of those functions to identify the
  data structures the locked region reads/writes.
- If a lock-bypass strategy in the spirit of `form_caching` becomes
  feasible for one of the two locks, design v2.0.

This phase has no committed delivery date. It is the path to true
prevention — but it does not need to land for v1.0 to ship a robust
solution.

## 7. What we explicitly are not doing

To be exhaustive about the design boundary:

- **No more "wrap selected gameplay functions with a recursive mutex."**
  v0.1 - v0.14 conclusively demonstrated that this approach is unbounded
  and produces a new AB-BA with our own mutex.
- **No more entry-point hook on `BSSpinLock::Acquire` via raw
  trampoline.** v0.9 / v0.9.1 conclusively demonstrated that raw
  trampolines collide with other prologue patchers. `safetyhook`
  replaces this.
- **No held-set maintenance via Acquire+Release pairing.** Inlined
  `Release` makes this impossible to keep accurate. Cycle detection
  walks the wait chain instead, which does not require it.
- **No reliance on engine function IDs in the cycle detection path.**
  Function IDs are used only for the entry-point hook target
  (`id 12210`, `BSSpinLock::Acquire` itself). Per-call detection is
  ID-independent.
- **No attempt to "fix" Bethesda's locking by enforcing ordering across
  the engine.** We acknowledge that both A-before-B and B-before-A
  exist in compiled code and that we cannot tell which is intended
  without engine source.

## 8. Status line for this document

This is a design proposal. Implementation begins at Phase 1 once approved.
Until Phase 3 ships and a real cycle has been broken cleanly, v0.15
remains the production strategy.

The design is intentionally conservative on the corruption-window
question (3.6). It does not attempt to invent semantics the engine does
not provide. It commits to the following:

1. Make detection sub-millisecond and ID-independent.
2. Make multi-mod hook coexistence robust.
3. Make every intervention loggable and post-hoc auditable.
4. Hold the structural research as a parallel track that can supersede
   this design when (and only when) it produces tractable knowledge.

## Cross-references

- `06-root-cause.md` — the deadlock evidence this design is built around.
- `11-worker-spinlockfix-retrospective.md` — the v0.1 - v0.15 history
  whose lessons constrain this design.
- `12-engine-fix-mod-audit.md` — the audit that produced the `safetyhook`
  and "lock-bypass philosophy" inputs to this design.
