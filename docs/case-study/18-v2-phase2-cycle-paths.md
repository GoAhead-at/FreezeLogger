# 18. v2.0 Phase 2 - The AB-BA Cycle Paths

**Date:** 2026-05-22
**Status:** Phase 2 first deliverable. Both back-edges located. Cycle
closes via vtable dispatch at depth 2-3.
**Inputs:**
`analysis/find_back_edges.py` (new), `analysis/dump_one_func.py`,
the disassembly dumps for every cycle-intermediate function
(`out_id19371.txt`, `out_id19372.txt`, `out_id19373.txt`,
`out_id19374.txt`, `out_id35974.txt`, `out_id36016.txt`,
`out_id36501.txt`, `out_id38152.txt`).
**Produces:**
`analysis/out_find_back_edges_direct.txt`,
`analysis/out_find_back_edges_vt.txt`.

## Why a Phase 2

Doc 17 closed Phase 1.5 with the structural identity of both
locks pinned down (LockA's sole acquirer is id 19369; LockB's
acquirer set is exactly four `RE::ProcessLists` methods) but
left one question outstanding for v2.0 design:

> Which call inside id 19369 reaches a ProcessLists method,
> and which call inside the ProcessLists family reaches
> id 19369?

That question is the difference between a structural fix that
preserves engine behaviour and one that breaks it. We need to
know which call sites are on the cycle so any C1/C2 redirect
can hold them stable.

## Method

`analysis/find_back_edges.py` builds the entire engine's
function-level call graph in one linear `.text` pass:

- Direct calls (`call <imm>`) are recorded as
  `caller_fn -> callee_fn` edges, keyed by Address Library id.
- Indirect calls (`call qword ptr [reg + disp]`) are recorded
  as `caller_fn -> indirect@disp` placeholder edges. With
  `--expand-vtables`, every named CommonLibSSE-NG vtable is
  parsed (7,091 vtables, 256 distinct slot offsets) and each
  placeholder is fanned out to the concrete function at that
  slot, capped at 4,000 candidates per slot.

Forward BFS then runs from each lock acquirer with a depth
limit, looking for any path that reaches a function in the
opposite lock's acquirer set.

## Result 1: LockB -> LockA back-edge is at depth 2

With vtable expansion, **three depth-2 paths** exist from any
LockB acquirer to id 19369:

| #  | path                                                                       |
| -- | -------------------------------------------------------------------------- |
| B1 | `id40285 -> [vtable +0x8]   -> id39816 -> id19369`                         |
| B2 | `id40285 -> [vtable +0x790] -> id16760 -> id19369`                         |
| B3 | `id40285 -> [vtable +0x8]   -> id41785 -> id19369`                         |

All three are independently confirmed - `id 39816`, `id 16760`
and `id 41785` are present in `out_find_callers.txt` from Phase
1.5 as direct callers of id 19369 (id 16760 at `0x211de4`,
id 41785 at `0x722569`, id 39816 at `0x6c035d`).

**Reading the static call site:** id 40285's body contains
several `call qword ptr [rax + 8]` instructions
(at `+0x241`, `+0x59b`, `+0x60f`) and one
`call qword ptr [rax + 0x790]` at `+0x4f7`. These are virtual
calls dispatched through whatever form object id 40285 is
currently iterating in `*(0x1ebead0)->BSTArray<...>`. When the
runtime form's vtable points its slot 1 / slot +0x790 at one of
the three intermediate functions, the call lands in id 19369
directly. id 19369 takes LockA on entry.

**Result:** while T2 is inside any of the four ProcessLists
methods (LockB held), it can re-enter id 19369 (LockA needed)
in a single virtual dispatch followed by a single direct call.
Reachability requires only that the form being iterated has its
slot 1 or slot +0x790 vtable entry pointing at id 39816 / id 16760 /
id 41785 - i.e., that the iteration cursor lands on a specific
form subtype.

## Result 2: LockA -> LockB back-edge is at depth 3

The LockA -> LockB direction is wider (BFS finds 8 distinct
depth-3 paths) but goes through the same shape of dispatch -
two vtable hops, then a direct call. The most informative paths
are:

| #  | path                                                                                              |
| -- | ------------------------------------------------------------------------------------------------- |
| A1 | `id19369 -> [vtable +0x2f0] -> id41777 -> [vtable +0x4f0] -> id36295 -> id40333`                  |
| A2 | `id19369 -> [vtable +0x2f0] -> id27458 -> [vtable +0x88]  -> id36644 -> id40333` (and id40334)    |
| A3 | `id19369 -> [vtable +0x8]   -> id55605 -> id19372 -> id40333`                                     |
| A4 | `id19369 -> [vtable +0x8]   -> id55587 -> id35492 -> id40334`                                     |

