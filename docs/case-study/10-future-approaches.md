# 10 - Future Approaches Beyond v0.11

`08-mitigation.md` was written before the `WorkerSpinLockFix` plugin
existed. This file picks up where that one stopped: it summarises what
was actually tried, why each version failed, and what realistic
options remain. It is forward-looking and intentionally narrower than
`08-mitigation.md` - approaches that have already been proven to be
dead ends are documented here so we don't try them again.

## Where we are

| Version | Strategy                                                       | Outcome                                                                                                                                                                                            |
|---------|----------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| v0.1-v0.8 | Caller-wrap with `std::recursive_mutex` on `id 19369` / `id 40706` | Caused new freezes. Cause: hooks at the call sites of these functions did not see every entry/exit pair, and engine code paths did not bracket the way the wrapper assumed.                        |
| v0.9    | Entry-point Strategy A: hook the prologues of `BSSpinLock::Acquire` (id 12210) and `BSSpinLock::Release` (id 66983); auto-acquire LockA before LockB | Crashed on startup. Cause: collided with `skyrim-freeze-fix.dll v0.0.4`'s entry-point patch on the same prologues. Two manual 5-byte detours on the same 5 bytes cannot coexist.                    |
| v0.9.1  | v0.9 with chain-detection                                      | Same crash. Cause: chain detection only triggers when the OTHER mod patched first. Load order in this modlist puts our DLL before theirs, so on our first install the prologue still looks pristine. |
| v0.10   | Call-site Strategy A: hook every direct CALL/JMP to id 12210 (1529 sites) and id 66983 (725 sites) | Active harm: we leaked LockA. Cause: the engine compiler inlined `BSSpinLock::Release` at most call sites, so the 725 direct CALLs are a tiny fraction of all releases. Stats showed `LockB acq=3492 rel=0` after 60 s. We auto-acquired LockA on a worker, never saw the matching outer-LockB release, the worker eventually exited, LockA stayed held by a phantom TID, main deadlocked on it. |
| v0.11   | Strategy F: stale-owner reaper. No hooks. A background thread checks LockA / LockB every 250 ms; force-releases via CAS if the same `(owner, state)` persists for >2 s and the holder TID is no longer alive. | Confirmed harmless across one full session. Containment, not a structural fix.                                                                                                                       |

## Three things v0.10 taught us

These constrain every future approach.

**1. `BSSpinLock::Release` (id 66983) is heavily inlined.** The
function is 28 bytes of code padded to 48 with `int3`s. The compiler
inlined it at most call sites, leaving only 725 direct CALLs vs. 1529
direct CALLs to Acquire. Any approach that needs to count or pair
acquires with releases at the BSSpinLock level cannot work via
call-site hooking alone.

