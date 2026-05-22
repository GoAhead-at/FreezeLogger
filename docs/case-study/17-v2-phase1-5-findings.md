# 17. v2.0 Phase 1.5 - Acquirer-Side Identification

**Date:** 2026-05-22
**Status:** Phase 1.5 complete. Phase 2 unblocked.
**Inputs:**
`analysis/find_callers.py`, `analysis/match_vtable.py`,
`analysis/probe_global.py`, `analysis/dump_one_func.py`,
`include/RE/Offsets_VTABLE.h` (CommonLibSSE-NG, parsed for 7,091
named vtables).
**Produces:**
`analysis/out_find_callers.txt`, `analysis/out_match_vtable.txt`,
`analysis/out_probe_global.txt`.

## Why a Phase 1.5

Document 16 closed Phase 1 with a strong but uncomfortable conclusion:
LockA and LockB are *standalone `.data` globals* with no host class
that owns them. That ruled out implementation option **C1**
(replace the host singleton's data structure with something
lock-free), but it left three follow-up questions unanswered:

1. **Caller identity for the LockB family.** id 40285 / 40333 /
   40334 / 40335 all hold LockB on entry. If their callers all
   funnel `rcx` from the same global pointer slot, that slot
   identifies the class whose *methods* the lock serialises -
   even though the lock isn't a member of that class.
2. **Vtable-fingerprint for id 19369 (LockA).** A non-virtual
   helper inside a known class still gets cited from that class's
   vtable nowhere - but a *virtual* method override would land in
   exactly one vtable slot somewhere in `.rdata`. Confirming or
   ruling that out is one binary scan.
3. **Identity of the hot global at `0x2eff7d8`.** It's read 525
   times across 375 functions inside the LockA window, almost
   always as `cmp <reg>, qword ptr [rip + ...]`. If that global
   is e.g. the `PlayerCharacter*` pointer slot, then most of LockA's
   code path is "is this REFR the player?" branching - which
   tells us a lot about *what* LockA serialises.

All three questions are answered below. Two of the three answers
flip earlier conclusions; one of them strengthens the case for a
specific implementation option in Phase 4.

## Method

Three small Capstone-based scanners were added under
`analysis/`:

- **`find_callers.py`** - linear disassembly of `.text`,
  collecting every direct `call <rva>` whose immediate matches
  one of our six target RVAs (the four LockB acquirers, id 19369
  for LockA, and id 40706 as a control). For each match the 10
  preceding instructions are dumped so the `rcx` / `rdx` / `r8` /
  `r9` setup is visible at the call site.
- **`match_vtable.py`** - qword-aligned scan of every PE section
  for the absolute address `image_base + target_rva` for each of
  the six functions. Hits are cross-referenced against
  `Offsets_VTABLE.h` (parsed for `VTABLE_*{ REL::VariantID(SE,
  AE, VR) }` entries; 7,091 vtables resolved through the address
  library).
- **`probe_global.py`** - linear `.text` disassembly classifying
  every RIP-relative reference to `0x2eff7d8` as
  WRITE / LEA / READ / OTHER, grouped by enclosing
  function (nearest address-library id).

All three were run from a single `Shell` batch in parallel so the
30 - 50 s linear-disasm cost overlapped.

## Result 1: LockB *is* `RE::ProcessLists`'s lock

Despite LockB's storage being a `.data` global at fixed RVA
`0x2f3b8e8` rather than a member of any object, **every single
direct caller of every LockB acquirer loads `rcx` from the same
global pointer slot before calling**:

| acquirer  | RVA        | direct callers | every caller's `rcx` source |
| --------- | ---------- | -------------- | --------------------------- |
| id 40285  | `0x6d37b0` | 10             | `*(0x1ebead0)`              |
| id 40333  | `0x6d9720` | 17             | `*(0x1ebead0)`              |
| id 40334  | `0x6d9890` | 5              | `*(0x1ebead0)`              |
| id 40335  | `0x6d99d0` | 2              | `*(0x1ebead0)`              |

`*(0x1ebead0)` is the resolved RVA of address-library
`id 514167`, which CommonLibSSE-NG names
**`RE::ProcessLists::GetSingleton`** (`Offsets.h` -
`namespace ProcessLists { Singleton = RELOCATION_ID(514167, ...) }`).

So even though LockB itself is *not* a field of `ProcessLists`,
**every method that takes LockB is a method on `ProcessLists`**.
LockB is a free-standing global mutex used to serialise
`ProcessLists`-mutating code paths.

This is the missing structural identity. It re-opens
implementation option **C1**, partially: we cannot replace the
"host singleton's data structure" because LockB isn't a member of
one - but we *can* replace the data structures inside
`ProcessLists` that the four acquirers manipulate, since those
arrays / handle lists *are* class members and have a known owner.

Concretely, the four acquirers all iterate either
`BSTArray<ActorHandle>` (FormID list) or
`BSTArray<NiPointer<TESObjectREFR>>` at fixed offsets inside
`*(0x1ebead0)`. Those arrays are exactly the kind of structure
that a per-bucket lock-free or RCU scheme can replace.

