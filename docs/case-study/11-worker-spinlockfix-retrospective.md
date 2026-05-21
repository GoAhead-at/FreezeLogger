# 11 - WorkerSpinLockFix Retrospective

This document records what was tried in the `WorkerSpinLockFix` companion
plugin after the original freeze investigation identified a `BSSpinLock`
AB-BA deadlock. Its purpose is defensive: future work should not repeat the
same failed designs.

The short version:

- Static diagnosis was correct: the freezes are wait cycles around engine
  `BSSpinLock`s, especially the global LockA / LockB pair documented in
  `06-root-cause.md`.
- The prevention strategy was wrong: repeatedly trying to wrap "all functions
  that can reach LockA/LockB" turned into an unbounded enumeration problem.
- Every missed path to LockB made our own wrapper mutex become the new "other
  lock", producing a fresh AB-BA deadlock.
- The current direction, starting with v0.15, is runtime wait-graph detection:
  observe threads spinning in `BSSpinLock::Acquire`, detect stable cycles, and
  break one observed lock. This does not depend on gameplay function IDs.

## Definitions

**LockA**  
`SkyrimSE.exe + 0x2eff8e0`.

**LockB**  
`SkyrimSE.exe + 0x2f3b8e8`.

**Original AB-BA**  
One thread holds LockA and waits for LockB, while another holds LockB and waits
for LockA. The main/render threads then wait forever for worker completion.

**Wrapper mutex**  
The `std::recursive_mutex` introduced by v0.1-v0.8 and later v0.12-v0.14 to
serialize selected engine regions. The important retrospective point is that
this mutex itself became a participant in later deadlocks.

**BSSpinLock layout**

```cpp
struct BSSpinLock {
    std::uint32_t owner; // +0x0, current/last owner TID
    std::uint32_t state; // +0x4, 0 free, 1 held in observed builds
};
```

State is authoritative for "held". The owner field is not cleared on every
release, so owner alone is not a safe signal.

## Version Timeline

| Version | Strategy | Outcome | Lesson |
|---|---|---|---|
| v0.1-v0.8 | Caller-wrap `id 19369` / `id 40706` with a shared recursive mutex. | Caused new freezes. | Hooking callers assumes balanced function entry/exit. Engine control flow did not bracket the way we assumed. |
| v0.9 | Entry-point hook `BSSpinLock::Acquire` / `Release`, enforce LockA-before-LockB. | Immediate crash. | Entry-point detours on shared primitives are fragile in a modded process. |
| v0.9.1 | Same as v0.9 with chaining support. | Same crash. | Chain support does not help if load order means another plugin patches after us or the expected chain is not visible. |
| v0.10 | Call-site hook direct calls to `BSSpinLock::Acquire` / `Release`. | Freeze on save load; leaked LockA. | `BSSpinLock::Release` is heavily inlined. Direct-call coverage is insufficient for pairing acquire/release. |
| v0.11 | Stale-owner reaper only; no hooks. | Ran without introducing freezes. Did not structurally prevent live AB-BA. | Passive observation is safe; force-releasing only dead-owner locks is conservative but limited. |
| v0.12 | Serialize `id 19369` and `id 40706` by call-site wrappers with RAII. | Short-session crash class appeared only with plugin installed. | `id 40706` is extremely hot and mostly per-object. Global serialization perturbed worker timing. |
| v0.13 | Narrow `id 40706`: only serialize when `this+0x150 == &LockB`; keep `id 19369`. | Long clean run, then freeze. | Narrowing fixed the hot-path crash, but missed other direct LockB acquisition paths. |
| v0.14 | Also serialize direct LockB bracket functions `id 40285`, `id 40333`, `id 40334`. | Immediate freeze after loading a save. | Enumerating LockB paths is not comprehensive. A missed path (`id 36438` stack evidence) recreated AB-BA with our mutex. |
| v0.15 | Remove all gameplay function hooks. Watch live `BSSpinLock` wait graph and break stable cycles. | Built/deployed for testing. | This is the first design that is not based on enumerating engine gameplay functions. |

