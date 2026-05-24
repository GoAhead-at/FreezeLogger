# WorkerSpinLockFix

SKSE plugin for **Skyrim SE 1.5.97** that fixes a documented AB-BA
spinlock inversion in the engine's worker dispatcher.

**v2.0.1 (internal)** ships a structural fix layered on top of the
v1.0.0 runtime breaker. One inline hook (a wrap on the LockA
acquirer at `id 19369`) plus two surgical call-site patches inside
the cycle hub (`id 36016+0xdcb` and `id 19372+0x606`) defer the
LockB-protected portion of `id 40333` / `id 40334` whenever the
current thread is inside the LockA acquirer, so the AB-BA cycle
simply cannot form. The wrap on `id 19369` matches the engine
function's real signature: 6 args (`rcx`, `rdx`, `r8b`, `r9`, plus
a dword stack arg 5 at `[rsp+0x28]` and a byte stack arg 6 at
`[rsp+0x30]`) and a `bool` return, all forwarded verbatim through
`unsafe_call<bool>`. The per-actor `kInTempChangeList` bit toggle
runs synchronously in the gate as defensive scaffolding -- it
eliminates any stale-flag window for readers that might observe
bit 9 inside the LockA scope without adding any lock contention.
The v1.0.0 runtime breaker (surgical hook on `BSSpinLock::Acquire`,
per-thread wait-for graph, time-based confirmation flow,
force-release via `InterlockedCompareExchange`) is retained as
defence-in-depth: if the structural fix misses any cycle path the
runtime breaker still catches and force-releases.

> v2.0.0 silently broke scripted-animation activators (skyshards
> being the most visible case). Three diagnostic cuts of v2.0.1
> were needed to find the actual cause: the wrap declared 4 args
> for an engine function that takes 6, so the trampoline read
> garbage for arg 5 and arg 6 every invocation. The earlier
> hypotheses (stale-bit window, dropped `bool` return) turned
> out to be incorrect; the bit-toggle and bool-return fixes
> from those passes are kept as defensive scaffolding. The
> 6-arg wrap signature is the load-bearing fix. See
> [`../docs/case-study/24-v2-0-1-skyshard-regression-fix.md`](../docs/case-study/24-v2-0-1-skyshard-regression-fix.md).
>
> A subsequent v2.0.1 refactor (also internal) replaces the two
> entry-gates on `id 40333` / `id 40334` with surgical call-site
> patches at `id 36016+0xdcb` and `id 19372+0x606`. Same gate
> semantics, narrower blast radius, and other mods that hook
> `id 40333` / `id 40334` directly continue to work. See
> [`../docs/case-study/25-v2-0-1-callsite-refactor.md`](../docs/case-study/25-v2-0-1-callsite-refactor.md).

## Scope

This plugin addresses **one** documented engine bug: the AB-BA
inversion between two specific `BSSpinLock` globals (LockA at
`SkyrimSE+0x2eff8e0`, LockB at `SkyrimSE+0x2f3b8e8`) inside the
worker dispatcher. It does **not** address:

- Cell-loading freezes caused by `BSReadWriteLock` deadlocks. If
  your freezes correlate with crossing cell boundaries, install
  GarrixWong's [`skyrim-freeze-fix`](https://github.com/garrixwong/skyrim-freeze-fix)
  or a successor in addition to this plugin. The two mods are
  complementary -- they target different lock primitives at
  different call sites and can run together without conflict.
- HDT-SMP / havok physics-pipeline waits where the main thread is
  blocked on a producer-consumer event. Freezes of that shape
  show no `BSSpinLock` activity in this plugin's telemetry; they
  belong to the physics mod chain.
- Generic engine slowdowns, stuck splash screens, or
  startup-script issues unrelated to the worker-pool spinlock
  cycle.

The plugin's diagnostic logging and `cycles_observed` /
`breaks_done` counters can confirm whether a given freeze is in
this plugin's scope. If `cycles_observed` is `0` while you are
frozen, the freeze is *not* the AB-BA `BSSpinLock` cycle and a
different mod is the right place to look.

