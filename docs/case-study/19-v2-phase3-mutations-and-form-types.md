# 19. v2.0 Phase 3 - Mutation Surface and Cycle-Driving Form Types

**Date:** 2026-05-22
**Status:** Phase 3 complete. v2.0 split-fix is feasible; pre-conditions
documented.
**Inputs:**
`analysis/enumerate_writes.py` (new), `analysis/fingerprint_form_types.py`
(new), `out_id19369.txt`, `out_id40285.txt`, `out_id40333.txt`,
`out_id40334.txt`, `out_id40335.txt`, the cycle-intermediate function
dumps from doc 18.
**Produces:**
`analysis/out_enumerate_writes.txt`,
`analysis/out_fingerprint_form_types.txt` (~270 KB; the largest output
in the project, indexed below).

## Why a Phase 3

Doc 18 closed Phase 2 with the AB-BA cycle paths fully mapped. Phase
3 makes the structural-fix design concrete by answering two
quantitative questions:

1. **What does each lock-cycle acquirer actually mutate?** A C1
   (lock-free) replacement of the LockB-protected data structures
   only works if the mutation surface is small enough to be
   modelled as a small set of atomic operations. A C2 (single-
   thread marshalling) of id 19369 only works if id 19369's
   internal mutations don't depend on other threads observing them
   immediately.
2. **Which form / handler classes drive the cycle in practice?**
   The depth-2 and depth-3 cycle paths from doc 18 close through
   vtable dispatches whose targets vary by form subtype. Knowing
   exactly which subtypes can land on the cycle bounds the worst-
   case freeze surface and tells us which gameplay systems trigger
   it.

## Method

### `analysis/enumerate_writes.py`

For each of the five lock-cycle acquirer functions
(id 19369, id 40285, id 40333, id 40334, id 40335), linear-disasm
the function body and bucket every write-side memory access into
four categories:

- **GLOBAL** - destination operand is `[rip + disp]`.
- **INSTANCE** - destination operand is `[reg + disp]` for any
  register other than rsp / rbp.
- **ATOMIC** - any instruction with a `lock` prefix
  (cmpxchg / xadd / inc / dec / and / or / xor on memory) plus
  mfence.
- **VTABLE** - `call qword ptr [reg + disp]`, recorded for
  context (these are *reads* but tell us how the function
  dispatches).

Each instance-write group is reported with its base register so
the reader can manually tag it as "this", "iter", "scratch", etc.

### `analysis/fingerprint_form_types.py`

For every cycle-intermediate function from doc 18 (the depth-2
LockB->LockA hops `id 39816 / 16760 / 41785` and the depth-3
LockA->LockB hops `id 55605 / 55581 / 55587 / 41777 / 27458 /
43950 / 54046 / 36295 / 36644 / 36775 / 36563`), scan every
named CommonLibSSE-NG vtable's first 256 slots for a function
pointer matching the target's RVA. Report each hit as
`VTABLE_<class>` + slot offset.

A vtable that lists a target function in any of its slots tells
us the runtime form type that, when iterated by a ProcessLists
method or dispatched by id 19369 via virtual call, lands the
control flow on that target.

## Result 1: id 19369 is a traverser, not a mutator

| target  | size    | GLOBAL writes | INSTANCE writes (substantive) | ATOMIC | VTABLE dispatches              |
| ------- | ------- | ------------- | ----------------------------- | ------ | ------------------------------ |
| id19369 | 0x6a0   | 3             | 0 (only 3 callee-save spills) | 8      | +0x8 (3x), +0x150, +0x1b8, +0x2f0 |

The three global writes are:

```
+0xd6   mov byte ptr [0x2eff95c], 0       ; clear an "active" flag (id 514953, +0x7c past cluster start)
+0x5e8  mov byte ptr [0x2eff95c], 1       ; set the same flag at end of body
+0x663  mov dword ptr [0x2eff8e0], ecx    ; release LockA (epilogue, BSAutoLock dtor inlined)
```

So id 19369's *only* persistent write is a single one-byte
"recursion-active" flag at `0x2eff95c`. Every other "write" in
the function body is either:

- A spill of a callee-saved register into the caller-supplied
  shadow-space (`mov [rax + 0x10/0x18/0x20], rbx/r8b/r9` at +0x3 / +0x7
  / +0x21 - standard MSVC ABI, not engine state);
- A refcount maintenance op (`lock xadd dword ptr [rcx + 8],
  eax` at +0xbe / +0x391 / +0x3b6 - those are
  `BSReferenceCount`-style decrements on whatever object id
  19369 was referring to via local pointers).