**2. Manual entry-point detours are fragile in a multi-mod
environment.** When two plugins both write a 5-byte rel32 JMP to the
same prologue without coordinating, the prologue stub one of them
copied as "the original five bytes" is actually the other one's JMP,
and executing it from a different base address does not preserve the
displacement. If we go back to entry-point hooking we need either
chain detection that works regardless of load order, or a transactional
patch protocol with the other mod (which doesn't exist).

**3. `BSSpinLock` releases never zero the owner field.** `id 66983`'s
`state==1 -> 0` path does `lock dec [rcx+4]` only. The owner field at
`[rcx+0]` is read only by the recursion check at acquire time
(`cmp [rdi], eax`); it is overwritten on the next acquire. So owner
alone is not a reliable "is this lock held" signal. State is. v0.11
already encodes this.

## Approach G - Option #1 retry, with proper RAII bracketing

**Idea.** Same shape as Option #1 in `08-mitigation.md` but applied at
the function entry, not the call site. Hook the prologues of `id
19369` and `id 40706` with one shared `std::recursive_mutex`. On entry
the wrapper acquires the mutex; on every exit (including tail-calls
and the recursion at `id 19369+0x9d`) it releases.

**Why v0.1-v0.8 didn't work.** The caller-wrap approach hooked at the
call sites and assumed acquire/release brackets perfectly. Several
engine paths violated that assumption: tail-jumps out of `id 19369`,
recursive entry through `id 40706 -> ... -> id 19369`, and at least
one error path that returns from a different RVA than the prologue
entry. Each unbracketed exit leaked the mutex; each unbracketed entry
took it without a matching release.

**What's different now.** Entry-point hooking gives us exactly one
entry per call. The exits are enumerable from the disassembly:

- `id 19369`: 5 `ret` instructions in the dump, plus one tail-jump at
  `+0x4c5` (verify before implementing).
- `id 40706`: 3 `ret` instructions, no tail-jumps in the body we've
  seen.

The clean way to express RAII over multiple exit points is a
trampoline that calls a small wrapper which itself uses
`std::scoped_lock`:

```cpp
namespace {
    std::recursive_mutex g_section;

    using Fn19369 = bool (*)(void*, void*, int, int, void*);
    Fn19369 g_orig_19369 = nullptr;

    bool __fastcall Hook_19369(void* a, void* b, int c, int d, void* e) {
        std::scoped_lock lock(g_section);
        return g_orig_19369(a, b, c, d, e);
    }
}
```

The wrapper acquires the mutex, calls the original (which can do
whatever exits it wants), and the destructor releases on the way out
of the wrapper. Engine-internal exits don't matter because they all
terminate inside the original, and the original always returns to the
wrapper.

The piece that's still not free is co-existence with v0.9's nemesis,
`skyrim-freeze-fix.dll`. That mod patches the same two prologues. Same
chain-detection problem. Two ways to address it:

- **Use call-site hooks instead of entry-point hooks.** Hook every
  direct CALL to `id 19369` and `id 40706`. Far fewer sites than for
  the BSSpinLock primitives (estimate: dozens, not thousands), and
  CommonLibSSE-NG's `write_call<5>` chains correctly with other mods'
  call-site patches. This sidesteps the prologue-collision problem.
  The disassembly counter for `id 19369` and `id 40706` direct call
  sites needs to be run before commitment.
- **Order our hook explicitly via load priority.** If we install
  AFTER `skyrim-freeze-fix.dll`, our chain detection in v0.9.1 works.
  This requires SKSE plugin priority manipulation that is unreliable
  across MO2 / Vortex.

The call-site variant is the more conservative choice.

**Risks.** Performance overhead from globally serialising entries to
two engine functions that the worker pool calls into often. The
freeze logs suggest the deadlock-prone region is short
(microseconds), so the contention should not be measurable on a
32-core machine. On lower-end machines this is the option's main
cost.

**Status.** Not yet attempted with proper bracketing. This is the
default next step if a structural fix is desired.

## Approach H - Option #3, inner-acquire guard

**Idea.** Don't serialise entries to `id 19369` and `id 40706`. Just
prevent the inner acquisitions from forming a cycle. Set a per-thread
flag on entry to each function; clear it on exit. At the inner
`BSSpinLock::Acquire` call site (LockB inside `id 19369`'s descent
through `id 40333 +0x2b`; LockA inside `id 40706`'s descent through
`id 19369 +0x38`), check the opposing flag. If set, yield instead of
calling the original spin-acquire.

**Why this is interesting.** It does not require hooking
`BSSpinLock::Release` at all, which avoids v0.10's release-inlining
problem entirely. We only intercept Acquire, and only at two specific
sites where the second lock would form the cycle.

**Why we have not built it.** It needs the same per-function entry
hook as Approach G to set/clear the flag (otherwise we don't know
"are we inside `id 19369`?"), so it inherits the chaining-with-another-
mod question from G. Once that's solved the rest is small: two
thread_locals, two call-site `write_call<5>` patches at the inner
acquire sites, a small cooperative-yield loop.

**Trade-off vs G.** H lets workers run in full parallelism outside
the deadlock-prone region, which G does not. The deadlock-prone
region is short, so the parallelism win is small. H is more code than
G for not much extra benefit. The reason to prefer H is that it
removes a class of mistake (over-serialisation) that G has. The
reason to prefer G is simplicity.

**Status.** Not attempted. Worth building only if Approach G shows
measurable contention in profiling.

## Approach I - Replace the engine's worker dispatcher

**Idea.** Hook `id 67147` (worker thread entry, `BSJobs::JobThread::
Run`-ish), `id 68058` (pool dequeue helper), `id 68010` (dispatch
helper), and `id 40289` (job dispatch wrapper). Substitute our own
queue and worker pool. The producer side still calls into the engine's
`QueueTask`-like API, but we redirect those queues to our own
implementation.

**Why it sounds attractive.** A custom dispatcher could enforce any
lock-ordering invariant we want, including "never schedule two jobs
that take LockA and LockB concurrently".

**Why it's the wrong layer.** The deadlock is between engine functions
`id 19369` and `id 40706`, not between dispatcher invocations. Both
functions are called from many places besides the worker pool dispatch
chain - including from each other recursively. A custom dispatcher
that preserves engine semantics will still call those functions and
the AB-BA can still form. The only way a custom dispatcher prevents
the deadlock is by being **strictly serial**: at most one worker job
in flight at a time. That is functionally equivalent to Approach G but
implemented at the wrong layer with vastly more code.

**Why it's a substantial cost.**

- The job descriptor format inside `id 40289` is opaque. It contains
  vtable pointers, embedded state, completion handles. Reverse-
  engineering enough of it to safely replicate is on the order of
  weeks.
- The producer side reaches into the queue's internals from
  unrelated subsystems (queue length, worker counts, completion-event
  handles). All of those would need to be either re-served by our
  implementation or replaced where the producers read from them.
- Every other plugin that hooks the worker pool (HDT-SMP,
  ConsoleUtilSSE indirectly via Papyrus, parts of PapyrusTweaks)
  now has to coexist with our dispatcher. Some of them won't.
- Job bodies still touch the same global engine state via the same
  subsystem locks. A new scheduler does not change that.

**Status.** Documented and rejected. This option is here so that
"replace the dispatcher?" doesn't get re-proposed without remembering
why it doesn't pay off.

## Approach J - Hybrid: reaper plus live-AB-BA detector

**Idea.** Keep v0.11's reaper exactly as it is. Add a second branch
that handles the case Strategy F was deliberately conservative about:
LockA and LockB are both held, by different live threads, for >N
seconds, with each holder's stack containing the inner-acquire RVA of
the other lock. That's the live AB-BA topology described in `06-root-
cause.md`. When detected, force-release one side (lower TID, by
convention).

**Why we did not put this in v0.11.** Force-releasing a lock held by a
*live* thread is qualitatively different from reaping a phantom. The
live thread expects to own the lock for the rest of its critical
section. Stripping that ownership underneath it can corrupt the engine
state that lock was protecting. The freeze itself is bad; corrupted
engine state is potentially worse.

**What would make it worth doing.** If we observe, via
`FreezeLogger`, a confirmed live-AB-BA freeze that does NOT
self-recover (the engine never makes progress for tens of seconds and
both holders stay alive), then a forced break-out is preferable to a
hang. We don't have that evidence yet from a v0.11-active session.

**Implementation sketch.** Extend `Hooks::WatchdogBody` with one
extra check after `IsThreadAlive(owner)` returns true:

```cpp
if (BothLocksHeldByDifferentLiveTids(now_ms) &&
    EachHolderStackContainsOpposingInnerAcquire())
{
    BreakDeadlock(/*lower_tid_loses=*/true);
}
```

The "stack contains opposing inner acquire" check is the
discriminator that prevents false positives on long but legitimate
critical sections. We would suspend each holder briefly, walk the
stack with `StackWalk64` (same primitive `FreezeLogger` already uses),
look for the RVAs of `id 40333+0x2b` and `id 19369+0x38`, then
resume. If both RVAs match across the two threads and the address-of
relations look right, we have AB-BA.

**Status.** Designed but not built. Builds on top of v0.11 cleanly;
does not invalidate the existing reaper.

## Recommended progression

1. Keep `WorkerSpinLockFix v0.11` deployed indefinitely. It is
   cost-free, proven harmless, and the only remediation for the
   phantom-owner failure mode that we've actually observed in v0.10.
2. If a structural fix is wanted, build **Approach G with call-site
   hooking** as the next plugin version (v0.12). Side-by-side with
   v0.11 (the two are not in tension). Verify the call-site hook
   coexists with `skyrim-freeze-fix.dll` before going live.
3. Do not pursue Approach I.
4. Build Approach J only if a v0.11-active session captures a live-
   AB-BA freeze that fails to self-recover.

## Out of scope: the 18:27:46 worker crash

The crash captured in `crash-2026-05-19-16-27-46.log` (real time
2026-05-19 18:27:46) is not in this family. Summary for the record:

- Faulting instruction: `call [rax+0x158]` at `id 52655 +0x6C`,
  reading `0xFFFFFFFFFFFFFFFF` (a freed/uninitialised vtable slot).
- Faulting thread: a worker, running through `id 67147 -> id 68058
  -> id 68010` into the Papyrus VM dispatch chain (`id 38118 ...
  id 52655`).
- `PapyrusTweaks v4.1.1`'s `LoggerHooks::ValidationSignaturesHook::
  thunk` is on the stack at `RSP+3A8`.
- Stack data references `BSScript::Internal::VirtualMachine*` and a
  `BGSKeyword` from `Next-Gen Decapitations.esp`.
- `WorkerSpinLockFix.dll` is loaded but does not appear in any frame,
  register, or RSP slot. v0.11's stats line at the closest 60 s
  boundary read `LockA reaped=0 live_skips=0 races=0; LockB reaped=0
  live_skips=0 races=0`. The plugin was not implicated.

The signature is a Papyrus VM use-after-free: a queued function call
against a Form whose `IFunction` (or surrounding object) was torn
down before the worker pool dispatched the call. Likely trigger
window: NPC death or cell transition, where `Next-Gen Decapitations`
does aggressive script work and `PapyrusTweaks`'s validation hook
adds an extra dereference on the call path.

This is a separate engineering problem from the AB-BA work. It does
not motivate any of Approaches G-J. If it recurs the targets to
investigate are PapyrusTweaks's `ValidationSignatures` feature
interacting with NextGenDecapitations, not the BSSpinLock layer.

## See also

- `06-root-cause.md` for the AB-BA topology and the lock pair
  diagram.
- `07-discarded-hypotheses.md` for prior dead-end approaches.
- `08-mitigation.md` for the original Option #1 / #3 framing this
  document refines.
- `skyrim-freeze-fix/src/Hooks.cpp` for the v0.11 reaper as
  shipped.