## What We Tried In Detail

### v0.1-v0.8: Caller Wrapping

The first family of builds wrapped selected callers around `id 19369` and
`id 40706` with a shared `std::recursive_mutex`. The intent was to ensure only
one worker could be inside the suspected region at once.

The failure mode was unbalanced control flow. The wrapper existed at call
sites, not at a guaranteed entry/exit boundary. Engine recursion, tail jumps,
and paths not reached via the patched call site meant the wrapper could see an
entry without a matching exit, or vice versa. Once the mutex was leaked, the
fix became a new freeze source.

Do not repeat this form. If a wrapper is needed, it must own a true function
entry/return boundary or it will eventually leak.

### v0.9-v0.9.1: Entry-Point Patching The Primitive

These versions patched the prologues of `BSSpinLock::Acquire` and
`BSSpinLock::Release` directly and attempted to enforce global lock ordering:
LockA before LockB.

They crashed immediately. The likely cause was interaction with another plugin
patching the same primitive prologues. Manual 5-byte entry detours do not
compose safely unless every participant is chain-aware and load order is
controlled. We did not have that.

Do not return to primitive entry-point detours unless there is a robust patch
coordination mechanism or a non-invasive instrumentation method.

### v0.10: Call-Site Patching Acquire/Release

v0.10 avoided entry-point collisions by patching direct `CALL`/`JMP` sites to
the primitive functions instead. This still tried to enforce LockA-before-LockB:
when acquiring LockB, auto-acquire LockA first; when releasing LockB, release
the extra LockA.

This actively caused a deadlock:

- stats showed LockB acquisitions growing while releases stayed at zero;
- the plugin had taken an extra LockA;
- the corresponding release was never observed;
- a worker eventually exited or stopped making progress while LockA remained
held;
- later threads blocked forever.

The reason is now clear: `BSSpinLock::Release` is small and heavily inlined.
Direct-call patch coverage does not include most releases. Any approach that
needs balanced primitive acquire/release observations via direct call-site
patches is unsafe.

Do not build another acquire/release pairing strategy on direct call-site
coverage.

### v0.11: Stale-Owner Reaper

v0.11 removed all hooks. It watched LockA and LockB every 250 ms and
force-released a lock only if:

- `state != 0`;
- the same `(owner, state)` remained stable for the threshold;
- the owner TID was no longer alive;
- CAS from observed state to zero succeeded.

This was the first version that looked harmless in real play. Its limitation
was deliberate: it did not break live deadlocks. If both owners were alive, it
logged/skipped rather than corrupting a live critical section.

This is the safe baseline. Future designs should preserve the principle of
minimal intervention unless a real wait cycle is observed.

### v0.12: RAII Serialization Of `id 19369` And `id 40706`

v0.12 re-attempted the original idea with better bracketing. Instead of
caller-wrap leakage, the hook function itself acquired a shared
`std::recursive_mutex`, called the original function, and released via RAII
when the original returned.

This fixed the v0.1-v0.8 bracketing bug, but introduced a new problem:
`id 40706` was much hotter than expected and was mostly not the global LockB
path. Session stats showed thousands of `id_40706` calls and substantial
contention. A new crash pattern appeared only with the fix installed, in
unrelated scene-graph code.

Interpretation: v0.12 serialized work the engine expected to run concurrently.
The crash stack did not contain our DLL because the cause was timing
perturbation, not direct execution in our code.

Do not globally serialize a hot per-object engine path.

### v0.13: Narrowed `id 40706`

v0.13 kept `id 19369` serialized but narrowed `id 40706`: only calls where
`this + 0x150 == &LockB` took the mutex; all other per-object calls passed
through.

This was a major improvement. In one clean run:

- millions of `id_40706` calls passed through;
- zero `id_40706` calls matched the LockB narrow condition;
- `id_19369` had low frequency and near-zero contention;
- no immediate crash.