### Disqualifying id 40706 from the LockB set

`out_find_callers.txt` for id 40706 (RVA `0x6ef230`) shows
something different from the four "real" LockB acquirers: its 4
direct callers do *not* funnel `rcx` from
`*(0x1ebead0)`. Two of them load
`rcx` from `[rsi + 0x148]` and the other two from a value
returned by `0x5ff590` (the dereference of a local cache).

Re-reading id 40706's prologue (`out_id40706.txt`):

```
+0x00  ...prologue, save regs into rsp+...
+0x32  lea rcx, [r14 + 0x150]   ; pointer to a BSSpinLock
+0x39  call <BSSpinLock::Lock>  ; locks *(r14 + 0x150) -- NOT &LockB
```

id 40706 takes a **per-instance** `BSSpinLock` field at offset
`+0x150` of whatever object its caller passes in. That field is
not the same lock as LockB; it just happened to be flagged in
the original `find_singleton_base.py` ranking because the
candidate-base window included a coincidental qword that
matched LockB's RVA.

**Drop id 40706 from the LockB acquirer set.** The final, audited
set is exactly four ProcessLists methods: 40285 / 40333 / 40334 /
40335.

## Result 2: All six acquirer functions are non-virtual

`match_vtable.py` scanned every PE section for any 8-byte qword
equal to `image_base + target_rva` for each of the six
functions. The result, against 7,091 named CommonLibSSE-NG
vtables:

| acquirer  | RVA        | vtable hits |
| --------- | ---------- | ----------- |
| id 19369  | `0x296c00` | **0**       |
| id 40285  | `0x6d37b0` | **0**       |
| id 40333  | `0x6d9720` | **0**       |
| id 40334  | `0x6d9890` | **0**       |
| id 40335  | `0x6d99d0` | **0**       |
| id 40706  | `0x6ef230` | **0**       |