This is the companion fix plugin for `FreezeLogger`. The bug it
addresses is documented in
[`../docs/case-study/06-root-cause.md`](../docs/case-study/06-root-cause.md);
the v1.0 architecture is documented in
[`../docs/case-study/14-final-design-v1.md`](../docs/case-study/14-final-design-v1.md);
the v2.0 cycle-hub characterisation and the structural fix design
are in
[`../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md`](../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md).

## What it does

Skyrim SE 1.5.97 contains a vanilla AB-BA inversion between two static
`BSSpinLock` globals in the worker dispatcher:

- `LockA` at `SkyrimSE+0x2eff8e0`, taken inside the LockA acquirer
  (`id 19369`).
- `LockB` at `SkyrimSE+0x2f3b8e8`, taken inside three non-virtual
  ProcessLists methods: `id 40285`
  (`TransferBetweenTempChangeLists`-style traverser), `id 40333`
  (`AddToTempChangeList`), `id 40334`
  (`RemoveFromTempChangeList`).

Two worker threads can hold one lock and spin on the other; once both
threads are caught in the cycle, neither makes progress, the
worker-ack event the main thread is waiting on is never signalled, and
the game freezes. The race is rare per-session but cumulative on long
play and was reproduced in nine independent freeze captures with
`FreezeLogger`.

The plugin layers two complementary mechanisms.

### Layer 1 - Structural fix (v2.0, primary)

One inline hook on the LockA acquirer plus two surgical
call-site patches inside the cycle hub:

1. **`id 19369` (LockA acquirer) wrap via `safetyhook::create_inline`** -
   increments a thread-local "LockA depth" counter on entry, runs
   the original, decrements on return, and drains the deferred-
   call queue when the counter returns to 0.
2. **`id 36016+0xdcb` (call to `id 40334`) call-site patch via
   `Trampoline::write_call<5>`** - if the current thread's LockA
   depth is `> 0`, synchronously clear `kInTempChangeList`, push
   `(pl, actor)` onto the thread-local deferred queue, return
   early. Otherwise tail-call the original `id 40334`.
3. **`id 19372+0x606` (inner call to `id 40333`) call-site patch
   via `Trampoline::write_call<5>`** - same shape: if LockA is
   held, set `kInTempChangeList` synchronously and queue the
   bucket-array append; otherwise tail-call the original
   `id 40333`.

The drain at LockA-depth-0 happens on the same thread that originally
queued the call, so per-thread call ordering is preserved. LockB is
acquired normally during the drain because LockA is no longer held.
The AB-BA cycle simply cannot form.

The two call sites we patch are the **only** paths in the binary
through which `id 40333` / `id 40334` are reached while LockA is
held (verified during Phase 4.1 cycle-hub characterisation). The
function entries of `id 40333` and `id 40334` are left pristine,
so any other mod that hooks those functions cooperates with this
plugin instead of competing for the prologue. Pre-patch
verification reads the 5-byte CALL at each site and refuses to
patch if another mod has already redirected it; the plugin
downgrades cleanly to v1.0 runtime-breaker mode in that case.

The LB->LA direction (`id 40285` -> `id 36614` -> `id 38413` ->
`id 19369`) is intentionally left alone: once the LA->LB edge is
broken, the cycle cannot close.

### Layer 2 - Runtime breaker (v1.0, defence-in-depth)

A single inline hook on `BSSpinLock::Acquire` (`id 12210`) via
`safetyhook::create_inline`. The detour applies a **surgical filter**:
only acquisitions of LockA or LockB do real work; every other
`BSSpinLock` pays one pointer compare and a tail-call through the
lock-free trampoline (~2 ns at 3 GHz).

When the detour fires for LockA or LockB and the lock is contended,
the plugin:

1. Marks the current thread as waiting on the target lock in a fixed-
   size, lock-free wait-graph.
2. Walks the wait-for chain. If it closes back to the current thread
   the cycle is reported to the breaker.
