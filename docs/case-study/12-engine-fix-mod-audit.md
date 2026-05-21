# Engine Fix Mod Source Audit

**Date:** 2026-05-21
**Sources audited:**
- `EngineFixesSkyrim64` — by aers (`https://github.com/aers/EngineFixesSkyrim64`).
- `po3-Tweaks` — by powerof3 (`https://github.com/powerof3/po3-Tweaks`).

**Purpose:** before committing more engineering effort to original reverse
engineering of Skyrim SE 1.5.97 worker-thread internals, audit the two most
prolific public open-source mods that touch the engine to determine:

1. whether either author has already mapped or fixed the AB-BA `BSSpinLock`
   deadlock between LockA (`SkyrimSE+0x2eff8e0`) and LockB
   (`SkyrimSE+0x2f3b8e8`),
2. whether they expose annotated engine struct definitions, RVA tables, or
   semantic comments for the dispatcher / worker subsystem we can reuse,
3. whether they offer hook idioms or libraries that solve known
   `WorkerSpinLockFix` problems (multi-mod prologue collision, inlined
   `BSSpinLock::Release`, etc.),
4. whether they suggest higher-level architectural strategies we have not
   yet considered.

This document records the conclusions so we do not repeat the audit and so
the negative findings are explicit.

---

## 1. The single most important finding (negative)

Across both repositories there is **zero source-code reference to**:

- `BSSpinLock`, spin lock, or any spin-loop primitive,
- the LockA / LockB RVAs (`0x2eff8e0`, `0x2f3b8e8`),
- `BSTaskPool`, `BSJobs2`, `BSWorkerThread`, dispatcher, or worker-thread
  internals,
- the word "deadlock" in any context.

In `po3-Tweaks` the only synchronization primitive in the entire repository
is one `std::mutex` in `src/Cache.h` protecting an editor-ID cache,
unrelated to engine threading.

In `EngineFixesSkyrim64` the only lock-adjacent comment is in
`src/patches/form_caching.h`:

> `// the game does not lock the form table on these clears so we won't either`
> `// maybe fix later if it causes issues`

That is the entire extent of synchronization-related engineering across
both projects.

### What this means concretely

- **Neither aers nor powerof3 — the two most active SE 1.5.97 reverse
  engineers — has touched the engine's `BSSpinLock`-based worker locking
  system.** They either never observed this deadlock or judged it
  untouchable from a third-party plugin.
- **There is no public off-the-shelf solution we missed.** The
  `WorkerSpinLockFix` work to date is not duplicating known-good code; it
  is genuinely original engine work.
- **No public RVA tables or struct annotations exist** for the dispatcher,
  worker pool, or lock primitive. Any further reverse engineering of those
  subsystems is original work we have to do ourselves.

This is the single most actionable conclusion of the audit and it sets the
realistic ceiling for the project: we should plan the future work on the
assumption that nobody else will hand us a worker-subsystem reverse
engineering pass.

---

## 2. What both repos do contain — patches we did not need

For completeness, the patch surface of both repos is recorded here so we
can rule them out at a glance in any future audit.

### `EngineFixesSkyrim64`

`src/patches/` (~14 patches): `disable_chargen_precache`,
`disable_snow_flag`, `enable_achievements`, `form_caching`,
`ini_setting_collection`, `max_stdio`, `regular_quicksaves`, `safe_exit`,
`save_added_sound_categories`, `save_game_max_size`,
`scrolling_doesnt_switch_pov`, `sleep_wait_time`,
`tree_lod_reference_caching`, `waterflow_animation`.

`src/fixes/` (~38 fixes): mostly null-check / nullptr-crash fixes for
specific engine functions (e.g. `bethesda_net_crash`,
`bgskeywordform_load_crash`, `create_armor_node_nullptr_crash`,
`facegen_morphdatahead_nullptr_crash`, `null_process_crash`,
`shadowscenenode_nullptr_crash`, `texture_load_crash`) plus localized
gameplay correctness fixes (`archery_downward_aiming`,
`vertical_look_sensitivity`, `weapon_block_scaling`).

`src/memory/`: replacement scrapheap, scaleform allocator, render-pass
cache, havok memory system. None of these touches threading.

### `po3-Tweaks`

`src/Tweaks.h`: gameplay tweaks (`OffensiveSpellAI`, `SitToWait`,
`SilentSneakPowerAttacks`, `LoadDoorPrompt`, `VoiceModulation`, etc.).