Zero. Not in `VTABLE_PlayerCharacter`, not in
`VTABLE_TESObjectREFR`, not in any `VTABLE_ProcessLists*` entry,
not in any of the 7,091 named vtables. They are all non-virtual
methods (or, in id 19369's case, a non-member helper).

Practical consequences:

- We cannot replace any of the six functions by simply over-
  writing a vtable slot. There is no slot to overwrite.
- To redirect them at the call boundary we need either
  (a) inline trampolines at each function entry (`safetyhook`),
  or (b) trampolines at every direct call site (50 sites for
  id 19369, 34 sites for the four ProcessLists methods).
  Per-function entry hooks are clearly cheaper and the only
  realistic option.
- The four LockB acquirers are non-virtual `ProcessLists`
  methods. CommonLibSSE-NG does not currently expose names for
  them; we will assign internal names (e.g.
  `ProcessLists::DispatchActiveActorEvent_RVA_0x6d37b0`) when
  hooking.

## Result 3: The hot LockA neighbour is `PlayerCharacter*`'s second cache slot

The single hottest global inside LockA's window - 525 reads across
375 functions, almost all of the form
`cmp <reg>, qword ptr [rip + disp]` - is `0x2eff7d8`.

`probe_global.py` finds **only two writers** to that slot, in
two adjacent functions:

- id 39340 (RVA `0x699040`, size **0xdb0**) - writes its `this`
  pointer `rsi` into `0x2eff7d8` near the end of execution
  (`mov qword ptr [rip + 0x2865a02], rsi` at +0xd8f).
- id 39341 (RVA `0x699df0`, size 0xbc0) - writes `0` into
  `0x2eff7d8` near the start (`mov qword ptr [rip + 0x28658fa],
  r12` at +0xe7, with `r12 = 0`).

That access shape - one writer at end-of-function with `this`,
one writer at start-of-function with null - is the canonical
**constructor / destructor pair for a singleton object whose
pointer is cached in a `.data` slot for fast access.**

Confirming the class identity is one disassembly dump. id 39340
opens with `mov qword ptr [rsp+8], rcx; ...; mov rsi, rcx; call
<base ctor>; lea rcx, [rsi+0x2d0]; call <sub-object ctor>; ...`,
then writes a long sequence of vtable pointers. The *first*
vtable it writes, at `[rsi + 0]`, has RVA `0x16635e0`. That RVA
is **address-library `id 261916`**, which `Offsets_VTABLE.h`
names exactly:

```cpp
constexpr std::array<REL::VariantID, 17> VTABLE_PlayerCharacter{
    REL::VariantID(261916, 208040, 0x16e2230),  // <-- this slot
    ...
};
```

Therefore:

- **id 39340 is `RE::PlayerCharacter::PlayerCharacter()`** - the
  `PlayerCharacter` constructor, 0xdb0 bytes (heavily inlined).
- **id 39341 is `RE::PlayerCharacter::~PlayerCharacter()`** - the
  matching destructor.
- **`*(0x2eff7d8)` is a `RE::PlayerCharacter*` cache slot**,
  separate from the canonical `Offset::PlayerCharacter::Singleton`
  at RVA `0x2f26ef8` (id 517014). The engine maintains *two*
  pointers to the player: the documented `NiPointer<PlayerCharacter>`
  at `0x2f26ef8`, and a bare `PlayerCharacter*` at `0x2eff7d8`
  written directly by ctor / dtor.

Both pointers hold the same value once construction is complete;
the second slot exists for a fast-path comparison. The 525 reads
of `0x2eff7d8` are overwhelmingly *"is this REFR the player?"*
checks - the canonical engine fast-path that avoids dereferencing
the `NiPointer`.

### What this tells us about LockA

id 19369 (the only LockA acquirer) reads `0x2eff7d8` itself
(its body contains
`cmp rdi, qword ptr [rip + 0x2c68aaf] -> 0x2eff7d8`). Combined
with the 50-caller call-site survey from `out_find_callers.txt`,
id 19369's signature and behaviour now read clearly:

- **Six arguments**: `(this REFR*, second TESForm*, bool flag1,
  void* fourth, uint32_t fifth, bool sixth)`.
- **Body checks `rdi == player`** before branching on form types
  and inventory entries.
- **Body iterates `BSSimpleList`** at `[rsi + 0x80]; rsi+0x80;
  rsi+0x10; rsi+0x6f` etc., consistent with extra-data
  manipulation on `TESObjectREFR`.
- **Recursively calls itself** at `+0x9d` with the two REFRs
  swapped.
- Called from 50 different sites scattered across the engine.
- Not in any vtable.

This is a free-standing **REFR-graph helper**, almost certainly
something on the order of `RE::TESObjectREFR::DispatchToContainer`
or `RE::PlayerCharacter::AddItem*`'s internal recursion helper.
LockA serialises this *one specific helper*. There is exactly
one acquirer site to redirect.

LockA's structural shape is now: **one global mutex, one
acquirer function, fifty caller sites, one logical operation.**

## Re-grading the Phase 4 options

The Phase 1 doc (16) ranked the implementation options
**C2 > C4 > C3 > C1**. Phase 1.5 sharpens that ordering:

| option | description                                              | LockA fit                                            | LockB fit                                                                                          | new rank |
| ------ | -------------------------------------------------------- | ---------------------------------------------------- | -------------------------------------------------------------------------------------------------- | -------- |
| C1     | replace host singleton's data structure with lock-free   | n/a (LockA has no host)                              | viable for ProcessLists's actor / refr arrays                                                      | C1 (LockB only) |
| C2     | marshal acquirer onto a single dedicated thread          | excellent (one acquirer, fifty sites)                | painful (four acquirers, all hot)                                                                  | C2 (LockA only) |
| C3     | replace global mutex with a finer-grained / per-key lock | possible (key off `rcx == player ? 1 : 0` etc.)      | possible (key off form-id bucket)                                                                  | C3 (both)    |
| C4     | wholesale replace the function with a clean rewrite      | only if we can fully reconstruct id 19369's contract | only if we can fully reconstruct each ProcessLists method                                          | C4 (long-term)  |

The **single most attractive concrete fix that emerges from
Phase 1.5** is a *split* approach:

- **LockA -> C2** (single-thread marshalling). One acquirer
  function (id 19369) makes this clean: one `safetyhook` inline
  hook, queue the call onto a dedicated worker, block the caller
  on a future. The lock disappears because there is only ever
  one thread inside id 19369.
- **LockB -> C1** (per-bucket lock-free / RCU on the
  ProcessLists arrays). Four acquirer functions, each iterating
  arrays inside `*(0x1ebead0)`. Replace the arrays with
  appropriate concurrent containers and the four acquirers no
  longer need a shared mutex.

Both halves of the AB-BA cycle then disappear at the source. v1.0
remains installed as a backstop for any acquirer we miss.

## Open questions for Phase 2

Phase 2 starts here, with the goal of producing a clean
behavioural specification of each of the five acquirer functions:

1. **id 19369**: name (best-confidence), exact argument
   semantics, exact recursion contract (does the recursive call
   re-acquire LockA?), full set of mutated `TESObjectREFR`
   fields, whether any caller is on the BS task-graph thread vs.
   the main thread.
2. **ProcessLists family** (40285 / 40333 / 40334 / 40335):
   exact array(s) iterated by each, the per-element work
   (vtable-dispatched for each element), the legal concurrency
   for those arrays in vanilla, whether other ProcessLists
   methods *not* in this set also touch the same arrays without
   taking LockB (i.e., is LockB protecting writes only, or
   reads + writes?).
3. **LockA -> LockB inversion path**: which call inside
   id 19369 reaches a ProcessLists method (transitively, likely
   via vtable dispatch through `[rax + ...]`)? And which call
   inside the ProcessLists family reaches id 19369? Identifying
   exactly the two "back-edges" tells us the minimum behaviour
   we need to preserve in any C1 / C2 replacement.

Phase 2 will operate on the five acquirer disassemblies + the
per-caller register-setup snippets we already have in
`out_find_callers.txt`, so it is a paper exercise from here.
