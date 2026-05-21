# WorkerSpinLockFix

SKSE plugin for **Skyrim SE 1.5.97** that breaks a documented AB-BA
spinlock inversion in the engine's worker dispatcher at runtime. When
a real cycle forms between the two `BSSpinLock` globals involved, the
plugin force-releases one of them so the spinning thread can proceed
and the game keeps running instead of freezing hard.

This is the companion fix plugin for `FreezeLogger`. The bug it
addresses is documented in
[`../docs/case-study/06-root-cause.md`](../docs/case-study/06-root-cause.md);
the design that produces the v1.0.0 plugin is documented in
[`../docs/case-study/14-final-design-v1.md`](../docs/case-study/14-final-design-v1.md).

## What it does

Skyrim SE 1.5.97 contains a vanilla AB-BA inversion between two static
`BSSpinLock` globals in the worker dispatcher:

- `LockA` at `SkyrimSE+0x2eff8e0`
- `LockB` at `SkyrimSE+0x2f3b8e8`

Two worker threads can hold one lock and spin on the other; once both
threads are caught in the cycle, neither makes progress, the
worker-ack event the main thread is waiting on is never signalled, and
the game freezes. The race is rare per-session but cumulative on long
play and was reproduced in nine independent freeze captures with
`FreezeLogger`.

The plugin installs a single inline hook on `BSSpinLock::Acquire`
(`id 12210`) via `safetyhook::create_inline`. The detour applies a
**surgical filter**: only acquisitions of LockA or LockB do real work;
every other `BSSpinLock` pays one pointer compare and a tail-call
through the lock-free trampoline (~2 ns at 3 GHz).

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

A separate stale-owner reaper acts as an optional backstop for cases
the entry-point hook cannot observe (threads that died holding a
lock, indirect dispatches the hook never sees). It is disabled by
default; the surgical AcquireHook + breaker pipeline is sufficient
for the documented engine bug.

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
WorkerSpinLockFix armed. Surgical mode (acquire_hook_active=true, ...).
```

That is the only line that has to be present; everything else is
optional telemetry.

## Configuration

`WorkerSpinLockFix.toml` lives next to the DLL. Key settings:

| Section          | Key                         | Default   | Meaning |
|------------------|-----------------------------|-----------|---------|
| `[plugin]`       | `enabled`                   | `true`    | Master kill-switch. `false` loads the plugin idle (no hooks). |
| `[log]`          | `stats_interval_s`          | `60`      | Periodic counter-dump interval. `0` disables. |
| `[acquire_hook]` | `enabled`                   | `true`    | Emergency kill-switch for the entry-point hook. |
| `[breaker]`      | `break_enabled`             | `true`    | If `false`, the breaker logs cycles but never force-releases anything (detect-only). |
| `[breaker]`      | `confirmation_window_ms`    | `2`       | How long a cycle must remain present before being broken. |
| `[breaker]`      | `log_cycle_events`          | `true`    | Log every cycle observation, confirmation, and break attempt. |
| `[reaper]`       | `enabled`                   | `false`   | Stale-owner backstop. Off by default. |
| `[reaper]`       | `interval_ms`               | `30000`   | Stale-owner scan interval (ms). |
| `[test_mode]`    | `enabled`                   | `false`   | Synthetic AB-BA validation harness. See below. |

All keys are documented inline in the shipped TOML.

## Telemetry

Once per `stats_interval_s` the plugin emits a single info-level line:

```
stats: acq_slow=N cycles_observed=N cycles_confirmed=N
       breaks_done=N breaks_raced=N breaks_suppressed=N
     | reaper: scans=N threads=N spinners=N candidates=N
       edges=N stale_reaped=N races=N
```

What to expect during normal play:

- `acq_slow` rises steadily (a few hundred per minute under load) –
  this is normal LockA/LockB contention from the engine's worker
  pool. None of it is a deadlock.
- `cycles_observed` stays at `0` unless the AB-BA race actually fires.
- `breaks_done` stays at `0` unless the AB-BA race fires AND persists
  past the confirmation window (i.e. it's a real deadlock, not a
  near-miss).

If `breaks_done` increments and the game keeps running, the plugin
just rescued you from a freeze that would otherwise have ended the
session.

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

- `acquire_hook.enabled = false` runs the plugin reaper-only.
- `breaker.break_enabled = false` runs in detect-only mode (logs
  cycles, never force-releases).

## License

Same as the parent FreezeLogger project (currently unspecified;
deferred until publication).
