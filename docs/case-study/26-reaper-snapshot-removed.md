# 26. v2.0.3 - Reaper safety redesign: `SuspendThread` retired

**Date:** 2026-05-24
**Status:** Released as part of `WorkerSpinLockFix v2.0.3`
**Type:** Architectural change. Same external behaviour for the
common case; reduces blast radius of the optional stale-owner
backstop.

## 1. Context

`WorkerSpinLockFix` ships two layers:

| Layer | Module(s) | What it does |
|---|---|---|
| Layer 1 (default) | `Phase4Defer` | Structural fix. Defers LockB acquires while LockA is held, so the AB-BA cycle never forms. |
| Layer 2 (default) | `AcquireHook` + `WaitGraph` + `Breaker` | Runtime fix. Hooks `BSSpinLock::Acquire (id 12210)`, builds a wait-for graph on the slow path, force-releases a lock when a confirmed cycle is detected. |
| Layer 3 (optional) | `Reaper` | Stale-owner backstop for cases Layer 2 cannot observe (the owner thread died still holding a lock between the slow-path entry and the cycle check). |

The Reaper is OFF by default. It exists as a safety net, not as a
hot path. But until v2.0.3, when you turned it on, it was the one
piece of the plugin that:

- Enumerated every process thread via `CreateToolhelp32Snapshot`.
- Opened each thread with `THREAD_GET_CONTEXT |
  THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION`.
- Called `SuspendThread` on every engine thread.
- Called `GetThreadContext` on every engine thread.
- Walked registers and stack windows looking for plausible
  `BSSpinLock` candidates, by recognising the spin-retry RVA in
  `RIP` and `RSP[0..N]`.
- Called `ResumeThread` on every engine thread.

That design was inherited from `v0.x` where it was the entire
plugin (no entry-point hook existed). It survived through `v1.x`
and the early `v2.0.x` builds because it was the only piece that
could see indirect dispatches the entry-point hook never observes.

The user audit on 2026-05-24 surfaced a hazard in this design that
is not closeable inside user mode.

## 2. The `GetThreadContext`-on-suspended-thread hazard

The unsafe pattern is documented in the Win32 docs but worth
restating because every workaround we considered turns out to be
strictly worse:

```
SuspendThread(t);          // schedules a kernel-level suspension
                           // of `t`. Returns synchronously, but
                           // suspension is asynchronous: the kernel
                           // marks t but only stops it on the next
                           // kernel transition.

GetThreadContext(t, &ctx); // blocks the caller until t is fully
                           // suspended at a "safe" instruction
                           // boundary (returns from any in-progress
                           // syscall, etc.). On a healthy thread
                           // this completes in microseconds.
                           //
                           // On a thread that is itself blocked
                           // inside the kernel on a primitive whose
                           // wakeup the suspending caller is now
                           // (transitively) holding -- e.g. inside
                           // a global allocator critical section,
                           // or a heap lock the suspending caller
                           // also touches in its allocator path --
                           // the kernel cannot complete the safe-
                           // boundary handoff. GetThreadContext
                           // never returns.

ResumeThread(t);           // never reached.
```

That last bullet is the part that does not have a clean fix.

We considered three mitigations and rejected all three:

### 2.1. Add a wall-clock timeout via thread-pool helper

> "Run `GetThreadContext` on a thread-pool work item, abandon it
> after 500ms, the main reaper thread keeps going."

This works for `GetThreadContext`. It does not work for
`SuspendThread`. After abandonment the helper is still in the
kernel waiting on the same primitive; the engine thread is still
suspended; nothing on the suspended thread has run; the helper
cannot be killed safely.

`TerminateThread` on the helper is not a fix. The Win32 docs are
explicit:

> If the target thread owns a critical section, the critical
> section will not be released. If the target thread is allocating
> memory from the heap, the heap lock will not be released.

