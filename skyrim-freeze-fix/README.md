# WorkerSpinLockFix

SKSE plugin for **Skyrim SE 1.5.97 and AE 1.6.x** that fixes **five
distinct engine hard-freeze classes**:

1. **The AB-BA spinlock inversion** in the engine's worker dispatcher
   — prevented structurally by **`Phase4Defer`** so the deadlock cycle
   can never form.
2. **The `WaitForJobTask` lost-wakeup hang** — where `Main::Update`
   parks on a job-completion event that a producer tore down without
   signalling — recovered by **`JobWaitBreaker`** (new in v2.3.0).
3. **The Site-A worker-ack deadlock** — where `Main::Update` parks in
   the `id 34554` helper on a worker-ack event that a worker consumed
   the wake for but never signalled — recovered by **`SiteABreaker`**
   (new in v2.4.0).
4. **The render-side Site-A worker-ack deadlock** — where the render /
   worker thread parks in the per-task cloth-join `id 34557` waiting on a
   dispatched sub-task that never completes, so it never signals the
   worker-ack — recovered by **`SiteARenderBreaker`** (new in v2.5.0).
5. **The leaked `BSSpinLock` freeze** — where a thread (main, via the
   HDT-SMP cloth chain → `BSSpinLock::Acquire` `id 12210`) spins forever
   on a heap lock whose owner is an idle worker parked in the pool
   (`id 68058`) that acquired the lock and went idle without releasing
   it — recovered by **`LeakedSpinLockBreaker`** (new in v2.6.0), which
   proves the lock is steady-state leaked and force-releases it.

