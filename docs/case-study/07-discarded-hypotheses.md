# 07 - Discarded Hypotheses

Every freeze investigation produces wrong turns. Documenting them is
useful so the next one can reach the right answer faster.

## H1 - "It's the Recursion FPS Fix"

### Belief
The user had `RecursionFPSFix.dll` installed; it hooks the Papyrus
`StackFrameOverFlow` site (Address ID 98130/104853 +0x7F) and cancels
recursive calls past 1000 frames. Early hypothesis: maybe its detection
logic or its message-box popup was misbehaving and causing the freeze.

### Evidence considered
- `RecursionFPSFix.dll` was loaded.
- The recursion-fix codebase (a sibling project we have read access to)
  contains both a fast scan-and-cancel path and a notification path.

### Why "Recursion FPS Fix causes the freeze" is ruled out

We did *not* run a controlled "disable the mod and try to reproduce"
test - the user has consistently kept the mod installed. The argument
against the mod being the cause is structural rather than empirical:

1. **Wrong code path entirely.** The mod's hook is installed at
   `id 98130 +0x7F`, which is a Papyrus VM stack-frame-overflow check
   site. The deadlock lives in the worker-dispatch chain
   `id 67147 -> id 68058 -> id 68010 -> id 40289 -> id 40706 / id 19369 / id 40333`,
   which is unrelated to Papyrus VM stack handling. The two regions
   of the engine do not share locks or call into each other on the
   relevant paths.
2. **No `RecursionFPSFix.dll` frame at freeze time.** The freeze
   reports cover all 248+ threads in the process. None of them shows
   a `RecursionFPSFix.dll` frame on the stack at the moment of the
   freeze - not on the main thread, not on either spinning worker
   (TID 5096, TID 18456), not on the tail-blocked workers, not on
   any other suspended thread.
3. **The mod's trigger has never fired.** The user reports no popup
   or notification has ever been observed in either the original or
   the updated version, which means the recursion-handling code
   (Path B in the source diff in `04-plugin-evolution.md`) has never
   executed in this install. The cold path the mod adds is
   essentially a tail call to the engine's original target.

The mod is therefore not on the lock-acquiring code paths and is not
present in any frame at freeze time. It is not the cause of the AB-BA
deadlock.

Note: this conclusion would be stronger with a controlled "disable
the mod for an N-hour play session" experiment, which has not been
performed. The structural argument is sufficient to rule out the
mod-as-cause hypothesis without that test.

### Open question: empirical observation of fewer freezes with the
updated version

The user observed that freezes were noticeably less frequent with the
**updated** Recursion FPS Fix (the `recursion-fix-updated` checkout)
compared to the **original** version. We cannot fully explain this
from the source diff. Important constraint: the user reports that the
recursion threshold (Papyrus stack > 1000 frames) **has never fired**
in either version - no popup and no toast have ever been seen.

This rules out the most natural explanation. The two versions differ
substantially on the *trigger path* (modal popup vs. corner toast,
bounded vs. unbounded scan, `std::string` vs. `std::string_view`,
once-per-session suppression vs. per-event firing) but those paths
only execute when the threshold trips. If the threshold has never
tripped, the trigger-path differences are irrelevant.

On the *cold path* (the code that runs every time the hook is invoked
when the threshold is *not* exceeded), the two versions are
essentially identical:

```
old:  if (a_stack && a_stack->frames > 1000) { /* never enters */ }
      return func(...);

new:  if (a_stack && a_stack->frames > kPapyrusStackLimit
              && a_funcCallQuery != nullptr) { /* never enters */ }
      return func(...);
```

The new version performs one additional null-check per invocation.
Otherwise the cold path is the same: same hook target, same trampoline
mechanism, same tail call to the engine's original implementation.
`StackOverFlowLogHook` shows the same shape. `PCH.h` and `main.cpp`
contain only logger-configuration differences that do not affect the
hot path.