The helper, by construction, was holding *whichever* heap or
allocator critical section made `GetThreadContext` block.
`TerminateThread` leaks that lock process-wide. The plugin would
now be the most likely cause of the next freeze, not the
prevention of it.

`CloseHandle` does not help: the handle goes away but the
suspension count on the engine thread does not. Future
`SuspendThread / GetThreadContext` calls in subsequent ticks
inherit the stuck state.

### 2.2. Skip `GetThreadContext`, just suspend + resume

Without the context we cannot identify which threads are
spinners. We would suspend every engine thread blindly, including
ones inside critical sections, then immediately resume them. That
is a recipe for accidental priority inversion and adds nothing
detectable to the plugin's job; it is strictly worse than running
the reaper less often.

### 2.3. Move suspension to a debug-only build

That is a de-facto removal in production. We rejected it as
half-hearted: if the runtime path is unsafe, the runtime path
should not have it at all.

## 3. The decision

Replace the snapshot-based scan with a `WaitGraph`-edge consumer.

The `WaitGraph` is a 64-slot lock-free array maintained by
`AcquireHook` on the slow path. When a thread enters the slow
path of `BSSpinLock::Acquire`, it claims a slot keyed on its TID
and publishes `(tid, waiting_on)` atomically. When the slow path
exits, it clears `waiting_on`.

The Reaper does not need to discover candidates. The slots ARE
the candidates.

```
Reaper::Tick():
  edges = WaitGraph::SnapshotEdges(64)         // O(64), lock-free
  for e in edges:
    (owner, state) = SEH-load(e.waiting_on)    // 8-byte load
    if state == 0 or owner == 0 or owner == e.waiter: skip
    fold (e.waiter, e.waiting_on, owner) into a moving window
  for stable in window:
    if stable.age >= 2000ms and not IsThreadAlive(stable.owner):
      CAS state -> 0
    if stable.age >= 5000ms and IsThreadAlive(stable.owner):
      log diagnostic (LIVE-OWNER WAIT)
```

`IsThreadAlive` is `OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
...) + GetExitCodeThread + STILL_ACTIVE`. None of those calls
suspend anything. None of them require thread context. None of
them have an unbounded blocking case under adversarial kernel
states.

## 4. Trade-offs

### 4.1. Coverage

The pre-v2.0.3 reaper saw any thread holding any `BSSpinLock`,
including threads acquiring through inlined or non-`id 12210`
paths -- because it scanned every thread's registers and stack
for the spin-retry RVA.

The v2.0.3 reaper sees only threads that traversed the
`AcquireHook` slow path, i.e. threads whose `id 12210` call hit
`state != 0`.

In principle this is a coverage loss. In practice, Phase 1.5
(case-study doc 17) confirmed all six known acquirers in the
binary go through `id 12210`. The locks the AB-BA bug touches
(LockA `SkyrimSE+0x2eff8e0`, LockB `SkyrimSE+0x2f3b8e8`) are
only ever taken via `id 12210`. The "inlined" code paths the old
scan was theoretically protecting against do not exist for the
bug class this plugin targets.

The new reaper still catches the headline backstop case: an
engine thread died still holding a lock that another engine
thread is now waiting on via `id 12210`. That is the only
stale-owner pattern the plugin has ever seen evidence of in
production.

### 4.2. Coverage requires AcquireHook

`AcquireHook` populates the `WaitGraph`. With `[acquire_hook]
enabled = false` the `WaitGraph` stays empty; the v2.0.3 reaper
becomes a no-op. The toml comments now make this explicit. The
`Hooks::Install` log line states it as well.

This is a real change in behaviour for the rare user who runs
`acquire_hook = false, reaper = true`. The pre-v2.0.3 reaper had
its own discovery mechanism (the thread-stack scan) and could
function alone. The v2.0.3 reaper cannot. Given that
`acquire_hook = false` is itself a defensive opt-out, anyone who
turned it off was already accepting reduced coverage; the
post-v2.0.3 plugin makes that reduction louder rather than
silently buying coverage with `SuspendThread` calls.

