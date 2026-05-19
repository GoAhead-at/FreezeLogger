# 07 - Discarded Hypotheses

Every freeze investigation produces wrong turns. Documenting them is
useful so the next one can reach the right answer faster.

## H1 - "It's the Recursion FPS Fix"

### Belief
The user had `RecursionFPSFix.dll` installed; it patches a recursion
issue in Skyrim's renderer. Early hypothesis: maybe its detection logic
or its message-box popup was misbehaving and causing the freeze.

### Evidence considered
- `RecursionFPSFix.dll` was loaded.
- The recursion-fix codebase (a sibling project we have read access to)
  contains both an early-out hot path and a popup path.

### Why we ruled it out
1. The user clarified: a *hard freeze* is observed, not a popup.
   The recursion fix only ever fires a popup; if the popup never
   appeared, the recursion fix never fired.
2. Reproduced the freeze with Recursion FPS Fix disabled. Same Site A
   wait signature.
3. Stack traces never show any `RecursionFPSFix.dll` frame on either
   the main thread or any worker.

### Lesson
"Mod is loaded and on the stack" is not evidence that "mod is the
cause". Mods that hook hot engine functions appear on every captured
stack regardless of whether they are involved.

## H2 - "It's HDT-SMP"

### Belief
`hdtsmp64.dll+0x42dfe` appeared on main's stack in the very first
freeze, sitting between the Skyrim wait-site and the rest of
`Main::Update`. Easy to read as "HDT-SMP is holding something".

### Evidence considered
- HDT-SMP frame visible at frame #04 of main's stack in the May 17
  freeze.

### Why we ruled it out
1. Read hdtSMP's source (the local `hdtSMP64/src` checkout). The
   `Main::Update` hook in HDT-SMP is a passthrough: do its physics
   step, then tail-call into the original `Main::Update` body. There
   is no lock acquisition that survives across the tail call.
2. The captured wait is happening *inside* `id 35565` (the original
   `Main::Update`) at `+0x5b35dd -> +0x5765ff`, which is far past
   HDT-SMP's tail-call point.
3. HDT-SMP frames never appear on any of the spinning worker
   threads.

### Lesson
A frame on the stack *between* the wait site and the engine entry
point is usually a hook trampoline, not a lock holder. Always check
the hook source before blaming it.

## H3 - "Site B is the same as Site A, just observed from a different
line in the wait."

### Belief
After the May 18 13:16 freeze landed at a new location
(`+0x5b34fe -> +0xc38130`), the first instinct was that this was just
an inlined alias for Site A.

### Why we ruled it out
1. `+0xc38130` is a small wrapper that ends in
   `jmp [rip+disp]` to `KERNEL32!WaitForSingleObject`. Site A goes
   through `+0x5765ff -> KERNELBASE!WaitForSingleObjectEx`. Different
   underlying API and different waiter.
2. Site A reads its handle from `[Singleton-A + 0x60]`. Site B reads
   its handle from a multi-level pointer chain through `Singleton-B`
   (`SkyrimSE+0x2f26a70`) which is nearly 0x400 bytes away in `.data`.
3. The `Main::Update` body has *two distinct call instructions* that
   reach these wrappers, at different offsets: `+0x5b35dd` (Site A)
   and `+0x5b34fe` (Site B). They are not branches of one site.

### Lesson
Two wait sites in the same function are common in dispatch-style
engines. A change of a single byte in `RIP` between freezes is a
strong signal that the wait is structurally different, not just an
inlined variant.

## H4 - "The worker is missing the wake-up because the auto-reset
event was consumed before it got a chance to wait."

### Belief
Worker-wake is auto-reset. If main set it but no worker was inside
`WaitForSingleObject(worker-wake)` at the time, an auto-reset could
fire and immediately drain. Was the dispatch protocol racy?

