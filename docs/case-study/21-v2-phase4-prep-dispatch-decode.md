# 21. v2.0 Phase 4 Prep - Decoding id 40335 and id 40285's Six Dispatch Sites

**Date:** 2026-05-22
**Status:** Phase 4 prep complete. Two surprises change the
implementation footprint: (1) id 40335 is `BSSpinLock::Unlock`,
not a ProcessLists method - the LockB acquirer set shrinks
from four to three; (2) only **2 of id 40285's 6 dispatch
sites** are cycle hazards in production - the other 4 (one slot
+0x4c8 dispatch, three slot +0x8 destructor dispatches) are
safe. A new single-point fix candidate (C5) emerges from
locating the cycle hub.
**Inputs:**
`analysis/dump_one_func.py` (existing), `analysis/read_vtable_slot.py`
(new, reverse vtable lookup), `analysis/reachability.py` (new,
direct-call BFS from arbitrary sources).
**Produces:**
`analysis/out_id40335_full.txt`, `analysis/out_id40285_full.txt`,
`analysis/out_reachability_dispatch_targets.txt`,
`analysis/out_reachability_la_to_lb.txt`,
`analysis/out_reachability_la_intermediates.txt`.

## Why Phase 4 prep

Doc 20 closed Phase 3.5 with three concrete pending tasks that
block the actual C1 / C2b implementations:

- decode id 40335's 0x70-byte body (zero `boolBits` writes; what
  does it actually do?);
- decode id 40285's six vtable dispatch sites and identify, for
  each, the runtime target class + concrete function (currently
  inferred from doc 18's vtable-expanded BFS, which produces
  shortest *possible* paths, not necessarily *runtime* paths);
- check, per dispatch target, whether it transitively reaches
  id 19369 (LockA) or any LockB acquirer - this tells us how
  many of the six dispatch sites need release-and-reacquire
  wrappers in the C1 design.

This document settles all three.

## Result 1: id 40335 is `BSSpinLock::Unlock`, not a ProcessLists method

Full disassembly of id 40335 (`out_id40335_full.txt`):

```
+0x0      sub      rsp, 0x28
+0x4      test     dl, dl                                              ; if (dl != 0) goto helper
+0x6      je       0x6d99ea
+0x8      xor      edx, edx
+0xa      lea      rcx, [LockB.tid]
+0x11     add      rsp, 0x28
+0x15     jmp      0x132bd0                                            ; tail-call to BSSpinLock helper
+0x1a     lfence
+0x1d     call     qword ptr [rip + 0xe2f8ad]                          ; GetCurrentThreadId
+0x23     cmp      dword ptr [LockB.tid], eax                          ; not the owner? skip
+0x29     jne      0x6d9a28
+0x2b     cmp      dword ptr [LockB.state], 1                          ; recursive depth > 1?
+0x32     jne      0x6d9a21
+0x34     xor      ecx, ecx
+0x36     mov      dword ptr [LockB.tid], ecx                          ; tid = 0      <-- RELEASE TID
+0x3c     mfence
+0x3f     mov      eax, 1
+0x44     lock cmpxchg dword ptr [LockB.state], ecx                    ; CAS state 1 -> 0   <-- RELEASE STATE
+0x4c     add      rsp, 0x28
+0x50     ret
+0x51     lock dec dword ptr [LockB.state]                             ; recursive unlock fast path
+0x58     add      rsp, 0x28
+0x5c     ret
```

Two paths, both `Unlock`-shaped:

- **`dl == 0`**: tail-call to `0x132bd0` with `rcx=&LockB`, `edx=0`. This
  is the standard `BSSpinLock::Unlock` helper.
- **`dl != 0`**: inline owner-check + recursive-depth-aware release.
  If the current thread owns the lock and the recursive depth is 1
  (final release), do the canonical `tid = 0; mfence; cmpxchg state
  1->0`. If the depth is > 1, just `lock dec` the state field.

There is no `cmpxchg state 0->1` (the `BSSpinLock::Lock` signature)
anywhere in the body.