3. The first observer of a given cycle signature claims the
   confirmation flow: it sleeps `confirmation_window_ms` (default 2 ms),
   then re-verifies the cycle is still topologically present.
4. If the cycle has self-resolved, the breaker stands down.
5. Otherwise the lock's `state` field is force-released via
   `InterlockedCompareExchange` and the spinning thread acquires it on
   its next spin iteration. The thread that thought it owned the
   released lock continues to run its critical section without
   protection until it next releases; in practice the engine's
   invariants on this section tolerate that.

With the structural fix active and healthy the runtime breaker should
**never fire** in normal play (`breaks_done` stays at 0). If it does
fire, that signals a cycle path the structural fix missed and the
runtime breaker still catches it.

A separate stale-owner reaper acts as an optional backstop for cases
neither the entry-point hook nor the structural fix can observe
(threads that died holding a lock, indirect dispatches no hook sees).
It is disabled by default; the structural fix + AcquireHook breaker
pipeline is sufficient for the documented engine bug.

The plugin ships with a **synthetic AB-BA test harness** that can be
enabled in the TOML to validate the breaker end-to-end on
heap-allocated test BSSpinLocks without touching the engine. See
[Testing the breaker](#testing-the-breaker).

## Installation

The plugin is shipped as a single Mod Organizer 2-ready archive:

```
SKSE/Plugins/WorkerSpinLockFix.dll
SKSE/Plugins/WorkerSpinLockFix.pdb
SKSE/Plugins/WorkerSpinLockFix.toml
```

Drop it into MO2 as a new mod, enable the mod, launch via SKSE.

On first launch confirm the log file at

```
Documents/My Games/Skyrim Special Edition/SKSE/WorkerSpinLockFix/WorkerSpinLockFix.log
```

contains a banner ending in:

```
WorkerSpinLockFix armed. Surgical mode (acquire_hook_active=true,
break_enabled=true, confirmation_window_ms=2, phase4_active=true,
...).
```

`phase4_active=true` confirms the v2.0 structural fix is armed.
`acquire_hook_active=true` confirms the v1.0 runtime breaker is
armed underneath. Either one is sufficient to keep the engine
running through the AB-BA race; both running together is the
intended configuration.

That banner is the only line that has to be present; everything else
is optional telemetry.

## Configuration

`WorkerSpinLockFix.toml` lives next to the DLL. Key settings:

| Section            | Key                         | Default   | Meaning |
|--------------------|-----------------------------|-----------|---------|
| `[plugin]`         | `enabled`                   | `true`    | Master kill-switch. `false` loads the plugin idle (no hooks). |
| `[log]`            | `stats_interval_s`          | `60`      | Periodic counter-dump interval. `0` disables. |
| `[phase4_defer]`   | `enabled`                   | `true`    | v2.0 structural fix. If `false`, the LockA/LockB hooks are not installed and only the v1.0 runtime breaker runs. |
| `[acquire_hook]`   | `enabled`                   | `true`    | v1.0 entry-point hook. Emergency kill-switch. |
| `[breaker]`        | `break_enabled`             | `true`    | If `false`, the breaker logs cycles but never force-releases anything (detect-only). |
| `[breaker]`        | `confirmation_window_ms`    | `2`       | How long a cycle must remain present before being broken. |
| `[breaker]`        | `log_cycle_events`          | `true`    | Log every cycle observation, confirmation, and break attempt. |
| `[reaper]`         | `enabled`                   | `false`   | Stale-owner backstop. Off by default. |
| `[reaper]`         | `interval_ms`               | `30000`   | Stale-owner scan interval (ms). |
| `[test_mode]`      | `enabled`                   | `false`   | Synthetic AB-BA validation harness. See below. |

All keys are documented inline in the shipped TOML.

The intended steady-state configuration is `[phase4_defer] enabled =
true` AND `[acquire_hook] enabled = true`. The structural fix
preempts cycles, and the runtime breaker is the safety net.

## Telemetry

Once per `stats_interval_s` the plugin emits a single info-level line:

```
stats: acq_slow=N cycles_observed=N cycles_confirmed=N
       breaks_done=N breaks_raced=N breaks_suppressed=N
       phase4_queued=N phase4_drained=N phase4_passthrough=N
     | reaper: scans=N threads=N spinners=N candidates=N
       edges=N stale_reaped=N races=N
```

What to expect during normal play:

- `acq_slow` rises steadily (a few hundred per minute under load) -
  this is normal LockA/LockB contention from the engine's worker
  pool. None of it is a deadlock.
- `phase4_passthrough` rises whenever execution reaches one of the
  two patched cycle-hub call sites (`id 36016+0xdcb`,
  `id 19372+0x606`) without LockA being held by the calling
  thread. Since the v2.0.1 call-site refactor this number is
  small: only callers that traverse the cycle hub itself fire
  the gate at all. (Before the refactor, when the gates were
  function-wrap inline hooks on the entries of `id 40333` /
  `id 40334`, every engine call into those functions paid the
  gate cost; the call-site patch design narrows that to exactly
  the cycle path.)
- `phase4_queued` rises whenever the structural fix detects a
  thread holding LockA and queues a LockB-acquirer call for later.
  Each `phase4_queued` event represents an AB-BA cycle that was
  preempted before it could form. `phase4_drained` should match
  `phase4_queued` over long horizons (every queued call is
  eventually replayed).
- `cycles_observed` stays at `0` unless a cycle path the structural
  fix missed actually fires.
- `breaks_done` stays at `0` unless such a cycle fires AND persists
  past the confirmation window (i.e. it's a real deadlock, not a
  near-miss).

The healthy signature is `phase4_queued > 0, breaks_done = 0`: the
structural fix is preempting cycles and the runtime breaker is
finding nothing to do. If `breaks_done > 0`, that indicates the
structural fix missed a cycle path and the runtime breaker caught
it - the plugin still rescued you from a freeze, but the case is
worth reporting so the structural fix can be widened.

When `log_cycle_events = true`, every cycle the breaker actually
operates on emits a structured `[CYCLE]` / `[BREAK]` block to the
log, including the participating thread IDs, lock addresses, and
observed owners. These are auditable after the fact.

## Testing the breaker

The TOML has an optional `[test_mode]` section:

```toml
[test_mode]
enabled = false
```

Set `enabled = true`, restart the game once. Right after data files
finish loading the plugin spawns two threads that deliberately
construct an AB-BA cycle on two heap-allocated test `BSSpinLock`
objects, routed through the real `BSSpinLock::Acquire (id 12210)` so
they go through the surgical hook. The breaker is expected to detect
the cycle, confirm it after the configured window, force-release one
test lock, and let both threads complete.

Expected log fragment on success:

```
[TEST] starting synthetic AB-BA validation. test_lockA=0x..., test_lockB=0x..., timeout=10000ms.
[TEST] worker A (TID ...) acquiring test_lockA (...).
[TEST] worker B (TID ...) acquiring test_lockB (...).
[TEST] worker A (TID ...) acquiring test_lockB (this is the AB half; will spin until breaker fires).
[TEST] worker B (TID ...) acquiring test_lockA (this is the BA half; will spin until breaker fires).
[CYCLE] first observation (length=2, ...):
[CYCLE]   TID ... waits on lock 0x... (owner TID ...)
[CYCLE]   TID ... waits on lock 0x... (owner TID ...)
[CYCLE] confirmed (will break) (length=2, age=2ms, observations=...):
[BREAK] force-released BSSpinLock 0x... (observed owner TID ..., state 1->0). Detector TID ... should now acquire on next spin.
[TEST] worker B (TID ...) completed; both test locks released.
[TEST] worker A (TID ...) completed; both test locks released.
[TEST] SUCCESS - both workers completed. The breaker detected the synthetic AB-BA, confirmed it via the time-based flow, and force-released one test lock. End-to-end cycle break is proven.
```

The harness has a 10-second safety net: if the breaker fails to fire,
both test locks are manually cleared so the worker threads drain
instead of spinning forever. The verdict line is `[TEST] FAILURE` in
that case.

The test does **not** touch the engine's BSSpinLocks. It allocates its
own pair of `BSSpinLock` objects and registers them with
`AcquireHook::AddTestLocks`. The breaker writes only into those two
pointers; engine state is never modified.

After running the test once, set `[test_mode] enabled = false` again
for normal play (the harness runs once per launch and is harmless
otherwise, just spammy in the log).

## Building

Prerequisites:

- Visual Studio 2026 with the C++23 toolchain.
- vcpkg, with `VCPKG_ROOT` set in the environment.
- Address Library for SKSE (only required at runtime, not build time).

From a PowerShell shell at the project root:

```powershell
$env:VCPKG_ROOT = "d:/Programme/Microsoft Visual Studio/18/Community/VC/vcpkg"
& "d:/Programme/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --preset windows-x64-release
& "d:/Programme/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build C:/sk/wslf/r --config Release
```

Output: `C:/sk/wslf/r/Release/WorkerSpinLockFix.dll` (and `.pdb`).
The build directory is set in `CMakePresets.json` to keep MSVC PDB
paths under `MAX_PATH`.

To package an MO2-ready RAR archive use the bundled script:

```powershell
.\packaging\make_release.ps1
```

The output archive is written to `dist-out\WorkerSpinLockFix_v<X.Y.Z>.rar`.

## Safety notes

- **Hard runtime pin.** The plugin refuses to install hooks on any
  build other than 1.5.97. The Address Library ID for
  `BSSpinLock::Acquire` (`12210`) and the LockA/LockB RVAs only match
  this version.
- **Surgical filter.** The slow path runs only for the two engine
  BSSpinLocks the plugin watches. Every other `BSSpinLock::Acquire`
  pays one pointer compare and a tail-call.
- **No heap allocation on the slow path.** Allocating from inside a
  `BSSpinLock::Acquire` detour puts the heap `CRITICAL_SECTION` on
  the BSSpinLock lock-order graph and creates a new deadlock class.
  All cycle-tracking buffers are stack-resident or fixed-size static
  storage.
- **No `std::mutex` / SRWLock on the slow path's contended hot
  loop.** The breaker takes a `std::mutex` only briefly when a cycle
  is observed (a rare event); it is never taken on uncontended
  acquires.
- **`safetyhook::unsafe_call` on the trampoline.** The library's
  `call<>` variant takes a `std::recursive_mutex` for thread-safe
  install/uninstall. With ~300 engine threads each routing every
  `BSSpinLock::Acquire` through the hook, that mutex would serialise
  every acquire across all threads. `unsafe_call` skips the mutex and
  tail-calls through the trampoline directly. The hook is never
  uninstalled at runtime so there is no install/destruction race.
- **Force-release semantics.** The CAS only flips `state` from `1` to
  `0`. The `owner` field is left intact (the engine does not always
  clear it on release either, so this matches normal release
  behaviour). The thread that thought it owned the released lock
  continues to run; the engine's invariants on the released section
  tolerate brief unguarded execution because the same race happens
  transiently in vanilla without crashes.

## Recovery / disable

If the plugin causes any problem, set `plugin.enabled = false` in
`WorkerSpinLockFix.toml` and restart the game. The plugin will load
idle and the engine runs unmodified. As individual escape hatches:

- `phase4_defer.enabled = false` disables the v2.0 structural fix.
  The v1.0 runtime breaker still detects and breaks cycles.
- `acquire_hook.enabled = false` disables the v1.0 entry-point hook.
  The v2.0 structural fix continues to preempt cycles.
- `breaker.break_enabled = false` runs the runtime breaker in
  detect-only mode (logs cycles, never force-releases).
- All three off plus the reaper off makes the plugin completely
  inert.

## License

Same as the parent FreezeLogger project (currently unspecified;
deferred until publication).
