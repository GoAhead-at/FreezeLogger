# 23. WorkerSpinLockFix v2.0.0 - Release Note

**Date:** 2026-05-22
**Status:** Released. `dist-out/WorkerSpinLockFix_v2.0.0.rar` is the
shipping artefact. Built from `skyrim-freeze-fix/` against
CommonLibSSE-NG with VS 18 (2026), x64 Release, static triplet.
**Predecessor docs:**
- [`14-final-design-v1.md`](14-final-design-v1.md) - the v1.0
  architecture that v2.0 layers on top of (still installed as
  defence-in-depth).
- [`15-v2-structural-strategy.md`](15-v2-structural-strategy.md) -
  the strategy doc that drove the v2.0 work, with as-built
  appendix.
- [`16`](16-v2-phase1-singletons.md) /
  [`17`](17-v2-phase1-5-findings.md) /
  [`18`](18-v2-phase2-cycle-paths.md) /
  [`19`](19-v2-phase3-mutations-and-form-types.md) /
  [`20`](20-v2-phase3-5-findings.md) /
  [`21`](21-v2-phase4-prep-dispatch-decode.md) /
  [`22`](22-v2-phase4-1-cycle-hub-characterisation.md) - the
  Phase 1 -> 4.1 reverse-engineering chain that produced the
  C5 design.

This document records what shipped, how it differs from v1.0.0,
how to verify it works in a real install, and what work remains
open.

---

## 1. What v2.0.0 changes

### 1.1 New module: `Phase4Defer`

The structural fix described in doc 22. Three
`safetyhook::create_inline` hooks plus a per-thread depth counter
and deferred-call queue:

| Hook | Address Library id | Role |
|---|---|---|
| `HookedLockAAcquirer` | `id 19369` | Wraps the LockA acquirer. Increments `tl_lockA_depth` on entry, calls original, decrements on return, drains the queue when depth returns to 0. |
| `HookedAddToTempChangeList` | `id 40333` | Entry-gate on the LockB acquirer `ProcessLists::AddToTempChangeList`. If `tl_lockA_depth > 0`, queue `(pl, actor)`; otherwise tail-call original. |
| `HookedRemoveFromTempChangeList` | `id 40334` | Entry-gate on the LockB acquirer `ProcessLists::RemoveFromTempChangeList`. Same shape as `id 40333`'s gate. |

`tl_deferred` is `thread_local std::vector<DeferredCall>` reserved
to 8 entries up-front so the hot path never allocates. The drain
swaps the queue out before iterating, so a re-entered LockA frame
that itself queues calls fills a fresh queue rather than
re-iterating the in-progress one.

The LB->LA direction (`id 40285` -> `id 36614` -> `id 38413` ->
`id 19369`) is intentionally not hooked. With LA->LB broken
structurally, the cycle cannot close in either direction.

**File layout:**

- `skyrim-freeze-fix/src/Phase4Defer.h` - public API
  (`Phase4Defer::Install()`).
- `skyrim-freeze-fix/src/Phase4Defer.cpp` - hook bodies, install
  routine, drain logic. ~290 lines.

### 1.2 Configuration changes

`WorkerSpinLockFix.toml` gains a new section:

```toml
[phase4_defer]
enabled = true
```

`Config.h` / `Config.cpp` parse the boolean into
`Config::phase4_defer_enabled`. Default `true`. When `false`,
`Phase4Defer::Install()` is not called and the plugin reverts to
v1.0.0 runtime-breaker-only behaviour without rebuilding.

The shipped TOML's `[phase4_defer]` section includes a
~25-line block comment cross-referencing doc 22.

### 1.3 Statistics

`Stats.h` / `Stats.cpp` gain three atomic counters:

| Counter | Increment site |
|---|---|
| `phase4_queued` | `HookedAddToTempChangeList` / `HookedRemoveFromTempChangeList` queues a call (LockA was held). |
| `phase4_drained` | `DrainDeferredOnExit` replays a queued call (called once per replay). |
| `phase4_passthrough` | `HookedAddToTempChangeList` / `HookedRemoveFromTempChangeList` tail-called the original (LockA was not held). |

These are appended to the periodic stats line. The intended healthy
signature is `phase4_queued > 0` (cycles are being preempted) AND
`breaks_done = 0` (the runtime breaker is finding nothing to do).

### 1.4 Hook installation order

`Hooks.cpp` installs in this order (logged on success):

1. `WaitGraph::Init()` (v1.0)
2. `Breaker::Init()` (v1.0)
3. `AcquireHook::Install()` (v1.0, gated by `acquire_hook.enabled`)
4. **`Phase4Defer::Install()` (v2.0, gated by `phase4_defer.enabled`)**
5. `Reaper::Install()` (v1.0, gated by `reaper.enabled`)

`Phase4Defer::Install()` is fail-soft: a failed install is logged
at `critical` and leaves the v1.0 `AcquireHook` + `Breaker`
pipeline active. The plugin still protects the user even if
the structural fix could not be armed.

The startup banner now includes `phase4_active=true|false` so the
state of the structural fix is visible at-a-glance in the log.

### 1.5 Build

`skyrim-freeze-fix/CMakeLists.txt`:

- Project version bumped from `1.0.0` to `2.0.0`.
- Description updated to reflect the v2.0 structural fix.
- `Phase4Defer.cpp` added to `SOURCES`, `Phase4Defer.h` to
  `HEADERS`.

The shipping archive is produced by
`skyrim-freeze-fix/packaging/make_release.ps1`, which writes
`dist-out/WorkerSpinLockFix_v<version>.rar` with the standard
MO2 layout:

```
SKSE/Plugins/WorkerSpinLockFix.dll
SKSE/Plugins/WorkerSpinLockFix.pdb
SKSE/Plugins/WorkerSpinLockFix.toml
```

---

## 2. What is unchanged from v1.0.0

- `BSSpinLock::Acquire (id 12210)` surgical inline hook
  (`AcquireHook`) - still installed and still acts as the
  defence-in-depth runtime breaker.
- `WaitGraph` - same 64-slot lock-free wait-for graph.
- `Breaker` - same time-based confirmation flow with default
  `confirmation_window_ms = 2`.
- `Reaper` - still off by default. No changes.
- `TestMode` - still validates the runtime-breaker path. A
  Phase4Defer-specific synthetic test is on the open-work list
  in §5.
- All v1.0 slow-path invariants (no heap alloc, no
  `std::mutex` on the contended hot path, no
  `safetyhook::call<>`, no `spdlog` from inside a contended
  acquire). `Phase4Defer`'s hot path is a single TLS read plus a
  branch and obeys all of them.

---

## 3. Configuration matrix

The two layers can be independently toggled without rebuilding.

| `phase4_defer.enabled` | `acquire_hook.enabled` | Result |
|---|---|---|
| `true` (default) | `true` (default) | Both layers active. Structural fix preempts cycles; runtime breaker is the safety net. **Intended configuration.** |
| `true` | `false` | Structural fix only. AB-BA cycles are preempted, but if a cycle path the structural fix doesn't cover ever fires there is no fallback. Useful for testing whether the structural fix is sufficient on its own. |
| `false` | `true` | v1.0 behaviour. Useful for A/B-testing v2.0 against v1.0 without rebuilding. |
| `false` | `false` | Plugin loads but installs no AB-BA mitigation. Use only for triage. |

A `plugin.enabled = false` overrides everything else: nothing
gets installed.

---

## 4. Verifying v2.0.0 in-place

### 4.1 First-launch banner check

Confirm the log shows the structural fix armed:

```
WorkerSpinLockFix armed. Surgical mode (acquire_hook_active=true,
break_enabled=true, confirmation_window_ms=2, phase4_active=true,
...).
```

`phase4_active=true` is the load-bearing flag. If it is `false`,
inspect the log immediately above the banner for the
`[Phase4Defer]` `critical` line that explains why install failed.

### 4.2 Steady-state telemetry

After a few minutes of gameplay the periodic stats line should look
like:

```
stats: acq_slow=NNN cycles_observed=0 cycles_confirmed=0
       breaks_done=0 breaks_raced=0 breaks_suppressed=0
       phase4_queued=0 phase4_drained=0 phase4_passthrough=NNN
     | reaper: ...
```

`phase4_passthrough` rising = the gate hooks are firing on
non-cycle paths and tail-calling the original. This proves the
hooks are installed and working.

`phase4_queued = 0, breaks_done = 0` is the expected long-run
state on a fully-loaded session: the structural fix is not yet
firing because the AB-BA path is rare. Trigger a freeze-prone
scenario (heavy combat, mass NPC scripts, save-load thrash) to
exercise it.

### 4.3 Cycle-preempted signature

When the structural fix actually preempts a cycle:

```
phase4_queued=N>0 phase4_drained=N>0 phase4_passthrough=NNN
breaks_done=0
```

`phase4_drained` should match `phase4_queued` over long horizons
(every queued call is eventually replayed by the same thread).

If `breaks_done` ever exceeds 0, that means a cycle path the
structural fix did not cover formed and was caught by the runtime
breaker. The game still kept running, but the case warrants
investigation - it's an opportunity to widen the structural fix.

---

## 5. Open work

These items did not block the v2.0.0 release because the runtime
breaker remains as the safety net even if the structural fix
misses something. They are tracked for follow-up.

| Item | Status | Notes |
|---|---|---|
| Synthetic harness for `Phase4Defer` | Pending | Extend `TestMode.cpp` to fake a call into `id 19369` that dispatches into a path reaching `id 40334`. Expectation: `phase4_queued` increments while inside the wrap; `phase4_drained` increments after the wrap returns. (todo `p4-2-impl-testharness`) |
| In-game smoke test on freeze-prone scenarios | Pending | Run combat / archery / casting / Papyrus-heavy scenarios from doc 19 §3 with v2.0.0 installed for several hours. Confirm no freezes, no behavioural regressions, `phase4_queued > 0`, `breaks_done = 0`. (todo `p4-2-impl-smoketest`) |
| Upstream PR to `EngineFixesSkyrim64` | Not attempted | The structural fix is structurally compatible with EFS's `safetyhook` adoption. Submission would require coordination with aers about ID stability across SE versions. |
| C5 widening if a missed cycle path is observed | Reactive | If `breaks_done > 0` is ever observed in a real install with `phase4_active=true`, decode the cycle path from the `[CYCLE]` log block, identify the missed acquirer, add it to `Phase4Defer`'s gate set. |

---

## 6. Why v2.0 and not just v1.x?

The version bump reflects the change in primary mechanism:

- v1.0 was reactive: detect the cycle, then break it.
- v2.0 is preventive: stop the cycle from forming in the first
  place. The reactive path is retained but is now an audit
  trail rather than a load-bearing fix.

A user with v1.0.0 installed and a v2.0.0 install side-by-side will
see the same observable behaviour (no freezes), but v2.0.0 should
show `breaks_done = 0` indefinitely while v1.0.0 will show
`breaks_done` increment whenever the AB-BA actually fired. The
mechanism by which the freeze is avoided differs entirely.