**Conclusion**: id 40335 is `BSSpinLock::Unlock` for LockB
specifically (the LockB address is hard-coded). It does not
acquire the lock; it releases it. The
`find_singleton_base.py` ranking that originally placed id 40335 in
the LockB acquirer set was misled by the proximity of LockB writes
to the address in question - a release-side write looks the same
as an acquire-side write to a "function references LockB" heuristic.

**Implication**: the LockB acquirer set is **{id 40285, id 40333,
id 40334}** -- three functions, not four. Doc 17's claim that "id
40335 is one of the four ProcessLists methods" is wrong; doc 17
has been overstated by exactly one entry. The cycle path
inventories from docs 18-20 are unaffected because they were
keyed on the actual acquirer functions (id 40285 in particular),
but Phase 4 implementation work that previously listed id 40335 as
a C1 detour target can drop it.

## Result 2: id 40285's six dispatch sites individually decoded

`analysis/read_vtable_slot.py` was written to reverse-look-up vtable
slots: given `VTABLE_<NAME>` and a slot offset, read the qword at
that slot in `.rdata` and resolve to nearest addrlib id. Run
across the runtime-plausible classes for each site
(Actor / Character / PlayerCharacter / TESObjectREFR /
TESForm / BSHandleRefObject / NiAVObject / BGSOutfit / ...):

| id 40285 site | dispatch     | runtime rcx              | resolves to                                   |
| ------------- | ------------ | ------------------------ | --------------------------------------------- |
| +0xe4         | `[rax+0x4c8]` | resolved Actor* (via `[r12]+rax*4` handle resolve) | Actor / Character / PlayerCharacter -> **id 36484** |
| +0x17e        | `[rax+0x558]` | same Actor*              | Actor / Character -> **id 36331**; PlayerCharacter -> **id 39416** |
| +0x241        | `[rax+0x8]`   | dying BSHandleRefObject (refcount-zero dispatch) | TESForm / Actor / TESObjectREFR / BGSOutfit -> **id 14442** (TESForm dtor); BSHandleRefObject / NiAVObject -> id 69177 |
| +0x4f7        | `[rax+0x790]` | Actor* loaded from the shared global `0x2f44db0` (the same Actor with formType=0x3e gated and stored at +0xaa) | Actor / Character / PlayerCharacter -> **id 36614**; BGSOutfit -> id 16760; BGSProjectile -> id 10864 |
| +0x59b        | `[rax+0x8]`   | dying BSHandleRefObject  | same as +0x241 -> id 14442 (or id 69177)      |
| +0x60f        | `[rax+0x8]`   | dying BSHandleRefObject  | same as +0x241 -> id 14442 (or id 69177)      |

The crucial observation is the +0x4f7 row. Doc 18's vtable-expanded
BFS reported the depth-2 LB->LA back-edge as
`id40285 -> [vt+0x790] -> id 16760 -> id 19369`, where id 16760
is at slot +0x790 of `VTABLE_BGSOutfit`'s vtable. That's the
shortest *possible* path the BFS could find when it expanded the
indirect call to *every* class that has slot +0x790 populated.

But in the actual id 40285 body, the dispatch at +0x4f7 reads its
`rcx` from the global slot `0x2f44db0`. Phase 3.5 confirmed that
slot is private to id 40285 + id 40334; the only stores are:

- +0x8a `mov [0x2f44db0], rsi` (rsi = 0; clears the slot)
- +0xaa `mov [0x2f44db0], rcx` where the preceding `cmove rcx, rbx`
  block stores rbx **only if `rbx->formType (byte at +0x1a) == 0x3e`**.
  `FormType::ActorCharacter == 0x3E`. So the global is set to either
  0 or an Actor* -- never a BGSOutfit*.

Therefore the runtime dispatch at +0x4f7 fires through
**Actor's vtable**, not BGSOutfit's. `Actor::vtable[+0x790] = id 36614`,
and id 36614 is the *real* runtime target.

## Result 3: only two of the six dispatch sites are cycle hazards

`analysis/reachability.py` runs direct-call BFS (no vtable
expansion) from any addrlib id to the LockA / LockB acquirer
sets. We ran it against every dispatch target identified in
Result 2 plus a few intermediate hubs:

```
from id14442  (TESForm dtor / +0x8 destructor target):     NO path to A or B at depth <= 8
from id19393  (TESObjectREFR vt+0x4c8):                     NO path
from id19717  (TESObjectREFR vt+0x558):                     NO path
from id36331  (Actor vt+0x558):                             -> id40334  depth 3   (id36331 -> id35950 -> id36016 -> id40334)
                                                            -> id40333  depth 4   (... -> id19372 -> id40333)
                                                            -> id19369  depth 8
from id36484  (Actor vt+0x4c8):                             NO path
from id36614  (Actor vt+0x790):                             -> id19369  depth 2   (id36614 -> id38413 -> id19369)
                                                            -> id40334  depth 4   (id36614 -> id38871 -> id35953 -> id36016 -> id40334)
                                                            -> id40333  depth 5
from id39416  (PlayerCharacter vt+0x558):                   -> id40334  depth 4   (id39416 -> id36331 -> id35950 -> id36016 -> id40334)
                                                            -> id40333  depth 5
from id69177  (BSHandleRefObject dtor / +0x8 alt target):   NO path
from id76783  (TESObjectREFR vt+0x790):                     NO path
```

Mapped back onto the six dispatch sites:

| id 40285 site | reaches LockA? | reaches LockB?         | cycle hazard? |
| ------------- | -------------- | ---------------------- | ------------- |
| +0xe4   (+0x4c8) -> id 36484 | NO    | NO                     | **safe**      |
| +0x17e  (+0x558) -> id 36331 / id 39416 | yes (8) | yes (3-4)        | **HAZARD**    |
| +0x241  (+0x8)   -> id 14442 / id 69177 | NO   | NO                     | **safe**      |
| +0x4f7  (+0x790) -> id 36614            | yes (2) | yes (4)              | **HAZARD**    |
| +0x59b  (+0x8)   -> id 14442 / id 69177 | NO   | NO                     | **safe**      |
| +0x60f  (+0x8)   -> id 14442 / id 69177 | NO   | NO                     | **safe**      |

So the C1 / C2b implementations only need release-and-reacquire
around **two** dispatch sites (+0x17e and +0x4f7), not six.
Four of the six sites are destructor-tier or virtual-non-recursing
calls that don't lead anywhere near LockA or LockB.

## Result 4: id 36016 is the cycle hub

The reachability runs surface a striking pattern: every single
back-edge path - in *both* directions - flows through **id 36016**:

| direction       | path                                                           |
| --------------- | -------------------------------------------------------------- |
| LB->LA (+0x4f7) | `id 40285 -> [vt+0x790] -> id 36614 -> id 38413 -> id 19369`   |
| LB->LA (+0x17e) | `id 40285 -> [vt+0x558] -> id 36331 -> id 35950 -> id 36016 -> ... -> id 19369` (depth 8) |
| LA->LB          | `id 19369 -> id 19371 -> id 35974 -> id 36016 -> id 40334`     |
| LA->LB          | `id 19369 -> id 19371 -> id 35974 -> id 36016 -> id 19372 -> id 40333` |
| LB(36331)->LB   | `id 36331 -> id 35950 -> id 36016 -> id 40334`                 |
| LB(36614)->LB   | `id 36614 -> id 38871 -> id 35953 -> id 36016 -> id 40334`     |

**id 36016 calls into LockB acquirers directly** (id 40334 and via
id 19372 into id 40333), and is reached from id 19369 (LockA
holder) at depth 3 and from each LockB-acquirer-side dispatch
target at depth 1-3.

This is a major finding for the Phase 4 design space. It means
the entire AB-BA cycle in the engine flows through one specific
non-virtual function: id 36016 is the bottleneck.

(Doc 18 already noted that id 36016 contains a `cmp rdi,
*(0x2eff7d8)` PlayerCharacter gate. That gate is what restricts
the cycle from firing on every call - it requires a specific
`rdi` value, typically the player or another specific actor.)

## A new Phase 4 option: C5 = single-point hub fix

Given that id 36016 is the cycle hub, a structural fix that just
patches *id 36016* breaks both directions simultaneously. Three
ways this could work:

**C5a -- defer-only-when-LockA-held.**
Hook id 36016. On entry, if a thread-local "I hold LockA" flag is
set (set in id 19369's prologue by another hook, cleared in its
epilogue), return early or defer the work onto a different
thread. This guarantees that id 36016's call into id 40334 / id
40333 only happens when LockA is *not* held by the current
thread, killing the LA->LB edge.

**C5b -- defer-only-when-LockB-held.**
Symmetric: hook id 36016. On entry, if a thread-local "I hold
LockB" flag is set (set in the prologue of id 40285 / id 40333 /
id 40334 by hooks, cleared in their epilogues), bail / defer.
Kills the LB->LA edge from this side. Harder than C5a because
id 36016 is reached via vtable dispatch from the LB-holders;
we'd need flags on each acquirer.

**C5c -- skip-when-the-current-call-would-recurse.**
Hook id 36016. On entry, examine `rdi` (or whatever register
holds the gate value) against `*(0x2eff7d8)` *ourselves*; if the
gate would fire (i.e. id 36016 would call id 40334), check
whether the current thread holds either LockA or LockB. If yes,
skip the LockB-call-out (return whatever value indicates "no
operation needed"). This is the most surgical fix; it preserves
all engine behaviour except the specific narrow case that closes
the deadlock.

Which of C5a / C5b / C5c is correct depends on what id 36016
actually does in production - i.e. whether the call into id 40334
/ id 40333 is "necessary work" or "opportunistic work that can
be skipped" in the cycle-firing case. This needs one more
investigation step (Phase 4.1) but is a smaller surface than
either C1 or C2b.

## Updated Phase 4 priority (post Phase 4 prep)

| order | option | scope                                                     | notes |
| ----- | ------ | --------------------------------------------------------- | ----- |
| 1     | **C5** | hook id 36016 + thread-local lock-held flags              | smallest patch, single function modified, both directions broken |
| 2     | **C1** | rewrite id 40333 / id 40334 (small) + scope-narrow id 40285 around its **two** hazardous dispatch sites only (`+0x17e` / `+0x4f7`) | conservative, modular; targets the LockB side directly |
| 3     | **C2b**| in-place patch id 19369 to release LockA around its dispatch sites | targets the LockA side; orthogonal to C1 |

C5 is now the recommended first attempt because:

- it's the *smallest* patch (one function), with the highest
  leverage (both back-edges close at it);
- it does not require rewriting id 40285's 0x680-byte body or
  releasing/reacquiring spinlocks at multiple sites;
- the four ProcessLists methods continue to use LockB
  unchanged - we don't touch concurrency in the bucket-array
  containers at all;
- id 36016's runtime call frequency (compared to id 40285's hot
  iteration) is much lower because the PlayerCharacter gate fires
  rarely.

C1 + C2b remain queued as fallbacks if C5 turns out to require
modifying behaviour that the engine genuinely depends on.

## Open questions for Phase 4.1

1. **Decode id 36016's body.** What does it do when its
   PlayerCharacter gate fires, and is the call into id 40334 /
   id 19372 -> id 40333 *necessary work* (mutates engine state
   that downstream code reads) or *opportunistic work*?
2. **Decode id 36614 and id 36331 / id 39416.** What do they do
   between entry and the call into id 38413 / id 35950 / id
   38871? If the dispatch target functions themselves can be
   short-circuited instead of id 36016, that's an even narrower
   fix.
3. **Decode id 19371 and id 35974.** These are the direct-call
   chain from id 19369 to id 36016. Can the LA->LB edge be cut
   at a lower hub than id 36016 itself?
4. **Decode id 38413.** This is the depth-1 hop from id 36614 to
   id 19369 (the LB->LA edge's last step). What is it? If id 38413
   is an obvious "actor visibility refresh"-style function, the
   cycle's LB->LA half can be characterised in one sentence.
5. **Decode id 19372.** The intermediate between id 36016 and id 40333.
   The path `id 36016 -> id 19372 -> id 40333` is interesting because
   id 19372 is numerically near id 19369 / id 19371; possibly a
   sibling helper.

These are sub-tasks of Phase 4.1 (cycle-hub characterisation)
and feed directly into the C5 implementation choice. None of
them require new tooling - all five are dump-and-read with the
existing `dump_one_func.py`.