Conclusion: from the source alone, we cannot identify a mechanism
that would make the updated version reduce freeze frequency when the
recursion trigger never fires in either version.

Possible explanations (in rough order of likelihood):

1. **Statistical noise.** The AB-BA race is probabilistic and rare.
   A multi-hour play session without a freeze is consistent both with
   "the change helped" and "the change was inert and you got lucky".
   The available freeze-log corpus (~18 reports across 5 days) is not
   large enough to distinguish a true frequency reduction from
   variance.
2. **A confounding variable.** Switching versions of one mod is rarely
   the only thing that changes between play sessions: Nolvus updates,
   Windows or driver updates, different play content (combat density,
   cell complexity, NPC count), MO2 load-order changes, machine
   reboots, etc. Any of these can shift worker-pool concurrency in
   ways that are independent of Recursion FPS Fix's diff.
3. **A hidden mechanism not visible in the source.** Possible but
   speculative; we cannot point to it without further evidence.

This question is left open. If a future freeze-frequency study with
controlled conditions becomes available (same Nolvus build, same
playthrough, same hardware, alternating versions over many hours)
we may revisit the conclusion.

### Lesson
"Mod is loaded and on the stack" is not evidence that "mod is the
cause". Mods that hook hot engine functions appear on every captured
stack regardless of whether they are involved.

Equally: an empirical observation of "X version of Y mod produces
fewer freezes" is *evidence* but not *proof* of a causal mechanism.
For probabilistic engine bugs, single-machine longitudinal
comparisons are dominated by confounders and small-sample variance.
A clean technical explanation requires a measurable difference on
the code paths that actually run, not just on paths that *would*
run if the trigger fired.

## H2 - "It's HDT-SMP"

### Belief
`hdtsmp64.dll+0x42dfe` appeared on main's stack in the very first
freeze, sitting between the Skyrim wait-site and the rest of
`Main::Update`. Easy to read as "HDT-SMP is holding something".

### Evidence considered
- HDT-SMP frame visible at frame #04 of main's stack in the May 17
  freeze.

### Why we ruled it out
1. Read hdtSMP's source at `hdtSMP64/src/Hooks.cpp`. The
   `Main::Update` hook is a **wrap-around** hook, not a passthrough:

   ```cpp
   void MainHooks::Update(RE::Main* const a_this) {
       _Update(a_this);                         // call original Main::Update
       const auto& runtimeData = a_this->GetRuntimeData();
       if (runtimeData.quitGame) {
           Events::Sources::ShutdownEventEventSource::GetSingleton()->SendEvent(&e);
       } else {
           Events::FrameEvent e;
           e.gamePaused = runtimeData.freezeTime;
           Events::Sources::FrameEventSource::GetSingleton()->SendEvent(&e);
       }
   }
   ```

   That is: call original first, then dispatch a frame-event. No
   physics work, no allocations, no locks are taken in the
   pre-`_Update` portion. The HDT-SMP frame visible on main's stack
   at freeze time is simply the saved return address pointing into
   the post-`_Update` portion of this function, which has not yet
   executed because `_Update` is still blocked inside the engine.
2. The captured wait is happening *inside* `id 35565` (the original
   `Main::Update`) at `+0x5b35dd -> +0x5765ff`, which is reached
   inside the `_Update(a_this)` call on line 204 of `Hooks.cpp`.
   Whatever is causing the wait was constructed by Skyrim itself
   in that call, not by HDT-SMP's wrapper.
3. HDT-SMP frames never appear on any of the spinning worker
   threads (TID 5096, 18456, 13052, 28176). HDT-SMP does not
   participate in the worker dispatch chain at all.

### Lesson
A frame on the stack *between* the wait site and the engine entry
point is usually a hook return-address into a wrapper, not a lock
holder. Always read the hook source to understand the wrapper
shape (pre-hook, post-hook, or wrap-around) before blaming it.

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
