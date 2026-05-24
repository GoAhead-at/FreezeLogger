# Skyrim SE 1.5.97 Hard-Freeze Investigation - Case Study

**Project:** FreezeLogger SKSE Plugin (+ WorkerSpinLockFix companion)
**Period:** 2026-05-14 to 2026-05-22 (v2.0 RE + structural fix:
2026-05-22)
**Modlist:** Nolvus Awakening (Skyrim SE 1.5.97)
**Outcome:** Root cause identified (AB-BA spinlock inversion in
Skyrim's worker dispatcher), a working runtime fix shipped as
`WorkerSpinLockFix` v1.0.0 on 2026-05-21, a structural fix shipped
as `WorkerSpinLockFix` v2.0.0 on 2026-05-22 that preempts the
cycle before it can form, plus an internal v2.0.1 patch on
2026-05-23 that fixes a regression in scripted-animation
activators (skyshards) caused by a 4-arg wrap on a 6-arg engine
function, plus a v2.0.1 call-site refactor on 2026-05-24 that
shrinks the LockB-acquirer gates' blast radius from
function-wraps to two surgical call-site patches inside the
cycle hub. The v1.0 runtime breaker remains installed in v2.0.x
as defence-in-depth. See documents 23, 24, and 25 for the
v2.0.0 / v2.0.1 / v2.0.1-callsite release notes.

This folder contains the long-form case study describing how a custom SKSE
diagnostics plugin was designed, iterated on across multiple real freezes,
and ultimately used to localize a deadlock to two specific functions inside
`SkyrimSE.exe`.

## Files

- `01-overview.md` - Executive summary and problem statement.
- `02-tools.md` - Cast of tools used.
- `03-timeline.md` - Chronological investigation timeline.
- `04-plugin-evolution.md` - How the plugin's diagnostic capabilities grew.
- `05-static-analysis.md` - Ghidra / capstone / Address Library work.
- `06-root-cause.md` - The AB-BA deadlock with full evidence.
- `07-discarded-hypotheses.md` - What we were wrong about and why.
- `08-mitigation.md` - Options for fixing or working around the bug.
- `09-lessons-learned.md` - Takeaways for future engine debugging.
- `10-future-approaches.md` - Forward-looking summary of what was actually
  tried in the WorkerSpinLockFix companion plugin (v0.1 - v0.11), why each
  version failed, and what realistic paths remain.
- `11-worker-spinlockfix-retrospective.md` - Current decision record for
  WorkerSpinLockFix v0.1 - v0.15: what was tried, how each attempt behaved in
  real freeze/crash logs, and the rules that prevent repeating the same
  serialization/enumeration mistakes.
- `12-engine-fix-mod-audit.md` - Audit of two public open-source engine-fix
  mods (`EngineFixesSkyrim64` by aers, `po3-Tweaks` by powerof3): what they
  do and do not contain regarding `BSSpinLock` / dispatcher / worker
  internals, and what concrete tooling improvements (notably `safetyhook`)
  the audit produces for our project.
- `13-rethought-solution.md` - Design proposal for `WorkerSpinLockFix v1.0`,
  built on the constraints from documents 11 and 12: sub-millisecond
  ID-independent cycle detection via a `safetyhook` entry-point hook on
  `BSSpinLock::Acquire`, with the stale-owner reaper retained as a safety
  net and a parallel research track on lock-bypass via data-structure
  replacement held in reserve.
- `14-final-design-v1.md` - Final architecture as actually shipped in
  `WorkerSpinLockFix v1.0.0`. Records the two new lock-ordering
  regressions discovered during bring-up (heap allocation inside the
  detour, `safetyhook::call<>`'s internal `std::recursive_mutex`), the
  time-based confirmation flow that replaced observation-counting (and
  fixed a real production gap for clean 2-thread AB-BA cycles), and the
  synthetic AB-BA harness that proves the breaker end-to-end without
  waiting for a real engine cycle.
- `15-v2-structural-strategy.md` - Forward strategy for `WorkerSpinLockFix
  v2.0`: a structural fix that eliminates the AB-BA inversion at its
  source instead of breaking cycles at runtime. Defines a four-phase
  plan (singleton identification, class-layout reconstruction,
  access-pattern survey, implementation), the four implementation
  options (lock-free data-structure replacement, single-thread
  marshalling, comprehensive lock-order enforcement, wholesale function
  replacement), parallel research tracks, and how v1.0 stays installed
  as a defence-in-depth backstop.
- `16-v2-phase1-singletons.md` - First execution of the v2.0 Phase 1
  plan. Adds two new analysis scripts (`find_singleton_base.py`,
  `find_constructor.py`) and disassembly dumps for every known LockA
  and LockB acquirer. Findings overturn the strategy doc's assumption
  that the locks are fields of singleton classes: **both LockA and
  LockB are standalone `.data` globals with no host class**. They
  serialise *operations*, not *objects*. Re-frames Phase 2 as
  function-purpose reconstruction rather than class-layout
  reconstruction, and re-ranks the Phase 4 implementation options to
  C2 ≻ C4 ≻ C3 ≻ C1.
- `18-v2-phase2-cycle-paths.md` - Phase 2 first deliverable.
  `analysis/find_back_edges.py` builds the engine-wide call
  graph (84,126 functions, 268,185 direct edges; 7,091 vtables
  expanded) and runs forward BFS from each lock acquirer to
  the opposite set. **Both back-edges located**: LockB -> LockA
  is depth 2 via vtable dispatch (`id40285 -> [vt+0x8] -> id39816 -> id19369`
  and two siblings); LockA -> LockB is depth 3 via two
  successive vtable hops (`id19369 -> [vt+0x2f0] -> id41777 ->
  [vt+0x4f0] -> id36295 -> id40333` plus 7 more). Documents the
  two specific `cmp rdi, *(0x2eff7d8)` PlayerCharacter gates
  inside id 19369 and id 36016 that narrow when the cycle can
  fire (non-player outer operation with a player-resolved inner
  target - typical container / inventory transfer patterns).
  Surfaces two *additional* engine locks held during the
  cycle (LockC at `0x2f38aa0` inside id 36501, LockD at
  `0x2f389b0` inside id 35974), so the runtime deadlock is
  AB-BA but with C and D held throughout. Constrains the
  Phase 4 split fix: LockA -> C2 holds up unchanged
  (single-thread marshal exactly id 19369 at one safetyhook
  trampoline); LockB -> C1 needs the additional constraint
  that per-bucket locks not be held across virtual dispatches
  into form code.
- `17-v2-phase1-5-findings.md` - Phase 1.5 acquirer-side
  identification. Three new parallel scanners (`find_callers.py`,
  `match_vtable.py`, `probe_global.py`) cross 778 k address-library
  entries and 7,091 named CommonLibSSE-NG vtables. Three load-bearing
  results: (1) every direct caller of id 40285 / 40333 / 40334 /
  40335 funnels `rcx` from `*(0x1ebead0)` =
  `RE::ProcessLists::GetSingleton`, so **LockB is `ProcessLists`'s
  serialisation lock**; (2) all six acquirer functions are
  non-virtual (zero vtable hits across 7,091 vtables), so any v2.0
  redirection must happen at function entry, not via vtable patch;
  (3) the hot global at `0x2eff7d8` is `RE::PlayerCharacter`'s second
  cache pointer slot, with id 39340 / id 39341 identified as
  `PlayerCharacter::PlayerCharacter()` / `~PlayerCharacter()` via
  `VTABLE_PlayerCharacter[0]`. id 40706 is dropped from the LockB
  acquirer set on the same evidence. Phase 4 options re-graded to a
  **split fix**: LockA -> C2 (single-thread marshalling, one
  acquirer), LockB -> C1 (lock-free `ProcessLists` arrays).
- `19-v2-phase3-mutations-and-form-types.md` - Phase 3 deliverable.
  Two new analysis scripts (`enumerate_writes.py`,
  `fingerprint_form_types.py`) quantify the work that any v2.0
  fix must preserve, and identify which gameplay systems
  exercise the AB-BA cycle in the wild. Three load-bearing
  results: (1) **id 19369 is a traverser, not a mutator** - its
  only persistent write across a 0x6a0-byte body is one byte at
  `0x2eff95c` (a recursion-active flag) plus the LockA-release
  tail, so single-threading it (option C2) reorders no
  observable engine state; (2) the four ProcessLists methods
  collectively mutate **~16 instance fields plus one shared
  `{ptr,ptr}` global** at `0x2f44db0` (id 517950, unnamed in
  CommonLibSSE-NG), with the per-element work being a single
  bit-9 toggle of `[<form>+0xe0]` - the mutation surface for a
  C1 lock-free replacement is small enough to model directly;
  (3) the cycle drivers are **not generic form types**: the
  LockB -> LockA back-edge fires through `BGSProjectile`,
  `BGSOutfit`, and 58 distinct animation event handlers
  (combat, death, archery, casting, summons, flight,
  head-tracking), and the LockA -> LockB back-edge fires
  through ~40 `SkyrimScript::*Functor` Papyrus delay functors
  (MoveTo, Enable, Disable, DamageObject, RemoveItem,
  AddItem, ApplyHavokImpulse, ForceAddRemoveRagdoll, Delete,
  ResetFunctor, ...). This explains why heavy-script Wabbajack
  / Nolvus lists trigger the freeze most often, and confirms
  the split fix (LockA -> C2, LockB -> C1) is both feasible
  and cheap. Surfaces three open questions for Phase 3.5
  (consumers of `0x2f44db0`, naming the bit-9 form flag,
  rare-case acquirers).
- `20-v2-phase3-5-findings.md` - Phase 3.5 deliverable. Settles
  doc 19's two open questions and corrects one wrong claim:
  (1) the shared global `0x2f44db0` is **private to id 40285 +
  id 40334** (binary-wide scan: zero external readers), so a
  C1 LockB replacement needs no thread-local for it - the
  pairwise mutual exclusion any LockB substitute already
  guarantees is enough; (2) bit 9 of `[Actor + 0xe0]` is
  **`Actor::BOOL_BITS::kInTempChangeList`** (CommonLibSSE-NG
  `RE/A/Actor.h`), so the four ProcessLists methods translate
  cleanly to `AddToTempChangeList` (id 40333) /
  `RemoveFromTempChangeList` (id 40334) /
  `TransferBetweenTempChangeLists` (id 40285) / a fourth
  drain-or-counter helper (id 40335), and the per-element
  mutation has a direct `std::atomic<uint32_t>::fetch_or` /
  `fetch_and` substitute; (3) **the byte at `0x2eff95c` is
  function-local to id 19369** (id 19369 is the only writer
  AND the only reader across the entire binary), so the
  recursion flag can be replaced with a `thread_local` -
  this enables a C2 variant that runs id 19369 fully
  concurrently. Surfaces a **wrong claim in doc 19**: naive
  C2 (single-thread marshalling without removing LockA)
  re-forms the AB-BA cycle through the marshal queue
  (worker holds LockA, blocks on LockB; the LockB-holder
  blocks on the queue waiting for the worker). Rewrites
  the Phase 4 priority order: **C1 is the load-bearing
  fix**, with C2b (lock-scope narrowing inside id 19369)
  as a cheap defence-in-depth, and C2a (full id 19369
  rewrite + LockA elimination) reserved for if LockA
  contention is still measurable after C1.
- `21-v2-phase4-prep-dispatch-decode.md` - Phase 4 prep
  deliverable. Two new analysis tools (`read_vtable_slot.py`,
  `reachability.py`) decode id 40335 + id 40285's six dispatch
  sites and check direct-call reachability of every dispatch
  target. **Two surprises.** (1) **id 40335 is `BSSpinLock::Unlock`**
  for LockB, not a ProcessLists method - the LockB acquirer
  set shrinks from four to **three** (id 40285 / id 40333 /
  id 40334). The original `find_singleton_base.py` ranking
  was misled by release-side writes looking like acquire-side
  writes to a "function references LockB" heuristic. (2) Of
  id 40285's six dispatch sites, only **two are cycle hazards**:
  +0x17e (slot +0x558 -> Actor::id 36331 / PlayerCharacter::id 39416,
  reaches LockA at depth 8 / LockB at depth 3-4) and +0x4f7
  (slot +0x790 -> Actor::id 36614, reaches LockA at depth 2 /
  LockB at depth 4). The other four sites - one slot +0x4c8
  dispatch and three slot +0x8 destructor dispatches -
  resolve to id 14442 (TESForm dtor) or id 36484 (Actor
  vt+0x4c8) which have **zero direct-call paths to LockA or
  LockB** at depth <= 8. Doc 18's vtable-expanded BFS
  identified id 16760 (BGSOutfit) at slot +0x790, but the
  shared global `0x2f44db0` only ever stores Actors (the
  formType byte is checked against 0x3e = ActorCharacter
  before storage), so the runtime cycle through id 40285
  fires through **Actor's vtable, not BGSOutfit's**. The
  reachability analysis also surfaces a major finding:
  **id 36016 is the cycle hub** - both directions of the
  cycle and both intermediate paths flow through it
  (`id 19369 -> id 19371 -> id 35974 -> id 36016 -> id 40334`
  for LA->LB, and `id 36331 / id 36614 -> ... -> id 36016 ->
  id 40334` for two of the LB->LA paths; doc 18's
  PlayerCharacter gate at id 36016's `cmp rdi, *(0x2eff7d8)`
  is what restricts the cycle from firing on every call).
  This produces a **new Phase 4 option C5**: hook id 36016 +
  thread-local lock-held flags. Smaller patch surface than
  C1 or C2b (one function modified) and breaks both
  back-edges simultaneously. Updated Phase 4 priority:
  C5 first, C1 second, C2b third. Surfaces five sub-tasks
  for Phase 4.1 (decode id 36016 / id 36614 / id 36331 /
  id 38413 / id 19371 etc.) before C5 implementation.
- `22-v2-phase4-1-cycle-hub-characterisation.md` - Phase 4.1
  deliverable. Decoded id 36016 (1063 lines, 96-way event
  switch), id 19371 (animation event dispatcher), id 35974
  (event-emit helper that introduces a **third spinlock LockC**
  at RVA 0x2973a78 around event-sink dispatch -- not on the
  AB-BA cycle), id 19372, id 38413 (LB->LA depth-1 helper),
  and the two LockB acquirers id 40333 / id 40334 in full.
  **id 40333 = `AddToTempChangeList`** (locks LockB; bucket-
  array append + `actor->boolBits |= kInTempChangeList` +
  TLS bookkeeping). **id 40334 = `RemoveFromTempChangeList`**
  (locks LockB; bucket-array memmove + count-- +
  `actor->boolBits &= ~kInTempChangeList` + clears the
  private global at 0x2f44db0). The PlayerCharacter gate at
  id 36016 +0xd9c (`cmp rdi, [0x2eff7d8]; jne ...`) restricts
  the cycle to firing only on animation events whose hash key
  resolves to the player. **Critical correctness check passes:**
  the immediate followup at each LockB-acquirer call site
  does NOT read the fields those functions mutate (id 36016
  +0xdcb followup reads bit 30 of `[rbx+0xe0]`, NOT bit 9;
  id 19372 +0x606 followup reads only a stack flag; id 36016
  +0xfa3 has empty followup before jmp to epilogue). This
  proves a `defer-when-LockA-held` design is mutation-safe.
  **Final C5 design:** three inline hooks (wrap id 19369 to
  track thread-local LockA depth; gate id 40333 entry; gate
  id 40334 entry) plus a thread-local `std::vector` of
  deferred calls drained when id 19369's depth returns to
  zero. ~100-120 LOC total. Wins over C1 / C2b on scope,
  patch size, and semantic risk; the LB->LA direction is left
  alone (id 40285 / id 36614 / id 38413 unmodified). Phase 4
  implementation checklist included.
- `23-v2-release.md` - v2.0.0 release note. Records what
  shipped (`Phase4Defer` module: three inline hooks + per-thread
  depth counter + per-thread deferred-call queue), what changed
  in `Config.h/.cpp`, `Hooks.cpp`, `Stats.h/.cpp`, and
  `WorkerSpinLockFix.toml` between v1.0.0 and v2.0.0, the
  configuration matrix (Layer 1 + Layer 2 toggle independently),
  the in-place verification procedure (banner check ->
  steady-state telemetry -> cycle-preempted signature), and the
  outstanding work tracked for follow-up (synthetic harness for
  `Phase4Defer`, in-game smoke test on doc-19 freeze-prone
  scenarios, optional upstream PR to `EngineFixesSkyrim64`,
  reactive C5 widening if a missed cycle path is ever observed).
- `24-v2-0-1-skyshard-regression-fix.md` - v2.0.1 retrospective.
  v2.0.0 silently broke scripted-animation activators (skyshards
  the most visible). Three diagnostic cuts: (1) blamed a stale
  `kInTempChangeList` window, split the bit toggle out -- didn't
  fix it; (2) blamed a `void` wrap dropping `id 19369`'s `bool`
  return -- didn't fix it; (3) discovered `id 19369` takes
  **six args, not four**. The body reads stack args 5 and 6 at
  `[rbp+0x77]` / `[rbp+0x7f]` (six call sites: +0x47, +0x87,
  +0x2f5, +0x41f, +0x583, +0x5c1). The 4-arg wrap left those
  positions uninitialised on the trampoline call, so every
  invocation of `id 19369` ran with garbage for arg 5 and
  arg 6. The 6-arg wrap is the load-bearing fix; the bool-return
  capture and the synchronous bit toggle are kept as defensive
  scaffolding. Confirmed by direct log evidence with the new
  `[phase4_defer].diagnostic_logging` toggle:
  `arg5=0x00000001 arg6=0x00` is what the engine actually
  passes, and `phase4: queued=9 drained=9 passthrough=8725` in
  one minute of play shows the structural fix engaging on real
  cycles (no AB-BA cycle ever formed: `cycles_observed=0
  breaks_done=0`). v2.0.1 is an internal build only -- the
  version label was retained per the user's request because the
  artefact was never officially released.
- `25-v2-0-1-callsite-refactor.md` - v2.0.1 call-site refactor.
  After analysing GarrixWong's `skyrim-freeze-fix` (a separate
  engine-deadlock mod targeting `BSReadWriteLock` cell-loading
  freezes), the LockB-acquirer gates are switched from
  function-wraps on `id 40333` / `id 40334` to two surgical
  `Trampoline::write_call<5>` patches at `id 36016+0xdcb` and
  `id 19372+0x606` -- the only two call sites in the binary
  that reach `id 40333` / `id 40334` while LockA is held. Wins:
  ~26000 hot-path passthroughs per minute drop to near zero
  (only cycle-hub-reachable callers pay the gate cost), the
  function entries stay pristine so other mods that hook
  `id 40333` / `id 40334` continue to work, and any pre-existing
  call-site rewrite by another mod is detected (pre-patch
  verification of the 5-byte CALL bytes) instead of silently
  stomped. The LockA acquirer wrap on `id 19369` is unchanged --
  the depth counter still requires whole-function bracketing.
  Documents the deliberate scope split between this plugin
  (`BSSpinLock` AB-BA) and `skyrim-freeze-fix` (`BSReadWriteLock`
  cell-loading deadlocks) so users understand the two are
  complementary, not alternatives.
- `appendix-A-evidence.md` - Raw freeze logs, disassembly excerpts, register
  dumps used as evidence in the main narrative.

If you read top-to-bottom these files reconstruct the entire investigation
without needing the agent transcripts.