Then a later freeze showed the flaw:

- one worker was inside `Hook_19369`, holding our mutex, and spinning on LockB
  through `id 40333`;
- another live thread owned LockB and blocked entering our mutex;
- the cycle was not LockA/LockB anymore, but `our mutex` / LockB.

This proved that any LockB path not covered by the mutex can turn the mutex
into the new LockA.

### v0.14: Broadened Static Coverage

v0.14 tried to fix the missed paths by adding wrappers around direct LockB
bracketing functions:

- `id 40285`;
- `id 40333`;
- `id 40334`;
- plus the earlier `id 19369`;
- plus narrowed `id 40706`.

This looked structurally sound on paper: if every path to LockA/LockB first
took our mutex, AB-BA should be impossible.

It froze almost immediately after loading a save. The freeze report showed:

- a thread inside `Hook_40333`, holding our mutex, spinning on LockB;
- the LockB owner blocked trying to enter our mutex;
- the owner path included `id 36438`, not in our hook list.

This is the critical lesson: **the list of LockB acquisition paths was not
complete, and likely cannot be made complete by simple static enumeration.**
There may be vtable dispatch, caller-specific primitive wrappers, inlined
release/acquire fragments, or paths not seen in our earlier static scan. Every
missed path recreates the same deadlock with our mutex.

Do not continue this strategy by "just adding `id 36438`". That is
whack-a-mole.

### v0.15: Runtime Wait-Graph Breaker

v0.15 removes the gameplay function hooks entirely. It does not try to prevent
deadlocks by predicting lock paths. Instead it observes the runtime state that
defines a deadlock:

```text
thread A waits on lock X
lock X is owned by thread B
thread B waits on lock Y
lock Y is owned by thread A
```

The implementation anchor is `BSSpinLock::Acquire` retry state. FreezeLogger
already proved this is observable: spinning threads have the acquire retry
return address on their stack, and their registers/stack contain candidate
`BSSpinLock*` values.

v0.15's watchdog:

1. Enumerates process threads.
2. Suspends each briefly and captures integer/control registers.
3. Detects threads spinning in `BSSpinLock::Acquire`.
4. Scans registers and a small stack window for plausible `BSSpinLock`
   pointers.
5. Builds observed wait edges: `waiter TID -> lock address -> owner TID`.
6. Keeps only stable edges.
7. Detects cycles in the wait graph.
8. If a cycle is stable long enough, force-releases one observed lock via CAS.

This is not prevention. It is recovery. The expected user-visible behavior is
a short hitch instead of a permanent freeze.

The risk is real: force-releasing a lock owned by a live thread can expose
inconsistent protected state. But this action is only taken once a stable cycle
has already made normal progress impossible. It is a last-resort recovery
mechanism, not a normal scheduling policy.

## Failed Assumptions

### "We Can Enumerate Every LockB Path"

False. We found new paths repeatedly:

- first `id 40706`;
- then direct paths like `id 40333`;
- then a freeze involving `id 36438`.

The engine and modded runtime are too large for this to be a reliable
foundation.

### "If Our Mutex Covers Enough Functions, It Cannot Deadlock"

False. Partial coverage is worse than no coverage. A missed LockB path lets a
thread hold LockB without holding our mutex. If that thread later enters a
hooked region, it waits on our mutex while still holding LockB. Another thread
inside the mutex can then wait on LockB. That is AB-BA again.

### "The Dispatcher Is The Right Layer"

Probably false. The worker dispatcher can schedule jobs, but it does not know
the lock graph inside those jobs. A dispatcher hook has only two practical
options:

- serialize worker execution globally, which risks timing crashes like v0.12;
- classify jobs/functions by which locks they might take, which returns to the
  same incomplete-enumeration problem.

The lock wait graph is the correct abstraction, not the job dispatcher.

### "A Crash Stack Must Contain Our DLL If We Caused It"

