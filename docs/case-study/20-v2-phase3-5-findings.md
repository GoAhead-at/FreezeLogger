# 20. v2.0 Phase 3.5 - Final Constraints, and a Correction

**Date:** 2026-05-22
**Status:** Phase 3.5 complete. Re-grades the Phase 3 claim that
"either C1 or C2 on its own breaks the cycle"; only C1 is fundamentally
sufficient. C2 needs lock-scope narrowing or LockA elimination to
avoid renaming the cycle through the marshal queue.
**Inputs:**
`analysis/probe_global.py` (extended to four new targets), CommonLibSSE-NG
`RE/A/Actor.h` and `RE/T/TESForm.h`, the doc 18 / 19 disassembly dumps.
**Produces:**
`analysis/out_probe_2f44db0.txt`, `analysis/out_probe_2eff95c.txt`.

## Why a Phase 3.5

Doc 19 identified two open questions for the v2.0 design:

1. The shared global `0x2f44db0` written by id 40285 (twice) and
   id 40334 (once). Was it a read-side publication API or an
   internal scratch?
2. Bit 9 of `[<form> + 0xe0]` toggled by id 40285 / id 40333 /
   id 40334. Did CommonLibSSE-NG name it?

It also closed with a confident claim that "either C1 or C2 on its
own breaks the AB-BA cycle". On detailed re-examination of the
marshal-thread cycle topology, that claim is wrong: a naive C2 can
re-form the cycle through the marshal queue. Phase 3.5 corrects it
and lays out the *actual* sufficient conditions for each option.

## Result 1: `0x2f44db0` is private to id 40285 + id 40334

`probe_global.py` was extended to scan a 0x30-byte window around
the slot. The full access map across all 90 MB of `.text`:

| RVA          | bucket            | WRITE    | READ      | LEA      |
| ------------ | ----------------- | -------- | --------- | -------- |
| 0x2f44da0    | -0x10             | -        | -         | id 40278 |
| **0x2f44db0**| target            | id 40285 (2), id 40334 (1) | **id 40285 (19), id 40334 (1)** | -        |
| 0x2f44db8    | +0x08             | id 6313, id 40471, id 40473, id 189090, id 529500 (1 each, all `mov ..., 0`) | id 88302, id 90068 (1 each) | -        |
| 0x2f44dc0    | +0x10             | -        | id 6315 (2), id 40640 (2) | id 6315, id 40640 |
| 0x2f44dc8    | +0x18             | id 189091 | -         | id 6314  |
| 0x2f44dd0    | +0x20             | id 189092 | -         | id 6315  |

**The decisive cell is the 0x2f44db0 row.** Across the entire
binary, only **id 40285 and id 40334** read the slot. So
`0x2f44db0` is **not** a publication API: nothing reads it
"from the side". The global is a scratch slot used internally by
the same two functions that write it.

The neighbouring slots `+0x8`, `+0x10`, `+0x18`, `+0x20` are
unrelated globals; they happen to share `.data` neighbours but
they are written and read by entirely different code (id 6313
through id 6315 belong to a different subsystem; id 40471 / 40473 /
529500 are also separate).

**Implication for any LockB replacement (option C1):** the shared
global needs **no thread-local replacement and no observer
redirection**. As long as the four ProcessLists methods remain
pairwise mutually exclusive (which any correctness-preserving
LockB substitute must guarantee), the slot stays consistent
automatically. This collapses one of the doc 19 open questions to
a no-op.

## Result 2: bit 9 of `[<form> + 0xe0]` is `Actor::BOOL_BITS::kInTempChangeList`

`TESObjectREFR` itself is only `0x98` bytes; bit 9 of `[obj+0xe0]`
cannot live there. The mutating site is therefore on a subclass.
`RE/A/Actor.h` defines:

```cpp
enum class BOOL_BITS {
    ...
    kVoiceFileDone     = 1 << 8,
    kInTempChangeList  = 1 << 9,        // <-- this is it
    kDoNotRunSayToCallback = 1 << 10,
    ...
};

#define RUNTIME_DATA_CONTENT \
    stl::enumeration<BOOL_BITS, std::uint32_t>  boolBits;          /* 0E0 */ \
    ...
```

So the four ProcessLists methods are toggling `Actor::boolBits &
kInTempChangeList`. With this naming, their semantics become
unambiguous:

| function | site                                                | semantic                          |
| -------- | --------------------------------------------------- | --------------------------------- |
| id 40333 | `or  dword ptr [rsi + 0xe0], 0x200`                 | **AddToTempChangeList(actor)**    |
| id 40334 | `and dword ptr [rsi + 0xe0], 0xfffffdff`            | **RemoveFromTempChangeList(actor)** |
| id 40285 | `and [rax + 0xe0], 0xfffffdff` *and*<br>`and [rcx + 0xe0], 0xfffffdff` | **TransferBetweenTempChangeLists(srcActor, dstActor)** - clears the bit on both endpoints |
| id 40335 | (does not touch bit 9; small body)                  | likely **DrainTempChangeList()** or similar - takes LockB but not the per-actor bit |