`src/Fixes.h`: small crash fixes (`AttachLightHitEffectCrash`,
`DistantRefLoadCrash`, `MagicItemFindKeywordFunctorCrash`) and
script/gameplay correctness fixes (`ProjectileRange`, `MapMarkerPlacement`,
`RestoreJumpingBonus`, `WornRestrictionsForWeapons`).

`src/Experimental.h`: Papyrus-side experiments
(`CleanupOrphanedActiveEffects`, `ModifySuspendedStackFlushTimeout`,
`ScriptSpeedup`, `UpdateGameTimers`). Despite the suggestive names, none
of these enters the worker-thread or `BSSpinLock` subsystem; they all
operate inside the Papyrus VM.

None of the above is relevant to LockA / LockB.

---

## 3. Positive findings — what we actually take away

Despite the negative core finding, the audit produced four useful
takeaways. All four are recorded here so that future iterations can act on
them.

### 3.1 `safetyhook` library (the most actionable single result)

`EngineFixesSkyrim64` does not use raw `Trampoline::write_branch<5>` for
inline hooks. It uses the `safetyhook` library
(`https://github.com/cursey/safetyhook`):

```cpp
inline SafetyHookInline g_hk_RemoveAt{};
g_hk_RemoveAt = safetyhook::create_inline(RemoveAt.address(), FormMap_RemoveAt);
```

`safetyhook` provides:

- **arbitrary-prologue inline hooks** (not just 5-byte rel32; handles
  whatever the prologue happens to contain),
- **detection of and chaining onto existing inline patches at the target
  address** — this directly solves the problem that killed
  `WorkerSpinLockFix v0.9` and `v0.9.1`, where another SKSE plugin
  (`skyrim-freeze-fix.dll`) had already patched
  `BSSpinLock::Acquire`'s prologue and our raw trampoline overwrote it,
- **mid-function hooks** (`safetyhook::create_mid`) for inserting
  observers inside a function body, which may be useful for picking up
  inlined `Release` sequences in callers,
- **IAT hooks** for redirecting imports.

This is the single highest-leverage change we can make to the
`WorkerSpinLockFix` toolchain. Any future entry-point hook on
`BSSpinLock::Acquire` (`id 12210`) should be implemented via
`safetyhook::create_inline`, not raw trampoline writes.

### 3.2 The "bypass, do not fight the lock" philosophy

`EngineFixesSkyrim64`'s only threading-adjacent patch is
`src/patches/form_caching.h`. Instead of patching Bethesda's lock-protected
form lookup, aers replaced the underlying data structure with a lock-free
one (`tbb::concurrent_hash_map`) and routed all callers through that:

```cpp
using HashMap = tbb::concurrent_hash_map<std::uint32_t, RE::TESForm*>;
inline HashMap g_formCache[256];

inline RE::TESForm* TESForm_GetFormByNumericId(RE::FormID a_formId)
{
    // lookup form in our cache first
    HashMap::const_accessor a;
    if (g_formCache[masterId].find(a, baseId)) { return a->second; }
    // fall back to bethesda's locked map
    formPointer = RE::TESForm::LookupByID(a_formId);
    if (formPointer != nullptr) { g_formCache[masterId].emplace(baseId, formPointer); }
    return formPointer;
}
```

Inline comment: "the game does not lock the form table on these clears so
we won't either."

The general posture is: **do not try to fix Bethesda's lock; remove the
need to take it**. For our project this opens an architectural option that
v0.1 - v0.15 never considered — instead of preventing or breaking the AB-BA
cycle on the existing locks, identify what data LockA and LockB protect
and replace those data structures (or those access paths) with lock-free
equivalents so the locks are simply not taken on the hot paths involved in
the cycle.

This is plausible but expensive: it requires reverse engineering what
LockA and LockB actually protect, which neither aers nor powerof3 has
documented. It is recorded here as a forward option for a future
architectural pass.

### 3.3 Hooking idioms worth adopting

Both repos use clean CommonLib idioms we can mirror:

- `RELOCATION_ID(SE_id, AE_id)` and `VAR_NUM(SE_off, AE_off)` macros to
  carry both SE 1.5.97 and AE RVAs simultaneously. Useful if the project
  ever needs to support AE; not urgent today.
- `REL::Relocation::write_call<5>(...)` for call-site hooks. We already
  use this.
- `REL::Relocation::write_vfunc(index, thunk)` for vtable hooks
  (`po3-Tweaks` `OffensiveSpellAI::Install`). Not relevant to LockA/LockB
  but recorded here for completeness.

None of these idioms changes our current trajectory; they are a small
quality-of-life improvement.

### 3.4 Library notes (lower priority)