**Implication for v2.0 / option C2 (single-thread marshal id 19369):**
moving id 19369 onto a single dedicated worker does not
reorder any persistent engine state writes, because there are
none beyond the lock release. The only cross-thread observable is
the one-byte recursion flag at `0x2eff95c`, and that is *exactly*
the kind of state that single-threading collapses trivially:
when only one thread enters id 19369 at a time, "is recursion
active?" is naturally serialised.

## Result 2: ProcessLists methods mutate <12 fields each

| target  | size  | GLOBAL writes        | substantive INSTANCE writes                           | ATOMIC                              |
| ------- | ----- | -------------------- | ----------------------------------------------------- | ----------------------------------- |
| id40285 | 0x680 | 3 (2 + LockB unlock) | 11 (`r12+0x10`, `rax+0xe0`, `rcx+0xe0`, `rdi+0`, `rdi+8`, `rsi+8`) | 5 (3 xadd + lock cmpxchg + lock dec) |
| id40333 | 0x170 | 1 (LockB unlock)     | 3 (`r14+0`, `rdx+0`, `rsi+0xe0`)                      | 3 (mfence + lock cmpxchg + lock dec) |
| id40334 | 0x140 | 2 (1 + LockB unlock) | 2 (`rbx+0x168`, `rsi+0xe0`)                           | 4 (mfence + lock cmpxchg + lock dec) |
| id40335 | 0x70  | 1 (LockB unlock)     | **0**                                                 | 3 (mfence + lock cmpxchg + lock dec) |

Three structural patterns appear in every method:

1. **Form-flag clear at `[someform + 0xe0]`** - id 40285, id 40333,
   id 40334 all do `and dword ptr [<form> + 0xe0], 0xfffffdff`
   (id 40285 and id 40334) or `or dword ptr [<form> + 0xe0],
   0x200` (id 40333). Bit 9 of `[<form> + 0xe0]` is being toggled.
   This is a `TESObjectREFR` flags-field bit, almost certainly the
   "currently being processed by ProcessLists" flag.
2. **Lock-release tail** - all four end with the same three-
   instruction sequence: `mov [rip+...], 0; mfence; lock cmpxchg
   [rip+...]; lock dec [rip+...]`. This is the inlined
   `BSAutoLock<BSSpinLock>` destructor releasing LockB. (No
   surprise.)
3. **Shared global `0x2f44db0` (id 517950)** - id 40285 writes it
   *twice* near function start (`+0x8a` and `+0xaa`), id 40334
   writes it once early (`+0x40`), neither id 40333 nor id 40335
   touches it. The pattern is `mov [rip + 0x287156f], rsi` and
   `mov [rip + 0x287154f], rcx` - two pointer-sized stores into
   adjacent slots. CommonLibSSE-NG does not name this address
   in `Offsets.h`. Working hypothesis: *current operation*
   record (a `{ this, parameter }` pair stored for the duration
   of the iteration), used by some other engine code to read
   what the iteration is currently doing without taking LockB.
   Replacing this with a thread-local or per-call structure is
   the only non-trivial part of a C1 LockB replacement.

The total write surface across all four ProcessLists methods is
**~16 instance fields and one shared `{ptr,ptr}` global**. A
lock-free / RCU / per-bucket-shared-exclusive scheme can model
this without a full rewrite.

**Implication for v2.0 / option C1 (lock-free ProcessLists arrays):**
viable. The data structures `*(0x1ebead0)` is mutating - the
`BSTArray<ActorHandle>` and `BSTArray<NiPointer<TESObjectREFR>>`
inside ProcessLists - need lock-free or shared-lock
replacements; the surrounding flag/operation-context globals
need a thread-local equivalent.

## Result 3: the cycle is driven by Papyrus script delay functors and animation events

The fingerprint scan against 7,090 named CommonLibSSE-NG vtables
returns sharp results for every cycle-intermediate function from
doc 18. The headline summary:

| function | role on cycle                          | vtable hits | dominant class group              |
| -------- | -------------------------------------- | ----------- | --------------------------------- |
| id 39816 | LockB->LockA depth-2 hop              | 4           | small functor / callback classes  |
| id 16760 | LockB->LockA depth-2 hop              | 4           | **`BGSProjectile` + `BGSOutfit`** |
| id 41785 | LockB->LockA depth-2 hop              | 58          | **animation event handlers**      |
| id 55605 | LockA->LockB depth-3 hop, level 1     | 27          | **`SkyrimScript::*Functor`**      |
| id 55581 | LockA->LockB depth-3 hop, level 1     | 11          | **`SkyrimScript::*Functor`**      |
| id 55587 | LockA->LockB depth-3 hop, level 1     | 32          | **`SkyrimScript::*Functor`**      |
| id 41777 | LockA->LockB depth-3 hop, level 1     | 50          | varied (TESForm-tier methods)     |
| id 27458 | LockA->LockB depth-3 hop, level 1     | 761         | **TESForm-tier base method**      |
| id 43950 | LockA->LockB depth-3 hop, level 1     | 158         | varied                            |
| id 54046 | LockA->LockB depth-3 hop, level 1     | 3           | varied                            |
| id 36295/36644/36775/36563 | LockA->LockB depth-3 sinks | 13/6/15/15 | varied (form-specific)         |