### 4.3. Diagnostics

`LIVE-OWNER WAIT` lines lose three previously-useful fields:

- `owner_rip` (the instruction the owner thread is parked on)
- `owner_module` (the DLL containing `owner_rip`)
- `owner_rcx` (likely the lock pointer the owner is itself
  waiting on)

Those came from `SnapshotThread` which is gone. The line now
says so explicitly:

```
[REAPER] LIVE-OWNER WAIT held=5012ms waiter=TID 4112 lock=0x...
owner=TID 7820 (alive). Owner RIP / module / RCX intentionally
omitted: the v2.0.3 redesign retired SuspendThread /
GetThreadContext from the runtime path. See
docs/case-study/26-reaper-snapshot-removed.md.
```

The diagnostic is now strictly weaker but still records the
existence and duration of the live-owner stall, which is the
signal the field is meant to surface.

If a future bug requires the RIP / module / RCX fields again, the
right path is a one-shot opt-in capture (e.g. a debug command
that takes the snapshot exactly once on user request) rather
than a periodic background scan. That is left as future work.

## 5. What changed in code

| File | Change |
|---|---|
| `src/WaitGraph.h` | Added `struct EdgeView` and `int SnapshotEdges(EdgeView*, int cap)`. |
| `src/WaitGraph.cpp` | Implemented `SnapshotEdges` (lock-free, allocation-free). |
| `src/Reaper.cpp` | Deleted `SnapshotThread`, `EnumerateThreads`, `IsSpinner`, `CollectLockCandidates`, `AddCandidate`, `OwnerSnapshot`, `ProbeOwner`, `ResolveModule`. Deleted includes for `TlHelp32.h`, `Psapi.h`. Rewrote `Tick()` around `WaitGraph::SnapshotEdges`. Updated `LogLiveOwnerProbes` to drop RIP / module / RCX fields and explain why. Updated `ReaperBody`'s startup banner to advertise the new design. |
| `src/Reaper.h` | Updated header comment to describe the WaitGraph consumer. |
| `src/Hooks.cpp` | Updated the spin-retry resolution comment and the `acquire_hook = false` warning to reflect the reaper's new dependency. |
| `dist/WorkerSpinLockFix.toml` | Rewrote the `[reaper]` doc block. |
| `docs/design.md` | Rewrote §3.5. |
| `docs/case-study/README.md` | Indexed this document. |
| `CMakeLists.txt` | Bumped version to `2.0.3`, updated description. |

## 6. Rollback

If `2.0.3` introduces a regression specifically against the
backstop case (a stale-owner pattern the new reaper does not
catch but the old one did), the `2.0.1` reaper code remains in
git history at the commit before this change. Rolling back is a
single-file revert of `src/Reaper.cpp` plus restoring the
deleted helpers; no API surface outside the Reaper module
depends on it.

The `WaitGraph::SnapshotEdges` API added in `2.0.3` does not
need to be removed on rollback; it is harmless if unused.

## 7. Out of scope

- This change does NOT touch `FreezeLogger`. `FreezeLogger`'s
  thread snapshot still uses `SuspendThread` + `GetThreadContext`
  + `StackWalk64`. That is acceptable because `FreezeLogger` is
  a one-shot dump at watchdog timeout, runs from a dedicated
  helper thread, and is the diagnostic users explicitly enable
  to capture freeze evidence. Its risk profile is fundamentally
  different from a periodic background scan.

- This change does NOT touch the AB-BA breaker. `Breaker` is
  still allowed to suspend cycle members via `SuspendThread`
  during force-release. That call site is bounded, has a known
  exit (single CAS, then resume), only touches the two threads
  in the confirmed cycle, and is gated on `breaks_done > 0`
  events that have never been observed in production with
  Layer 1 active. Removing it is a future cleanup, not part of
  this change.