- `EngineFixesSkyrim64` depends on `tbb` (Intel Threading Building Blocks)
  for `concurrent_hash_map`. Available via vcpkg; we do not need it yet,
  but worth knowing if we ever pursue the lock-bypass strategy in 3.2.
- `po3-Tweaks` depends on `xbyak` (runtime x86/x64 assembler) for
  generating patch fragments at runtime. Not a substitute for
  `safetyhook` — it solves a different problem (code generation, not
  prologue collision). Worth knowing if we ever need to synthesize
  per-callsite trampolines for the inlined-`Release` problem; not needed
  today.

---

## 4. What is still missing — what neither repo gives us

The audit explicitly does **not** provide any of the following, and these
remain prerequisites for any prevention-grade fix:

- **Annotated `BSSpinLock` struct layout.** We continue to rely on our own
  observed layout (`state` at +0, `owner` at +8). The CommonLib variant
  used by both projects (`powerof3/CommonLibSSE`) may have a struct
  definition; this is the same upstream as `CommonLibSSE-NG`, which we
  already use. Auditing CommonLib-NG directly is the better next step
  than expecting these mods to provide it.
- **Worker / dispatcher / task-pool RVAs.** We continue to rely on our own
  static analysis (`addrlib`, capstone) for `id 19369`, `id 40706`,
  `id 40285`, `id 40333`, `id 40334`, `id 40335`, `id 36438`, `id 12210`,
  etc.
- **Engine semantic annotations** (e.g. "this lock protects X struct",
  "this function can be called from worker threads", "the canonical lock
  order is A-before-B"). Neither repo has touched the relevant subsystem,
  so no comments exist.
- **A pattern scanner for inlined `BSSpinLock::Release`.** Neither repo
  has needed one. This remains an open engineering problem we will have
  to solve from scratch if we ever pursue full Acquire+Release
  observability.
- **A precedent for safe intervention on the lock primitive.** No
  third-party plugin has done this in public source, so we cannot lean
  on a known-safe pattern.

---

## 5. How this changes the WorkerSpinLockFix roadmap

The audit confirms the roadmap shape suggested in
`11-worker-spinlockfix-retrospective.md` rather than overturning it. The
practical changes are:

### Immediate (next iteration of the plugin)

- **Adopt `safetyhook`** as the inline-hook backend everywhere we currently
  use `trampoline.write_branch<5>` or any equivalent entry-point detour.
  Specifically, this enables a robust entry-point hook on
  `BSSpinLock::Acquire` (`id 12210`) that coexists with
  `skyrim-freeze-fix.dll` and any other prologue-patcher, regardless of
  load order. Without `safetyhook` the v0.9 / v0.9.1 failure mode is
  guaranteed to recur the moment we go back to entry-point hooking;
  with it, that failure mode is closed.
- **Audit `CommonLibSSE-NG`'s `BSSpinLock` definition** against our
  observed layout and adopt the canonical layout if it matches. This is
  the equivalent of the audit step neither external repo provides.

### Medium term (architectural option held in reserve)

- **Lock-bypass strategy à la `form_caching`.** If and when we identify
  what data LockA and LockB protect, we can consider replacing those
  data structures with lock-free equivalents (`tbb::concurrent_hash_map`
  or similar) so the locks are not taken on the contended paths at all.
  This requires multi-day reverse engineering work and is recorded here
  as an option, not a commitment.

### Out of scope from this audit

- **Inlined-`Release` pattern scanner.** Neither repo helps. Remains an
  open original-engineering problem.
- **Engine semantic understanding.** Neither repo helps. Remains an open
  original-engineering problem.

---

## 6. Status snapshot

- `WorkerSpinLockFix v0.15` (runtime wait-graph breaker) remains the
  current shipping strategy, with the rationale unchanged from
  `11-worker-spinlockfix-retrospective.md`.
- The single concrete tooling improvement extracted from this audit is
  `safetyhook`. It does not require a strategy change; it strengthens any
  hooking we do in future iterations and removes the v0.9 / v0.9.1
  failure class permanently.
- The audit closes the question "have we missed a public solution?" with
  a definitive no. Future planning should proceed on the assumption that
  any prevention-grade fix is original work.

---

## Cross-references

- See `11-worker-spinlockfix-retrospective.md` for the full v0.1 - v0.15
  history that this audit was meant to inform.
- See `06-root-cause.md` for the AB-BA deadlock evidence that motivates
  the project.
- See `10-future-approaches.md` (superseded by document 11 but retained
  for context) for earlier forward-looking notes from before this audit.