### Why we ruled it out
The worker-wake state in every Site A report is "NOT signaled".
That is a consistent reading: *some* worker took the signal (auto-
reset events are consumed by the first wakeup). Whoever took it has
not yet finished their job and called `SetEvent(worker-ack)`. The
production path is:

```
main: pending=1; SetEvent(worker-wake); WaitForSingleObjectEx(worker-ack, INF)
worker: WaitForSingleObjectEx(worker-wake) -> consume; do job; SetEvent(worker-ack); pending=0
```

If no worker had picked up the wake, worker-wake would still be
signaled. It is not. Therefore a worker did pick it up. It is the
worker's *job execution* that is stuck, not the dispatch protocol.

### Lesson
"Both events not signaled" actually carries a lot of information.
For a NotificationEvent (manual-reset) main is waiting on, "not
signaled" means main is correctly blocked. For a SynchronizationEvent
(auto-reset), "not signaled" + a clear pending flag means consumption
already happened.

## H5 - "The deadlock is a single recursive call inside id 19369."

### Belief
Right after spotting that `id 19369` is recursive, we briefly believed
that the freeze was a self-deadlock: thread enters `id 19369`,
acquires LockA, recurses into itself, second invocation tries to
acquire LockA again, livelocks.

### Why we ruled it out
1. `BSSpinLock::Acquire` (id 12210) supports re-entry by the same
   thread - the inline release pattern at the bottom of `id 19369`
   tests `dword [LockA] == eax` (current TID) before the
   `cmpxchg/dec` release; an outer-frame re-entry would still own
   the lock and the inner acquire would succeed instantly. The lock
   is not a strict mutex.
2. The freeze report shows two *different* threads (5096 and 18456)
   each holding a different lock. A self-deadlock would have one
   thread spinning on a lock owned by itself, not on a lock owned
   by another thread.
3. The xref scan shows LockA is private to one function and LockB is
   shared by four. A self-deadlock on LockA would be visible inside
   `id 19369` only; this freeze visibly involves both LockA and
   LockB across two threads.

### Lesson
Recursive functions *can* deadlock on themselves with the wrong lock
type, but the live-memory `owner` field is the cleanest disambiguator:
who owns the lock the spinner is waiting for? If that owner is
*another* thread, it is not a self-deadlock.

## H6 - "It's a `BSGraphics` / render-thread bug because the render
heartbeat also stalled."

### Belief
Reports labelled `_both` (both heartbeats stalled) suggested a
render-thread cause.

### Why we ruled it out
The render thread's stack consistently shows it parked at `id 34557`
(a sibling of `id 34554`) with its own SynchronizationEvent handle.
This is the standard pattern: when main is blocked, the render
thread runs its frame, then waits for main to advance the next-
frame signal. With main stalled in Site A, the render thread is
*correctly* idle; it is not the cause.

The "both stalled" label in the freeze report file name is a
heuristic in the watchdog: if both heartbeat counters fail to
advance for `threshold_ms`, both are flagged. It does not imply both
threads are stuck for the *same* reason.

### Lesson
A heartbeat that does not advance is not the same as a thread that
is stuck. Read the frames to distinguish "blocked" from "blocked
because main is blocked".

## H7 - "Maybe Singleton-B's null sub-array pointer is the bug."

### Belief
The May 18 Site B probe reported the sub-array pointer `[+8][idx0]`
went null between main's read and the probe's read. We initially
suspected a producer was clearing the field while main was sleeping.

### Why this is parked, not ruled out
We could not confirm or refute this with the data we had. The
heuristic Site-B helpers (singleton hex dump, co-consumer search,
toucher search) were added specifically to push this further on the
next Site-B freeze. As of 2026-05-19 the freezes have re-converged
on Site A, so Site B's null-pointer race is now a "future work"
item rather than an active hypothesis.

### Lesson
A diagnostic that is built to answer an unanswered question is
worth its line count even if it does not personally fire on the
*next* freeze. The Site-B probe is a sleeping asset.