Two narrative patterns emerge:

### LockB -> LockA: form types and animation events drive the freeze

The depth-2 hops on this side land in:

- **id 16760**, which is `VTABLE_BGSProjectile`'s slot +0x1b8
  *and* `VTABLE_BGSOutfit`'s slot +0x790. So whenever a
  ProcessLists iteration cursor lands on a **projectile** or an
  **outfit** form, that form's vtable dispatch can re-enter
  id 19369 directly. (Two of the four `id 16760` hits are
  `BGSOutfit` factory variants - same logical class.)
- **id 41785**, which is in the slot table of 58 distinct
  animation event handlers - `ActionActivateDoneHandler`,
  `DeathStopHandler`, `PickNewIdleHandler`, `EndSummonAnimationHandler`,
  `InterruptCastHandler`, `BowZoomStartHandler` /
  `BowZoomStopHandler` / `ArrowReleaseHandler` /
  `ArrowDetachHandler` / `ArrowAttachHandler` / `BowReleaseHandler` /
  `BowDrawnHandler`, the entire flight-handling family
  (`FlightActionHandler`, `FlightLandHandler`,
  `FlightHoveringHandler`, `FlightCruisingHandler`, ...),
  `HeadTrackingOnHandler` / `HeadTrackingOffHandler`, and so on.
  An animation event firing while a ProcessLists method holds
  LockB therefore re-enters id 19369 directly.
- **id 39816**, which is in slots of four small functor classes
  (`__ActivateChoiceMenuCallbackFunctor`,
  `TargetLock__SetTargetLockFilter`, `__PlayerControlsEGMClear`,
  `hkpFirstCdBodyPairCollector`). These are generic-purpose
  callbacks and account for the three lower-hot vtable variants
  of the same back-edge.

### LockA -> LockB: Papyrus script delay functors drive the freeze

The depth-3 level-1 hops are dominated by SkyrimScript delay
functors. id 55605 alone matches 27 distinct
`VTABLE_SkyrimScript__*Functor` entries, including:

- `__MoveToFunctor` and its `ConcreteDelayFunctorFactory`
- `__MoveToOwnEditorLocFunctor` and factory
- `__SetPositionFunctor` and factory
- `__SetAngleFunctor` and factory
- `__EnableFunctor` and factory, `__DisableFunctor` and factory
- `__DamageObjectFunctor` and factory
- `__DropObjectFunctor` and factory
- `__DeleteFunctor`, `__NonLatentDeleteFunctor` and factories
- `__ApplyHavokImpulseFunctor` and factory
- `__ForceAddRemoveRagdollFunctor` and factory
- `__RemoveItemFunctor` and factory
- `__ResetFunctor`

id 55581 / id 55587 are smaller dispatchers covering a subset of
the same functor family (notably `__AddItemFunctor`,
`__DamageObjectFunctor`, `__DeleteFunctor`,
`__NonLatentDeleteFunctor`).

These are exactly the **delayed side-effect functors** that the
Papyrus VM enqueues when a script calls e.g.
`Reference.MoveTo(target)`,
`Reference.Enable()`, `Reference.Disable()`,
`Reference.Delete()`, `Reference.AddItem(...)`,
`Reference.RemoveItem(...)`,
`Reference.ApplyHavokImpulse(...)`, etc. The functors are
flushed on the main thread later, and their vtable entries
funnel through id 55605 / id 55581 / id 55587 - which then call
into id 19372 / id 19373 / id 35492 - which then call into a
ProcessLists method (LockB acquirer).

So while id 19369 (LockA holder) is doing some traversal work
(reading TESObjectREFR / TESForm graphs under LockA), if the
work it does triggers the Papyrus VM to flush a queued delay
functor (e.g. via a `BSScript::Internal::CodeTasklet` step or
similar), the functor's dispatch will reach a LockB acquirer.

### What gameplay systems exercise this cycle

The two driver lists above translate directly to the gameplay
patterns most likely to expose the deadlock:

