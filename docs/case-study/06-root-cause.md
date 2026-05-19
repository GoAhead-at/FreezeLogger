# 06 - Root Cause

## TL;DR

Skyrim SE 1.5.97's worker pool contains a lock-ordering bug. Two
distinct dispatch paths reach the same pair of `BSSpinLock` globals in
opposite order. When two worker threads run those paths simultaneously,
the result is a classic AB-BA deadlock. Main thread, waiting on one of
the workers to signal completion, blocks indefinitely.

## The lock pair

| Lock  | RVA in `SkyrimSE.exe` | Address Library scope                     | How it is acquired                                     |
|-------|-----------------------|--------------------------------------------|--------------------------------------------------------|
| LockA | `+0x2eff8e0`          | Private to `id 19369`.                    | Direct `lea rcx, [rip + ...]` then `call BSSpinLock::Acquire`. |
| LockB | `+0x2f3b8e8`          | Shared by `id 40285`, `40333`, `40334`, `40335` (direct lea). Also `id 40706` (indirect via `[arg+0x150]`). | Same direct-lea pattern in the four shared callers; `[arg + 0x150]` lea in `id 40706`. |

Both locks are 8-byte `BSSpinLock` records:

```
struct BSSpinLock {
    uint32_t threadID;   // current owner; 0 means free
    uint32_t lockState;  // 0 = free, 1 = locked, 2 = contended
};
```

## The two acquire orderings

### Path P1 (LockA first, then LockB)

Frames as observed for TID 5096 in `freeze_2026-05-19_120444_both.log`,
top-of-stack first:

```
#03 BSSpinLock::Acquire           (id 12210)        <-- spinning
#04 id 40333  +0x30                                 <-- spinning on LockB
#05 id 19372  +0x60b
#06 id 17521  +0x626
#07 id 19369  +0x5a4              <-- already inside (LockA held)
#08 id 36854  +0x1eb              <-- bridge frame
#09 id 37388  +0xa0
#10 id 40706  +0x250
#11 id 40289  +0x6a               <-- worker dispatch
#12 id 36360  +0xfe
... (down to id 67147 worker thread entry)
```

Sequence of lock interactions:

1. `id 19369` is entered (frame #07). Its first non-prologue
   instruction at `+0x38` calls `BSSpinLock::Acquire(LockA)`. **LockA
   is now held** by TID 5096.
2. `id 19369` descends through `id 17521 -> id 19372` (calls in its
   body that are themselves still inside the LockA-protected region).
3. `id 19372` calls `id 40333` (frame #04 returning to `+0x30`).
4. `id 40333` at `+0x2b` calls `BSSpinLock::Acquire(LockB)`. **TID
   5096 spins on LockB**, holding LockA.

### Path P2 (LockB first, then LockA)

Frames as observed for TID 18456 in the same report:

```
#03 BSSpinLock::Acquire           (id 12210)        <-- spinning
#04 id 19369  +0x3d                                 <-- spinning on LockA
#05 id 36854  +0x1eb              <-- bridge frame
#06 id 37388  +0xa0
#07 id 40706  +0x250              <-- already inside (LockB held)
#08 id 40289  +0x6a               <-- worker dispatch
#09 id 36360  +0xfe
... (down to id 67147)
```

Sequence:

1. `id 40706` is entered (frame #07). Its first non-prologue
   instruction at `+0x71` calls `BSSpinLock::Acquire([this+0x150])`.
   On this invocation `[this + 0x150]` resolves to LockB. **LockB is
   now held** by TID 18456.
2. `id 40706` calls `id 37388 -> id 36854 -> id 19369` (still inside
   LockB's region).
3. `id 19369` at `+0x38` calls `BSSpinLock::Acquire(LockA)`. **TID
   18456 spins on LockA**, holding LockB.

### The deadlock

```
   +------------------+                      +------------------+
   |    TID 5096      |                      |    TID 18456     |
   |  holds: LockA    |  <----- waits ---+   |  holds: LockB    |
   |  wants: LockB    | -+               |   |  wants: LockA    | <-+
   +------------------+  |               |   +------------------+   |
                         |               |                          |
                         +-> [LockB]     |    +-> [LockA] <----------+
                             held by 18456    held by 5096
```

Neither side can make progress. Both spin forever inside
`BSSpinLock::Acquire -> SleepEx -> NtDelayExecution`.

The two tail-blocked workers (TID 13052 wanting LockB, TID 28176
wanting LockA) just amplify the contention; even if they never came
into the picture, the cycle between 5096 and 18456 alone is enough.

## Why main is stalled

Main thread is parked at Site A:

- `id 34554` is the lock primitive that wraps "ask a worker to do
  one job and wait for ack".
- The Singleton-A struct holds the worker-wake event (`+0x58`,
  auto-reset) and the worker-ack event (`+0x60`, manual-reset).
- Main sets `pending=1`, calls `SetEvent(worker-wake)`,
  `WaitForSingleObjectEx(worker-ack, INFINITE)`.
- A worker thread takes the wake (auto-reset is consumed; that's why
  the freeze report shows worker-wake `NOT signaled`), descends into
  the dispatch chain, and ... gets stuck in the AB-BA cycle. The
  worker never reaches the SetEvent on the worker-ack handle.
- Main waits forever.

The Wait-Graph section's verdict line summarises this as:

```
main TID 13584 waiting on HANDLE 0x29c0 [NotificationEvent, NOT signaled]; 0 other waiters reference it.
>>> classic dispatch+wait deadlock: nobody is holding the producer side of main's handle.
```

The "producer side" the verdict refers to is whichever worker would
have called `SetEvent(worker-ack)` after finishing its job. That
worker is TID 5096, which is currently spinning on LockB.

## Why this is intermittent

Both paths exist in normal Skyrim execution. They become a deadlock
only when:

1. Two worker threads happen to be running them simultaneously.
2. Each thread reaches its "outer" lock acquisition before the other
   has released its corresponding "inner" lock.

With 32 logical CPUs and a busy worker pool (Nolvus has many mods
that produce extra work), both conditions are common but not
constant. The freezes happen every 30 minutes to a few hours of
play, which is consistent with a low-frequency race on a hot path
that is otherwise correct.

## Why standard tools missed it

- **No exception:** Both threads are doing exactly what they should
  do (spin on a lock). No access violation, no ASSERT, no abort.
  Crash loggers never fire.
- **Main looks fine to Windows:** It is happily parked in
  `WaitForSingleObjectEx` with a kernel handle. Windows' "process
  not responding" UI never appears because Skyrim's window thread
  *is* the main thread, and main is just sleeping.
- **Render thread is parked too** (waiting on its own
  Synchronization-Event handle from `id 34557`), but it is parked
  *because* main is blocked, not because it is itself locked.
- **HDT-SMP** appears on main's stack (`hdtsmp64.dll+0x42dfe`) only
  because it is hooked into `Main::Update`. It is not the holder of
  any lock; the early hypothesis that it might be is ruled out by
  reading hdtSMP's source (it tail-calls back into the original
  `Main::Update` after its own physics step) and by the fact that
  the captured wait is happening *inside* `id 35565` (the original
  `Main::Update`) at the singleton-A wait site, not inside hdtSMP's
  hook prologue.

## Why earlier freezes told us less

The Site A wait signature was visible in every freeze from the very
first one. What changed across iterations was *resolution*, not
*signal*:

- v0.1.0 told us "main is waiting on a kernel event handle".
- + `MainWaitProbe` told us "the wait is on the worker-ack of
  Singleton-A and `pending=1` so it was a real scheduled wait".
- + the BSSpinLock probe told us "while main is waiting, four worker
  threads are spinning in BSSpinLock::Acquire", but until the
  heuristic candidate detector landed we could not see *which* lock
  any individual worker was waiting on.
- + the heuristic detector and the wait-graph cross-cut closed that
  gap: live memory readout of `{owner, state}` for every plausible
  lock pointer in every spinner's register/stack window, and then
  cross-thread topology so the AB-BA pair is unmistakable.

The earlier freezes were not less informative because the bug was
different - they were less informative because the plugin could not
yet talk about lock pairs.

## Confidence in this conclusion

The end-to-end conclusion rests on:

- Static disassembly of `id 19369`, `id 40333`, `id 40706`, `id 36854`
  showing the exact `BSSpinLock::Acquire` call sites and the lock
  arguments.
- Live readout of LockA and LockB from the suspended-thread snapshot,
  showing each lock's current owner.
- Stack frames of TID 5096 and TID 18456 putting both threads in a
  position consistent with the AB-BA hypothesis (the right offset of
  `id 40333` and `id 19369` to be inside `BSSpinLock::Acquire`).
- The wait graph showing main is waiting for one of those workers
  via the well-understood Singleton-A protocol.

The remaining ambiguity is whether *every* historical Site A freeze
corresponds to this same lock pair. The 12:04 report is the only one
that has been analysed end-to-end with the AB-BA topology in mind.
Earlier reports were captured before the heuristic candidate detector
existed in its current form (the tighter state filter `<= 2` was only
added on May 19), so they do not contain comparable lock-owner
readouts. A retroactive analysis of the older logs has *not* been
performed in this investigation. It is plausible but unverified that
the same lock pair is responsible for the earlier freezes.

## See also

- `appendix-A-evidence.md` for the verbatim freeze log excerpts and
  the `xref_locks.py` output that this conclusion is built on.
- `08-mitigation.md` for proposed fixes informed by this topology.