A single DLL installs on both SE 1.5.97 and AE 1.6.x (VR is refused).
See [Multi-version support](#multi-version-support) for the SE↔AE map.

**v2.6.0** is the current release — it adds the `LeakedSpinLockBreaker`
layer (Layer 5), a watchdog that force-releases a provably-leaked
`BSSpinLock` held by a parked idle worker (case-study 30; proven by
FreezeLogger v0.11.1 in `freeze_2026-06-25_222116`), on top of
**v2.5.0**. Field validation shows active mode (`detect_only = false`)
replaces a **hard-freeze** with a **stutter loop** (stuck → released →
stuck) while the leak producer remains — enough to move the camera and
narrow a content trigger, but not a root-cause cure. See
[`docs/case-study/30-leaked-spinlock-breaker.md`](../docs/case-study/30-leaked-spinlock-breaker.md).
**v2.5.0** adds the `SiteARenderBreaker` layer
(Layer 4), the render-thread counterpart of `SiteABreaker`, on top of
**v2.4.1** (a documentation/metadata maintenance build) and **v2.4.0**, which
added the `SiteABreaker` layer (Layer 3). v2.3.0 added `JobWaitBreaker` and **removed** the v1.0
runtime breaker (`AcquireHook` + `WaitGraph` + `Breaker`), its
stale-owner reaper, and the synthetic test harness: field telemetry
showed `Phase4Defer` prevents the AB-BA cycle outright
(`cycles_observed=0` across long sessions), so the runtime breaker
never fired and was redundant. The plugin is now five focused,
independent fixes that share no state.

### Version history

| Version | Status | Summary |
|---|---|---|
| v2.6.0 | **Current.** | Adds **`LeakedSpinLockBreaker`** (Layer 5) — recovery for the **leaked `BSSpinLock`** freeze (case-study 30; proven by FreezeLogger v0.11.1's parked-holder steady-state probe in `freeze_2026-06-25_222116`). A thread (main, via the HDT-SMP cloth chain `id 35565` → `BSSpinLock::Acquire` `id 12210`) spins forever on a heap lock whose `[+0]` owner is an idle worker parked in a kernel wait inside the pool (`id 68058`, on the pool Semaphore) that acquired the lock and went idle **without releasing it** — a lock-ownership deadlock no `SetEvent` (Layers 2–4) can break. A watchdog (no inline hook) scans suspended threads for one contending a lock at a version-independent spin-retry signature, follows the owner, and — only once the owner is provably parked and the lock's `(owner,state)` word plus the holder's RIP stay byte-for-byte unchanged across the dwell + recheck window — force-releases the lock via an `InterlockedCompareExchange64` that no-ops if anything moved. Ships **detect-only** by default. **Field validation:** active mode breaks the hard-freeze but recurs as a stutter loop while the leak producer stays active; disabling a correlated actor ref (`000198DC` / Dervenin) stopped recurrence. |
| v1.0.0 | Released 2026-05-21, removed | Runtime breaker only: surgical hook on `BSSpinLock::Acquire`, lock-free wait-for graph, time-based confirmation, force-release via `InterlockedCompareExchange`. Retired in v2.3.0. |
| v2.0.0–v2.0.3 | 2026-05-22 → 24, superseded | Added the structural fix (`Phase4Defer`) as the primary layer; fixed a 6-arg skyshard regression; rebased the LockB gates onto two surgical `Trampoline::write_call<5>` call-site patches; redesigned the reaper around `WaitGraph::SnapshotEdges`. See case-studies 22–26. |
| v2.1.0 | Released 2026-06-03, superseded | Moved the runtime detector onto a `safetyhook` mid-hook at `id 12210 +0x8a` (the backoff retry point) so uncontended/recursive acquires ran native with zero added cost — removing hot-path overhead that stacked under framerate-amplifying mods (e.g. PureDark's upscaler). |
| v2.2.0 | Released, superseded | **Anniversary Edition (1.6.x) support.** All engine targets re-derived structurally against the unpacked AE 1.6.1170 binary (byte signatures do not survive AE's recompile), wired in as `REL::RelocationID{se, ae}` pairs with runtime-selected lock RVAs, call-site offsets, and the `+0xe0 → +0xe8` Actor-flags shift. See [Multi-version support](#multi-version-support). |
| v2.3.0 | Superseded | Adds **`JobWaitBreaker`** — recovery for the Skyrim `WaitForJobTask` lost-wakeup hang (case-study 27/28), a freeze class unrelated to the spinlock bug. Wraps `WaitForJobTask` (located by a version-independent `.text` body signature via `SkyrimAnchors`); a watchdog detects the lost-wakeup signature and, in active mode, delivers the missing signal via `SetEvent`. Ships **detect-only** by default. **Removes** the v1.0 runtime breaker, the reaper, and the test harness (`Phase4Defer` made them redundant). |
| v2.5.0 | Superseded | Adds **`SiteARenderBreaker`** (Layer 4) — recovery for the **render-side** Site-A worker-ack deadlock (case-study 29 §6; captured in `freeze_2026-06-24_053440`). The render/worker thread runs the parallel cloth dispatch loop (`id 34567`) and parks inside the per-task join (`id 34557`) waiting on a dispatched HDT-SMP sub-task that never completes, so it never signals Singleton-A's worker-ack `[+0x60]` and any consumer (transitively, main) hangs. Wraps `id 34557` (located by a byte-unique `.text` entry signature via `SkyrimAnchors`), capturing the Singleton-A from `rcx` and the ack handle at entry; the same dwell + zero-progress watchdog as Layer 3 confirms the signature and, in active mode, `SetEvent`s the ack. Ships **detect-only** by default. |
| v2.4.1 | Superseded | Documentation/metadata maintenance release — **no behavior change**; the shipped DLL is functionally identical to v2.4.0. Corrects the `job_wait_breaker` active-mode TOML comment (the removed process-wide `WaitForSingleObjectEx` wrap — the wait handle is derived from the job-pool chain at wait entry), clarifies the `Phase4Defer` partial-install fallback comment (a failed install degrades to safe pass-through, never a half-applied deferral), and syncs the `vcpkg.json` manifest version. |
| v2.4.0 | Superseded | Adds **`SiteABreaker`** — recovery for the Skyrim Site-A worker-ack deadlock (case-study 29): `Main::Update` parks in the `id 34554` helper on Singleton-A's manual-reset worker-ack event after a worker consumed the wake but never signalled completion (driven by Faster HDT-SMP's per-frame dispatch via `id 35565`; observed on HDT-SMP 3.1.0.0 *and* 3.2.0.0, so version-independent). Wraps `id 34554` (located by an independent `.text` body signature via `SkyrimAnchors`) and captures the worker-ack handle at entry; a watchdog confirms the deadlock signature and, in active mode, delivers the missing signal via `SetEvent`. Ships **detect-only** by default. |

## Scope

This plugin addresses **three** documented engine freeze classes:

1. The **AB-BA inversion** between two specific `BSSpinLock` globals
   (LockA at `SkyrimSE+0x2eff8e0`, LockB at `SkyrimSE+0x2f3b8e8`)
   inside the worker dispatcher — prevented by `Phase4Defer`.
2. The **`WaitForJobTask` lost-wakeup hang** where `Main::Update`
   parks forever on a job-completion event that a producer cleared
   without signalling — recovered by `JobWaitBreaker`. This is the
   freeze class previously seen with HDT-SMP on main's stack (the
   FSMP frame is just the `Main::Update` hook trampoline, not the
   cause). See case-study 27/28.
3. The **Site-A worker-ack deadlock** where `Main::Update` parks
   forever in the `id 34554` helper on Singleton-A's manual-reset
   worker-ack event after a worker consumed the wake but never
   signalled completion (driven by Faster HDT-SMP's per-frame
   parallel dispatch via `id 35565`; observed on HDT-SMP 3.1.0.0 and
   3.2.0.0, so version-independent) — recovered by `SiteABreaker`.
   See case-study 29.

It does **not** address:

- Cell-loading freezes caused by `BSReadWriteLock` deadlocks. If
  your freezes correlate with crossing cell boundaries, install
  GarrixWong's [`skyrim-freeze-fix`](https://github.com/garrixwong/skyrim-freeze-fix)
  or a successor in addition to this plugin. The two mods are
  complementary -- they target different lock primitives.
- Generic engine slowdowns, stuck splash screens, or
  startup-script issues unrelated to the two bugs above.

This is the companion fix plugin for `FreezeLogger`. The AB-BA bug
is documented in
[`../docs/case-study/06-root-cause.md`](../docs/case-study/06-root-cause.md);
the structural-fix design is in
[`../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md`](../docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md);
the `WaitForJobTask` hang and the `JobWaitBreaker` design are in
[`../docs/case-study/27-hdtsmp-deadlock-report.md`](../docs/case-study/27-hdtsmp-deadlock-report.md)
and
[`../docs/case-study/28-jobwaitbreaker-design.md`](../docs/case-study/28-jobwaitbreaker-design.md).

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

The plugin runs three independent layers, each targeting a different
freeze class.

### Layer 1 - AB-BA spinlock prevention (`Phase4Defer`)

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
patch if another mod has already redirected it; that arm is left
unhooked in that case rather than risk a double-patch.

The LB->LA direction (`id 40285` -> `id 36614` -> `id 38413` ->
`id 19369`) is intentionally left alone: once the LA->LB edge is
broken, the cycle cannot close.

### Layer 2 - WaitForJobTask lost-wakeup recovery (`JobWaitBreaker`, v2.3.0)

A **separate** engine freeze, unrelated to the spinlock bug:
`Main::Update` periodically blocks in `WaitForJobTask` on a manual-reset
job-completion event until the in-flight jobs drain. Under heavy
parallelism a publish-then-tear-down race can lose the wakeup — main
reads the job chain as non-null, commits to
`WaitForSingleObject(handle, INFINITE)`, then **after** main is asleep
another thread clears the job sub-array **without** signalling main's
event. No thread is left to signal it, so main sleeps forever and the
game freezes. (FreezeLogger captured this exact signature; see
case-study 28.)

`JobWaitBreaker` has three pieces:

1. **`WaitForJobTask` inline wrap.** Resolved version-independently by
   `SkyrimAnchors` (a 15-byte `.text` body-signature scan that also
   derives the Singleton-B job-pool slot from the prologue). On the main
   thread it records "in a job-wait since `T`, on job index `N`".
2. **Watchdog thread.** When main has been parked longer than
   `dwell_threshold_ms` and the job slot it is waiting on has been torn
   down to null (the job is provably gone) — and the state survives a
   `recheck_window_ms` re-check — that is the lost-wakeup signature. A
   job still in flight is left alone.
3. **Release (active mode only).** Delivers the missing signal via
   `SetEvent` on the captured wait handle so main resumes. Because the
   job is already gone, this is equivalent to the signal that was lost
   (worst case a one-frame glitch vs a permanent freeze).

It **ships detect-only by default** (`[job_wait_breaker] detect_only =
true`): it logs the lost-wakeup signature but changes nothing, so the
trigger can be field-validated against real captures before the
`SetEvent` path is enabled. The module never suspends an engine thread
or reads a thread context.

### Layer 3 - Site-A worker-ack deadlock recovery (`SiteABreaker`, v2.4.0)

A **third** engine freeze, distinct from both above. `Main::Update` has
a second infinite-wait site — the `id 34554` helper, reached via
`id 35565` and driven by Faster HDT-SMP's per-frame parallel-cloth
dispatch. It publishes work into Singleton-A (`pending=1`, wakes a
worker via the auto-reset worker-wake event), then blocks `INFINITE` on
the manual-reset worker-ack event `[singleton+0x60]` until the worker
signals completion. When a worker consumes the wake, takes the job, and
never signals the ack, main sleeps forever. Two field captures show the
exact state on **different HDT-SMP versions** (3.2.0.0 and 3.1.0.0), so
it is independent of the HDT-SMP version — downgrading HDT-SMP does not
fix it. (See case-study 29.)

`SiteABreaker` has three pieces, mirroring `JobWaitBreaker`:

1. **`id 34554` inline wrap.** Resolved version-independently by
   `SkyrimAnchors` (an independent `.text` body-signature scan that also
   derives the Singleton-A slot from the `mov rbx,[rip]` load). On the
   main thread it opens an episode only when a wait is actually scheduled
   (`pending=1`) and captures the worker-ack handle at entry.
2. **Watchdog thread.** When main has been parked in the same Site-A
   episode longer than `dwell_threshold_ms` and is still parked after a
   `recheck_window_ms` re-check with zero progress (`pending` still 1,
   work-id + ack handle unchanged, ack event still unsignalled), that is
   the deadlock signature. A wait that is going to complete clears
   `pending` inside the window and is left alone.
3. **Release (active mode only).** Delivers the missing signal via
   `SetEvent` on the captured worker-ack handle so main resumes. The
   worker is provably not progressing, so this is equivalent to the ack
   that was lost (worst case a one-frame cosmetic cloth glitch vs a
   permanent freeze).

It **ships detect-only by default** (`[site_a_breaker] detect_only =
true`). `id 34554` is main-only (the render thread uses a different
Singleton-A pair, `id 34557/34567`), so the wrap never runs off the main
thread. Like Layer 2 it never suspends an engine thread or reads a
thread context. **Note:** the Site-A anchors are located by signature
scan with no AE validation yet, so on AE the module simply does not arm
(fail-safe) until the signature is confirmed there.

## Multi-version support

As of v2.2.0 one DLL runs on **SE 1.5.97 and AE 1.6.x**. VR is
explicitly refused — its targets have not been re-derived against
the VR binary, so the plugin stays idle there rather than risk
patching wrong addresses.

AE's executable was recompiled with a different MSVC, so byte
signatures do **not** transfer. Every target was instead pinned
*structurally* against the unpacked AE 1.6.1170 binary and verified
by disassembly:

| Role | SE | AE | How the AE counterpart was pinned |
|---|---|---|---|
| `BSSpinLock::Acquire` | `id 12210` | `id 13663` | Identical `+0x8a` spin-retry offset and `self`-in-RDI / tid-in-EBP register contract. |
| LockA acquirer (wrapped) | `id 19369` | `id 19796` | Unique recursive function holding a static spinlock; identical 6-arg `bool` prologue. AE *inlines* the LockA acquire. |
| `AddToTempChangeList` | `id 40333` | `id 41343` | Unique "static spinlock guarding a bit-9 actor-flag toggle" fingerprint (`or [actor+0xe8],0x200`), `+0x158` bucket. |
| `RemoveFromTempChangeList` | `id 40334` | `id 41344` | Twin of the above (`and [actor+0xe8],~0x200`), `+0x158`/`+0x168` bucket ops. |
| Cycle hub | `id 36016` | `id 36991` | Call graph: the ~10 KB dispatch function that calls Remove directly and the add-wrapper. `call->Remove` at `+0xdcb -> +0xf6e`. |
| Add-wrapper | `id 19372` | `id 19799` | The cycle hub's add arm; `call->Add` at `+0x606 -> +0x634`. |
| LockA global | `0x2eff8e0` | `0x31376d8` | The static spinlock held by the recursive acquirer. |
| LockB global | `0x2f3b8e8` | `0x319c2a8` | The static spinlock guarding the temp-change-list twins. |
| Actor flags offset | `+0xe0` | `+0xe8` | Actor struct grew 8 bytes between editions. |

All ids resolve through `REL::RelocationID{se, ae}`; all RVAs,
call-site offsets, and struct offsets are runtime-selected via
`REL::Module::IsAE()`. The two surgical call-site patches are still
guarded by `Phase4Defer::VerifyCallSite()`, which checks the patch
target is the expected `E8 rel32` call and **aborts cleanly** if an
AE offset is wrong — so a bad offset can never corrupt the binary,
it only leaves that arm unhooked.

The analysis tooling that produced this map lives under
`../analysis/` (`ae_find_lockb.py`, `ae_recursive_lockers.py`,
`ae_callers.py`, `ae_calltargets.py`, `ae_find_lacka.py`,
`ae_dump.py`).

> **First AE sessions:** there is no captured AB-BA *deadlock* on AE
> yet — the structural fix is preventative and will sit dormant if
> the cycle never forms. Running the first AE sessions with
> `[phase4_defer] diagnostic_logging = true` is recommended so the
> gate firings on the LA->LB path can be confirmed.

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
WorkerSpinLockFix armed. phase4_active=true (AB-BA spinlock
prevention), job_wait_breaker_active=true (WaitForJobTask
lost-wakeup recovery, detect_only=true).
```

`phase4_active=true` confirms the AB-BA structural fix is armed.
`job_wait_breaker_active=true` confirms the WaitForJobTask recovery
layer is armed (`detect_only=true` means it logs but does not yet
release — the shipped default).

That banner is the only line that has to be present; everything else
is optional telemetry.

## Configuration

`WorkerSpinLockFix.toml` lives next to the DLL. Key settings:

| Section              | Key                    | Default   | Meaning |
|----------------------|------------------------|-----------|---------|
| `[plugin]`           | `enabled`              | `true`    | Master kill-switch. `false` loads the plugin idle (no hooks). |
| `[log]`              | `stats_interval_s`     | `60`      | Periodic counter-dump interval. `0` disables. |
| `[phase4_defer]`     | `enabled`              | `true`    | AB-BA structural fix. If `false`, the LockA/LockB hooks are not installed. |
| `[phase4_defer]`     | `diagnostic_logging`   | `false`   | Per-call diagnostic logging for the structural fix. Enable when triaging a regression; recommended ON for first AE sessions. |
| `[job_wait_breaker]` | `enabled`              | `true`    | WaitForJobTask lost-wakeup recovery (`SkyrimAnchors` + watchdog). |
| `[job_wait_breaker]` | `detect_only`          | `true`    | If `true` (default), logs the lost-wakeup signature but changes nothing. Set `false` to actively release main via `SetEvent`. |
| `[job_wait_breaker]` | `dwell_threshold_ms`   | `3000`    | How long main must be parked in `WaitForJobTask` before it is treated as stuck. |
| `[job_wait_breaker]` | `poll_interval_ms`     | `1000`    | Watchdog poll cadence. |
| `[job_wait_breaker]` | `recheck_window_ms`    | `250`     | Re-check delay before acting, so a legitimate late completion is never raced. |
| `[job_wait_breaker]` | `diagnostic_logging`   | `false`   | Verbose per-decision watchdog logging. |

All keys are documented inline in the shipped TOML.

## Telemetry

Once per `stats_interval_s` the plugin emits a single info-level line:

```
stats: phase4: queued=N drained=N passthrough=N | job_wait: stuck=N released=N
```

What to expect during normal play:

- `phase4_passthrough` rises whenever execution reaches one of the
  two patched cycle-hub call sites (`id 36016+0xdcb`,
  `id 19372+0x606`) without LockA being held by the calling thread.
- `phase4_queued` rises whenever the structural fix detects a thread
  holding LockA and queues a LockB-acquirer call for later. Each
  event represents an AB-BA cycle that was preempted before it could
  form. `phase4_drained` should match `phase4_queued` over long
  horizons (every queued call is eventually replayed).
- `job_wait_stuck` stays at `0` unless the WaitForJobTask lost-wakeup
  signature is confirmed (main parked past the dwell threshold on a
  torn-down job).
- `job_wait_released` stays at `0` in detect-only mode. In active
  mode it increments each time the watchdog delivered the missing
  signal to release main.

The healthy steady-state signature is `phase4_queued > 0` (cycles
being preempted) and `job_wait_stuck = 0` (no lost-wakeup freezes).
If `job_wait_stuck > 0`, the plugin saw the WaitForJobTask freeze
class — in detect-only mode that is the cue to enable
`[job_wait_breaker] detect_only = false` so the next occurrence is
recovered rather than fatal.

When `[job_wait_breaker] diagnostic_logging = true`, each watchdog
decision (parked-but-job-in-flight, lost-wakeup detected, released)
emits a structured line to the log for after-the-fact audit.

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

- **Runtime gate.** The plugin installs on SE 1.5.97 and AE 1.6.x
  only; every other SE build and VR are refused (the plugin loads
  idle, leaving the engine unmodified). Cross-version targets are
  resolved per runtime — `REL::RelocationID{se, ae}` for addrlib
  ids, `REL::Module::IsAE()` for RVAs / call-site offsets / struct
  offsets — so the wrong-version addresses can never be used. See
  [Multi-version support](#multi-version-support).
- **Phase4Defer: pristine function entries.** Only the two cycle-hub
  call sites are patched (via `Trampoline::write_call<5>`); the entries
  of `id 40333` / `id 40334` are left untouched, so other mods that
  hook those functions cooperate with this plugin. `VerifyCallSite()`
  refuses to patch (and downgrades cleanly) if another mod already
  redirected a call site.
- **JobWaitBreaker: no thread suspension, no context reads.** The
  watchdog never calls `SuspendThread` / `GetThreadContext` / `Toolhelp32`
  — a deliberate contrast with the retired reaper. It only reads the
  job-pool chain under `__try/__except` and (in active mode) calls
  `SetEvent`.
- **JobWaitBreaker: tight break gate.** The release only fires after
  the dwell threshold AND the job slot is provably torn down to null
  AND the state survives a re-check, so a healthy frame is never
  touched. `SetEvent` on a manual-reset event is idempotent. The
  default is detect-only, which alters nothing.
- **Anchors are version-independent.** `WaitForJobTask` and the
  Singleton-B slot are located by a `.text` body signature, not a
  hard-coded RVA or Address Library id (which are not identity-stable
  for these targets). If the signature is not found the module stays
  idle rather than dereferencing a stale address.

## Recovery / disable

If the plugin causes any problem, set `plugin.enabled = false` in
`WorkerSpinLockFix.toml` and restart the game. The plugin will load
idle and the engine runs unmodified. As individual escape hatches:

- `phase4_defer.enabled = false` disables the AB-BA structural fix.
- `job_wait_breaker.enabled = false` disables the WaitForJobTask
  lost-wakeup layer entirely.
- `job_wait_breaker.detect_only = true` (the default) keeps the
  WaitForJobTask layer observing/logging only, without ever calling
  `SetEvent`.
- Both layers off makes the plugin completely inert.

## License

Same as the parent FreezeLogger project (currently unspecified;
deferred until publication).