id 19369's body has `call qword ptr [rax + 8]` at multiple sites
(`+0xcf`, `+0x3a2`, `+0x3c7`) and `call qword ptr [rax + 0x2f0]`
at `+0x155`. Both slot offsets show up as the first hop above.
The second hop in A1/A2 is *another* vtable dispatch
(slot +0x4f0 / +0x88) on whatever the first call returned.
A3/A4 are simpler: the first vtable dispatch already lands in a
function (id 55605 / id 55587) that directly calls a ProcessLists
acquirer or one of the LockA-helper siblings (id 19372 / id 19373)
that itself directly calls a ProcessLists acquirer.

The id 19372 / id 19373 / id 19374 cluster is part of the same
"REFR-graph helper family" as id 19369. They are not vtable
methods (Phase 1.5 result 2: zero vtable hits) and are sibling
non-virtual helpers. id 19372 calls id 40333 directly:

```
... grep result for id19372 -> id40333 in
analysis/out_find_back_edges_*.txt confirms a direct edge ...
```

## Why direct-call BFS missed (or under-stated) the back-edges

Without vtable expansion, the same script reports a depth-4
LockA -> LockB path (`id19369 -> id19371 -> id35974 -> id36016 -> id40334`)
and a depth-5 LockB -> LockA path
(`id40285 -> id36501 -> id19374 -> id19373 -> id38152 -> id19369`).

Two issues with those direct-only paths:

1. **The depth-5 LockB -> LockA path is statically valid but
   runtime-unreachable.** id 36501's call to id 19374 at `+0x1d1`
   is gated by `test r14b, r14b; je <skip>`, where `r14b` is
   id 36501's third argument byte. id 40285 calls id 36501 at
   `+0x47e` with `r8d := 0` (set by `xor r8d, r8d` at `+0x473`).
   The branch never fires from this entry. id 36501 *is* a
   real direct caller of id 19374, but only when called from a
   different parent that supplies the flag. So the BFS edge is
   real; the runtime path through id 40285 specifically is not.
2. **The depth-4 LockA -> LockB path adds two extra locks to the
   picture.** id 35974 takes a third lock at RVA `0x2f389b0`
   ("LockD") at function entry (gated by a global flag at
   `0x2dca841`), and id 36501 takes another lock at RVA
   `0x2f38aa0` ("LockC") unconditionally. Both LockC and LockD
   sit in the same `.data` neighbourhood as LockA and LockB.
   This is fine for the AB-BA narrative - the freeze cycle our
   v1.0 detector observes is specifically between A and B - but
   it means a fully structural fix that does *not* go through
   v1.0 needs to model the LockA-LockD nesting as well, because
   T1 may end up holding `{A, D}` and T2 may end up holding
   `{B, C}` simultaneously, and any fairness scheme has to keep
   that consistent.

The vtable-expanded BFS handles (1) by routing around id 36501
entirely (every depth-2 path via vtable goes through id 39816 /
id 16760 / id 41785, not id 36501). It handles (2) by giving us
the same two-hop deadlock cycle without dragging LockC or LockD
into the path - so AB-BA truly is the relevant cycle for v2.0
design, with C and D being held throughout but never themselves
deadlocked.

## The runtime gating conditions

Two specific control-flow conditions inside id 19369 narrow
when the cycle can form:

### Condition X1 - id 19369's "non-player" branch

id 19369's body at `+0x4a2`:

```
+0x4a2  cmp rdi, qword ptr [rip + 0x2c6872f]   ; cmp rdi, *(0x2eff7d8) = PlayerCharacter*
+0x4a9  je  <skip>                              ; if rdi IS the player, skip
+0x4ae  mov rcx, rsi                            ; rcx = this REFR
+0x4b1  call 0x297490                           ; → id 19371 (one of the depth-3 LA->LB direct-call paths)
```

The direct-call back-edge (the depth-4 path) only fires when
id 19369's second arg `rdi` is a **non-player** form. The
vtable-dispatched paths above don't share this gate - they go
through `+0xcf`, `+0x155`, `+0x3a2`, `+0x3c7` instead - so the
cycle can also fire on player-involving paths via vtables.

### Condition X2 - id 36016's "is-player" branch

For the rare case where the depth-4 direct-call path *is*
exercised, id 36016's body at `+0xd9c`:

```
+0xd81  mov rdi, qword ptr [rbp + 0x20]          ; reload rdi from a local computed earlier
+0xd9c  cmp rdi, qword ptr [rip + 0x2937b55]     ; cmp rdi, *(0x2eff7d8) = PlayerCharacter*
+0xda3  jne <skip>                               ; if rdi is NOT the player, skip
+0xda5  mov rcx, qword ptr [rip + 0x295f26c]     ; rcx = ProcessLists*
+0xdac  call 0x69a9b0                            ; ← id 39342 (a PlayerCharacter method)
+0xdb5  mov rcx, qword ptr [rip + 0x295f25c]
+0xdbc  call 0x69abd0                            ; ← id 39345 (another PlayerCharacter method)
+0xdc1  mov rdx, rbx
+0xdc4  mov rcx, qword ptr [rip + 0x18f6e25]     ; rcx = ProcessLists*
+0xdcb  call 0x6d9890                            ; ← id 40334 (LockB acquirer)
```

So the depth-4 direct-call closing call is *only* taken when the
inner-computed `rdi` IS the player. Combined with X1, that means
the depth-4 direct-call cycle requires id 19369's outer
operation to be on non-player REFRs, but its inner-derived
target to *resolve to* the player. That happens during cross-
container moves where the player is the destination - e.g.
looting a corpse, transferring items to the player from a
container.

## What this tells us about v2.0 design

Phase 1.5 closed with a "split fix" recommendation: LockA -> C2
(single-thread marshal id 19369), LockB -> C1 (lock-free
ProcessLists arrays). The cycle paths now constrain that
recommendation:

### LockA -> C2 (single-thread marshal id 19369) holds up

Every depth-3 LockA -> LockB path begins at id 19369. If id 19369
runs on exactly one dedicated worker thread, then by definition
no other thread is *inside* id 19369, so the LockA -> LockB edge
of the cycle cannot exist for any other thread. The cycle
collapses: T1 (the worker) holds A; T2 holds B; T2's vtable
dispatch can still reach id 19369; but id 19369 is single-
threaded, so T2 simply enqueues and waits. There is no AB-BA -
just a normal queue.

The marshalling boundary must include id 19369 itself; not
id 19371, id 35974, id 36016, id 38152 etc., which are all
called from many non-LockA paths. The hook is exactly one
`safetyhook` inline trampoline at `0x140296c00`.

### LockB -> C1 (lock-free ProcessLists arrays) is more nuanced

The LockB -> LockA cycle direction reaches id 19369 via vtable
dispatch on a form object that the ProcessLists method is
iterating. If we keep LockB but make it finer-grained, the
LockB-side acquirers still execute on whatever thread the engine
schedules them on, and they still dispatch to id 19369. The
cycle then re-forms on the *finer* lock(s) unless the finer
locks are released before the dispatch.

Concretely, every depth-2 LockB -> LockA path reaches id 19369
through a virtual call that happens **inside the ProcessLists
method's iteration loop**. If we replace the global LockB with
per-form-bucket locks, two threads could end up holding two
different per-bucket locks while one of them virtual-dispatches
into id 19369 - the per-bucket lock is held when id 19369 wants
LockA. With LockA -> C2 in place, id 19369 is always on the
worker, so the worker would block on the per-bucket lock, not
deadlock.

So C1 on LockB only needs to ensure that **per-bucket locks are
not held across virtual calls into form code**. That is
achievable - either drop the lock before dispatch, or use a
shared/exclusive scheme that allows the virtual call to take a
shared read lock on its way through.

### Both halves still rely on v1.0 as a safety net

Until C2 is shipped and stable, v1.0's runtime AB-BA breaker is
the only thing standing between users and a hard freeze. The
per-form vtable layouts are not exhaustively documented in
CommonLibSSE-NG, and there may be additional indirect-dispatch
paths we have not enumerated. v1.0 stays installed.

## Open questions for Phase 3 (access-pattern survey)

Phase 3 is a paper exercise on the four ProcessLists method
disassemblies plus id 19369. We need:

1. **Per-acquirer mutated-field set.** For each of the four
   ProcessLists methods, list every offset of `*(0x1ebead0)`
   it reads or writes, and every offset of the iterated form
   element it reads or writes. This bounds what a lock-free
   replacement must preserve.
2. **id 19369's mutated-field set.** Same exercise on
   id 19369's body. We need to know whether the helper writes
   to fields shared with the ProcessLists methods - if so,
   single-thread marshalling alone is not enough; some form
   fields would still need synchronisation between the worker
   and the main thread.
3. **Vtable slot enumeration on the cycle.** The slot-+0x8 and
   slot-+0x790 entries that close the cycle vary by form
   subtype. List the form subtypes whose vtable[1] or
   vtable[+0x790] points at id 39816 / id 16760 / id 41785 - those are the
   form types that can drive a deadlock. (This is one
   `match_vtable.py`-style scan with the three target
   functions.)

Phase 3 has no dependency on a freeze repro and can be done
purely from the binary + CommonLibSSE-NG headers.