| trigger                                      | cycle driver                                          |
| -------------------------------------------- | ----------------------------------------------------- |
| **Combat with projectiles**                  | `BGSProjectile`'s vtable +0x1b8 -> id 16760 (LB->LA)  |
| **Equipping / unequipping outfits**          | `BGSOutfit`'s vtable +0x790 -> id 16760 (LB->LA)      |
| **Bow attacks, archery animations**          | `BowDrawnHandler` / `BowReleaseHandler` /             |
|                                              | `ArrowReleaseHandler` etc. -> id 41785 (LB->LA)       |
| **Death animations**                         | `DeathStopHandler` -> id 41785 (LB->LA)               |
| **Spell casting / interrupts / summons**     | `InterruptCastHandler` / `EndSummonAnimationHandler` |
|                                              | -> id 41785 (LB->LA)                                  |
| **Flying-creature behaviour** (dragons etc.) | `FlightActionHandler` family -> id 41785 (LB->LA)     |
| **Activator menus / target-lock / EGM**      | id 39816 (LB->LA)                                     |
| **Heavy Papyrus script load** (followers,    | `MoveToFunctor`, `EnableFunctor`,                    |
| auto-loot, settlement managers, scripted     | `DamageObjectFunctor`, `RemoveItemFunctor`,          |
| events)                                      | etc. -> id 55605/55581/55587 (LA->LB)                 |

This matches the freeze profile users have reported on heavily-
modded Nolvus / Wabbajack lists: combat-heavy areas with active
followers / scripts are the worst. Empirically that's exactly
where v1.0's runtime breaker fires most often.

## Implications for v2.0 design

Phase 3 confirms the split-fix recommendation from doc 17 / 18 is
not just feasible but *cheap*:

### LockA -> C2 (single-thread marshal id 19369): minimal-impact

- **One safetyhook** at `0x140296c00`.
- The body's mutation surface is essentially zero, so marshalling
  doesn't reorder any engine-observable state.
- The 50 caller sites blocking on a future is fine because
  id 19369 is already serialised globally by LockA today - we're
  literally replacing one serialisation mechanism (a global spinlock)
  with another (a worker queue), with the same observable
  throughput.
- The runtime cost is one extra inter-thread handshake per
  invocation, which is tiny compared to id 19369's body (which
  itself does multiple vtable dispatches and recursion).

### LockB -> C1 (lock-free ProcessLists arrays): tractable

- **~16 instance fields total** to atomicise across all four
  acquirers, plus one shared `{ptr, ptr}` global at `0x2f44db0`.
- The dominant per-element mutation is one bit (`[form+0xe0]`
  bit 9) - a single CAS per element is enough.
- The `BSTArray` containers can be replaced with
  `BSTArray`-API-compatible concurrent containers (the array
  layout is small enough to mirror, since CommonLibSSE-NG
  already has the offsets via `RE::ProcessLists`).
- The `0x2f44db0` "current operation" pair becomes thread-local;
  any consumers that read it from the side need to be hooked to
  read the per-thread copy instead. (Phase 3.5: enumerate those
  consumers; one `match_qword.py` scan against the entire
  `.text` for `lea/mov [rip + ...], <ptr to 0x2f44db0>` is
  enough.)

### Combined effect

With C2 in place id 19369 is single-threaded; the LockA -> LockB
edge of the cycle disappears for *everything except the worker
thread itself*. With C1 in place LockB is gone entirely; the
LockB -> LockA edge no longer exists. Either fix on its own
breaks the AB-BA cycle, but doing both:

- removes the spinlock contention completely,
- preserves vanilla observable behaviour modulo the one-byte
  flag and the `{ptr,ptr}` global, and
- leaves v1.0 installed as a defence-in-depth backstop against
  any acquirer we have not yet enumerated.

## Open questions for Phase 3.5 and Phase 4

1. **Consumers of `*(0x2f44db0)`** - one binary-wide scan for
   `mov reg, qword ptr [rip + disp]` whose target equals `0x2f44db0`.
   Anything that reads this slot needs to be redirected to the
   thread-local equivalent.
2. **The bit-9 flag at `[form+0xe0]`** - confirm by reading
   `RE/T/TESForm.h` whether bit 9 is documented (kInProcessing,
   kBeingTraversed, etc.). If yes, the CAS is straightforward.
   If not, name it ourselves and document it in
   `WorkerSpinLockFix v2.0`'s engine-state comments.
3. **Rare-case acquirers** - id 16760 also appears in vtables we
   haven't fully classified yet (`ConcreteObjectFormFactory_BGSOutfit_124_83_1_`,
   `ConcreteFormFactory_BGSOutfit_124_`). Confirm these are
   factory-only paths that can't fire during ProcessLists
   iteration. id 41785 covers 58 animation handlers; the doc 18
   PlayerCharacter-gate observation suggests not every handler
   fires the cycle, but we have not verified that for
   non-PlayerCharacter cases.

Phase 4 (the actual implementation) can begin in parallel with
Phase 3.5; the safetyhook at `0x140296c00` is independent of the
remaining open questions.