The "TempChangeList" terminology matches the existing
`RE::ProcessLists` field `tempChangeList` in the engine layout
(distinct from `processList`, `lowProcessList`, `middleHighProcessList`,
`highProcessList`). This is the staging area for actors whose
process level is being moved between buckets, e.g. when an actor
crosses a cell boundary or a streaming-distance threshold. Toggle
of bit 9 prevents an actor from being processed twice while it's
half-moved.

**Implication for option C1**: each per-element mutation in the
LockB-protected critical sections is exactly one bit toggle. A
direct-replacement candidate in C++ is:

```cpp
// On entry to the C1 substitute for id 40333:
auto& bits = reinterpret_cast<std::atomic<std::uint32_t>&>(actor->boolBits);
bits.fetch_or(static_cast<std::uint32_t>(BOOL_BITS::kInTempChangeList),
              std::memory_order_acq_rel);

// On entry to the C1 substitute for id 40334:
bits.fetch_and(~static_cast<std::uint32_t>(BOOL_BITS::kInTempChangeList),
               std::memory_order_acq_rel);

// id 40285 is the harder case: two simultaneous clears across two
// actors. A single-actor CAS does not extend; we either fall back
// to a per-bucket lock for that one method, or perform two
// separate fetch_ands and accept that the
// add+remove transfer is not atomic from an external observer's
// point of view (probably fine since the only external observer
// is "is this actor in the list?", and the bit is already a
// transient marker).
```

`stl::enumeration` is just a wrapper over `std::uint32_t`, so the
reinterpret_cast is layout-safe.

## Result 3: the recursion flag `0x2eff95c` is a function-local of id 19369