False. v0.12 likely caused a scene-graph crash through timing perturbation.
Our DLL was not in the crashing stack because the failure happened later in a
subsystem whose scheduling assumptions we disrupted.

For synchronization fixes, causal evidence includes timing changes and global
throughput changes, not just stack frames.

### "Owner Field Alone Means Held"

False. `BSSpinLock::Release` may leave the owner field stale. Use `state` as
the primary held/free signal. Owner is meaningful only when state says held.

## Design Rules Going Forward

1. **Do not add another gameplay-function serialization hook.**  
   If the fix requires a list of IDs like `19369`, `40333`, `36438`, it is not
   comprehensive enough.

2. **Do not pair primitive acquire/release by call-site coverage.**  
   Release is inlined too often.

3. **Do not patch shared primitive prologues casually.**  
   They collide with other SKSE plugins and are load-order sensitive.

4. **Do not globally serialize the worker dispatcher.**  
   It may avoid one lock bug while creating timing bugs elsewhere.

5. **Prefer runtime lock-state observation over static path prediction.**  
   A deadlock is a cycle in the wait graph. Observe that graph directly.

6. **Only force-release after stability.**  
   One transient wait edge is normal. A stable cycle is not.

7. **Log every intervention with enough topology to debug it.**  
   At minimum: cycle length, released lock address, observed owner TID,
   waiter TID, state, and edge list.

8. **Treat "live owner" releases as last-resort recovery.**  
   They can corrupt state. They are justified only when the alternative is a
   permanent hang.

## Current Status After v0.15

v0.15 is built and deployed for testing. Expected log shape:

```text
stats: scans=N threads=N spinners=N candidates=N stable_edges=N stale_reaped=N cycles=N broken=N races=N
```

Interpretation:

- `spinners=0`, `stable_edges=0`, `cycles=0`, `broken=0`: normal.
- `spinners>0`, then returns to zero: transient lock contention, normal.
- `stable_edges>0` but `cycles=0`: one or more threads are waiting on locks,
  but no closed cycle was confirmed.
- `cycles>0`, `broken>0`: watchdog detected and broke a stable deadlock.
- `races>0`: watchdog tried to force-release but the engine changed the lock
  state before CAS; usually harmless.

## What To Do If v0.15 Fails

### If The Game Freezes And `broken` Stays Zero

Collect the newest FreezeLogger report and WorkerSpinLockFix log. The detector
may be failing to identify the spinner or the correct lock candidate. Improve
the runtime observer, not gameplay hooks.

Likely improvements:

- scan a larger stack window;
- rank candidates more intelligently;
- include direct RIP range detection inside `BSSpinLock::Acquire`, not only the
  retry return address on the stack;
- cache previously observed lock addresses and prefer them in candidate
  selection;
- add a one-shot diagnostic dump of observed edges when `spinners>0` persists.

### If The Game Crashes Shortly After `broken` Increments

The lock break worked but exposed inconsistent protected state. Options:

- increase the cycle stability threshold before intervention;
- choose a different victim edge in the cycle;
- release only LockB-like locks if the address is known from repeated evidence;
- downgrade to "log-only mode" to capture more topology before acting.

Do not respond by reintroducing broad serialization hooks.

### If `spinners` Is Always Zero During A Freeze

Then the freeze is not in the `BSSpinLock::Acquire` family, or the spinner
detection anchor is wrong. Use FreezeLogger to inspect the new wait family and
build a separate detector for that primitive. Do not assume the old LockA/LockB
diagnosis explains every future freeze.

## Superseded Documents

`10-future-approaches.md` is historically useful but now outdated. In
particular:

- Approach G was attempted as v0.12-v0.14 and failed for structural reasons.
- Approach I, dispatcher replacement, remains rejected.
- Approach J evolved into v0.15, but without hardcoding only LockA/LockB or
  specific inner-acquire RVAs. The better formulation is a generic observed
  `BSSpinLock` wait-graph detector.

Use this document as the current decision record for `WorkerSpinLockFix`.