While probing for cross-call dependencies, an additional fact
appeared. `probe_global.py` against `0x2eff95c` (the only
persistent global write inside id 19369, doc 19's "byte at +0x7c
from the cluster start"):

| access | sites                                               |
| ------ | --------------------------------------------------- |
| WRITE  | **id 19369** (2 sites; one clear `[byte] = 0`, one set `[byte] = 1`) |
| READ   | **id 19369** (2 sites; both `movzx eax, byte ptr [...]`) |

Other writers shown by the bucket query (id 19645, id 183781) all
target adjacent qword slots (`0x2eff960` / `0x2eff958`), not the
byte itself.

So `0x2eff95c` is read **only by id 19369** and written **only by
id 19369**. It is a self-recursion sentinel: id 19369 reads it on
entry, takes a "recursion-detected" branch if set, otherwise sets
it during the body and clears it before exit. Nothing observes the
flag from outside.

**Implication for option C2**: the only cross-thread observable
state inside id 19369 (besides LockA itself) is one byte of
self-recursion bookkeeping that is functionally equivalent to a
`thread_local bool`. A C++ rewrite of id 19369 that replaces the
byte with a `thread_local` does not break any external invariant.

## Correction to doc 19's "either fix on its own breaks the cycle"

The doc 19 summary said that "with C2 in place id 19369 is
single-threaded; the LockA -> LockB edge of the cycle disappears
for *everything except the worker thread itself*. With C1 in
place LockB is gone entirely; the LockB -> LockA edge no longer
exists. Either fix on its own breaks the AB-BA cycle".

The C1 half is correct. The C2 half is **wrong** as stated.
Spelling the failure mode out:

### Why naive C2 (single-thread marshal of id 19369) does NOT break the cycle

Let W be the marshal worker and assume id 19369 still acquires
LockA inside its body (we only added a hook on entry that
queues calls to W).

| step | thread | action                                                    |
| ---- | ------ | --------------------------------------------------------- |
| 1    | W      | dequeues an id 19369 call; takes LockA                    |
| 2    | W      | inside id 19369 body, vtable-dispatches into a ProcessLists method |
| 3    | W      | the ProcessLists method tries to acquire LockB. **W blocks**. |
| 4    | T2     | concurrently, T2 holds LockB inside a different ProcessLists method (LockB acquired before W started step 3) |
| 5    | T2     | vtable-dispatches to id 19369                            |
| 6    | T2     | the entry hook enqueues T2's call to W and **T2 blocks** waiting for the result |
| 7    | both   | W is waiting for LockB held by T2. T2 is waiting for the queue to drain, which requires W to finish step 1's call. **DEADLOCK.** |

So C2-as-naively-described renames the cycle through the marshal
queue. The same wait-for-loop exists, just with the queue
in T2's edge instead of LockA.

### What C2 actually needs

Two ways to fix C2:

**C2a -- LockA elimination + thread-local recursion flag.**
Patch id 19369 to never take LockA, and replace the
`0x2eff95c` byte with a `thread_local` (Result 3 says this is
safe). Now there is no global mutex on id 19369 at all; concurrent
execution is allowed. The cycle disappears because the LockA edge
is gone:

| step | thread | action                                                    |
| ---- | ------ | --------------------------------------------------------- |
| 1    | T1     | enters id 19369 (no LockA), dispatches into ProcessLists, blocks on LockB |
| 2    | T2     | holds LockB, dispatches into id 19369 → executes (no LockA), returns |
| 3    | T2     | resumes ProcessLists, eventually releases LockB           |
| 4    | T1     | acquires LockB, completes                                 |

**C2b -- lock-scope narrowing inside id 19369.**
Keep LockA, but have id 19369 release LockA before each
vtable dispatch and re-acquire it afterwards. (id 19369 has 6
dispatch sites at slots `+0x8` (3x), `+0x150`, `+0x1b8`, `+0x2f0`.)
This means LockA is never held across a dispatch, so the
LockA -> LockB edge is gone:

| step | thread | action                                                    |
| ---- | ------ | --------------------------------------------------------- |
| 1    | T1     | takes LockA inside id 19369, **releases LockA**, dispatches into ProcessLists, blocks on LockB |
| 2    | T2     | holds LockB, dispatches into id 19369 → takes LockA (free now) → executes → releases LockA → returns |
| 3    | T2     | resumes ProcessLists, releases LockB                      |
| 4    | T1     | acquires LockB, eventually re-takes LockA, completes      |

Both subvariants require either rewriting id 19369 or in-place
patching the disasm. C2a is structurally cleaner (no lock at all)
but requires a full body rewrite. C2b is in-place patches at six
specific sites.

### What C1 needs (revised)

Because the C1 LockB replacement now has the named atomic-toggle
target (`Actor::boolBits &= ~kInTempChangeList`, `|= kInTempChangeList`)
**and** because the shared global `0x2f44db0` does not need a
thread-local, the C1 design simplifies to:

1. Detour each of id 40333 / id 40334 in C++ (the smallest two,
   each `<= 0x170` bytes). Replace the body with a `std::atomic`
   toggle on `boolBits` plus the small bookkeeping the rest of the
   body does. Drop LockB.
2. Detour id 40335 (0x70 bytes, no `boolBits` write). Replace
   with a no-op or whatever its actual side effect is (the
   short body suggests it is a counter increment under LockB).
3. Detour id 40285 (0x680 bytes, the heavyweight method).
   This is the only LockB acquirer that needs real engineering:
   transfer between two TempChangeLists on two actors plus a
   handle-slot update. Either rewrite end-to-end or wrap with a
   smaller mutex *that is not held across vtable dispatches*. The
   six dispatch sites in its body are slots `+0x8` (3x), `+0x4c8`,
   `+0x558`, `+0x790`.

The shared global at `0x2f44db0` is naturally serialised by the
new mutex (since only id 40285 and id 40334 write it), so no
extra work.

## Updated Phase 4 priority

Given Phase 3.5's results, Phase 4's order changes from doc 19:

| order | option | scope                                                              | risk    |
| ----- | ------ | ------------------------------------------------------------------ | ------- |
| 1     | **C1** | rewrite id 40333, id 40334, id 40335 (small) + scope-narrow id 40285 | medium - the 4 functions are well-bounded, atomicifying `kInTempChangeList` is direct |
| 2     | **C2b** | in-place patch id 19369 to release LockA around its 6 dispatch sites | low for the patch, low for correctness - LockA still serialises the non-dispatch work |
| 3     | **C2a** | full rewrite of id 19369 with `thread_local` recursion flag       | high - body rewrite, no longer LockA-protected anywhere |

The original doc 19 ordering put C2 first because we hadn't yet
recognised it would re-form the cycle through the queue. With that
correction, **C1 becomes the load-bearing fix**; C2b is a cheap
defence-in-depth that closes the LockA -> LockB direction
independently. C2a is reserved for if measurements show contention
on LockA still hurts even after C1.

The plugin can ship any one of {C1, C2b, C1+C2b} and break the
cycle. v1.0's runtime breaker stays installed underneath all
of them as defence-in-depth.

## Open questions deferred to Phase 4

1. **id 40285's six vtable dispatches** - which engine functions
   do they reach? If any of them transitively enters another
   ProcessLists method (which would need LockB recursively), the
   "release LockB across dispatch" approach needs a recursive
   substitute lock.
2. **id 40335's body** - what does the LockB-only function
   actually do? Disasm-only inspection in Phase 3 showed zero
   instance writes plus the lock-release tail; this needs a closer
   read to confirm it is a pure synchronisation primitive (e.g. a
   barrier) rather than something we are missing.
3. **LockC at `0x2f38aa0` (id 36501) and LockD at `0x2f389b0`
   (id 35974)** identified in doc 18. Are these on the runtime
   cycle path now that we know the depth-2 / depth-3 vtable hops
   completely? If yes, the C1 design must also reason about them.

These do not block starting Phase 4 implementation -- they are
sub-tasks of detouring id 40285 specifically.
