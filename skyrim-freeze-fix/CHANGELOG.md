# Changelog

All notable changes to **WorkerSpinLockFix** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.6.1] — 2026-08-04

### Fixed
- **`LeakedSpinLockBreaker` watchdog no longer CTDs on wild reads during
  load/teardown.** Field crash bucket `CTD-7b4ecc21` (SE 1.5.97) faulted inside
  `TryReadQword` ~1.4 s after the watchdog started, with `StateFlags: Loading`
  and a wild read address harvested from a half-built thread context. SEH alone
  could not catch it — during loader transitions / process teardown the
  exception dispatcher does not always unwind a hardware fault. Every harvested
  pointer is now validated with `VirtualQuery` (committed + readable; writable
  for the force-release) **before** any dereference, with SEH kept only as a
  TOCTOU backstop. The watchdog thread is still created at install but stays
  fully idle (no thread suspension, no probing) until the SKSE `kDataLoaded`
  message arms it, so it never scans during the load/menu window where there is
  also no gameplay freeze to break. Detection, dwell/recheck gates, and
  force-release behaviour are unchanged.

## [2.6.0] — 2026-06-26

### Added
- **`LeakedSpinLockBreaker` (Layer 5)** — recovery for the leaked-`BSSpinLock`
  freeze proven by FreezeLogger v0.11.1 (`freeze_2026-06-25_222116`, case-study
  30). A thread (typically main, via the HDT-SMP cloth chain `id 35565` →
  `BSSpinLock::Acquire` `id 12210`) spins forever on a heap lock whose owner is
  an idle worker parked in the pool (`id 68058`) that acquired the lock and went
  idle without releasing it — a lock-ownership deadlock no `SetEvent` (Layers
  2–4) can break. A watchdog (no inline hook) scans suspended threads for a
  contender at a version-independent spin-retry signature, follows the owner,
  and — only once the owner is provably parked and the lock's `(owner,state)`
  word plus the holder's RIP stay byte-for-byte unchanged across the dwell +
  recheck window — force-releases via `InterlockedCompareExchange64` (no-op if
  anything moved). Ships **detect-only** by default. Field note: active mode
  breaks the hard-freeze but can recur as a stutter loop while the leak producer
  remains; disabling a correlated actor ref (`000198DC` / Dervenin) stopped
  recurrence in one session.

## [2.5.0] — 2026-06-26

### Added
- **`SiteARenderBreaker` (Layer 4)** — recovery for the render-side Site-A
  worker-ack deadlock (case-study 29 §6; captured in `freeze_2026-06-24_053440`).
  The render/worker thread runs the parallel cloth dispatch loop (`id 34567`) and
  parks inside the per-task join (`id 34557`) waiting on a dispatched HDT-SMP
  sub-task that never completes, so it never signals Singleton-A's worker-ack
  `[+0x60]` and any consumer (transitively, main) hangs. Wraps `id 34557`
  (located by a byte-unique `.text` entry signature via `SkyrimAnchors`),
  capturing the Singleton-A from `rcx` and the ack handle at entry; the same
  dwell + zero-progress watchdog as Layer 3 confirms the signature and, in
  active mode, `SetEvent`s the ack. Ships **detect-only** by default.

## [2.4.1] — 2026-06-16

### Changed
- Documentation/metadata maintenance release — **no behavior change**; the
  shipped DLL is functionally identical to v2.4.0. Corrects the
  `job_wait_breaker` active-mode TOML comment (the removed process-wide
  `WaitForSingleObjectEx` wrap — the wait handle is derived from the job-pool
  chain at wait entry), clarifies the `Phase4Defer` partial-install fallback
  comment (a failed install degrades to safe pass-through, never a half-applied
  deferral), and syncs the `vcpkg.json` manifest version.

## [2.4.0] — 2026-06-16

### Added
- **`SiteABreaker` (Layer 3)** — recovery for the Site-A worker-ack deadlock
  (case-study 29): `Main::Update` parks in the `id 34554` helper on
  Singleton-A's manual-reset worker-ack event after a worker consumed the wake
  but never signalled completion (driven by Faster HDT-SMP's per-frame dispatch
  via `id 35565`; observed on HDT-SMP 3.1.0.0 *and* 3.2.0.0, so
  version-independent). Wraps `id 34554` (located by an independent `.text`
  body signature via `SkyrimAnchors`) and captures the worker-ack handle at
  entry; a watchdog confirms the deadlock signature and, in active mode,
  delivers the missing signal via `SetEvent`. Ships **detect-only** by default.

## [2.3.0] — 2026-06-15

### Added
- **`JobWaitBreaker` (Layer 2)** — recovery for the `WaitForJobTask` lost-wakeup
  hang (case-study 27/28), a freeze class unrelated to the spinlock bug. Wraps
  `WaitForJobTask` (located by a version-independent `.text` body signature via
  `SkyrimAnchors`); a watchdog detects the lost-wakeup signature and, in active
  mode, delivers the missing signal via `SetEvent`. Ships **detect-only** by
  default.

### Removed
- The v1.0 runtime breaker (`AcquireHook` + `WaitGraph` + `Breaker`), the
  stale-owner reaper, and the synthetic test harness. Field telemetry showed
  `Phase4Defer` prevents the AB-BA cycle outright (`cycles_observed=0` across
  long sessions), so the runtime breaker never fired and was redundant.

## [2.2.0] — 2026-06-15

### Added
- **Anniversary Edition (1.6.x) support.** All engine targets re-derived
  structurally against the unpacked AE 1.6.1170 binary (byte signatures do not
  survive AE's recompile), wired in as `REL::RelocationID{se, ae}` pairs with
  runtime-selected lock RVAs, call-site offsets, and the `+0xe0 → +0xe8`
  Actor-flags shift. One DLL installs on SE 1.5.97 and AE 1.6.x (VR refused).

## [2.1.0] — 2026-06-10

### Changed
- Moved the runtime detector onto a `safetyhook` mid-hook at
  `id 12210 +0x8a` (the backoff retry point) so uncontended/recursive acquires
  ran native with zero added cost — removing hot-path overhead that stacked under
  framerate-amplifying mods (e.g. PureDark's upscaler).

## [2.0.0] – [2.0.3] — 2026-05-22 – 2026-05-24

### Added
- **`Phase4Defer` (Layer 1)** — structural prevention of the AB-BA spinlock
  inversion: an inline wrap on the LockA acquirer (`id 19369`) plus surgical
  `Trampoline::write_call<5>` patches at the cycle-hub call sites so LockB
  acquires are deferred while the current thread holds LockA. See case-studies
  22–26.

### Fixed
- Match `id 19369`'s 6-arg `bool` signature (skyshard regression from the
  initial v2 cut).
- Rebased the LockB gates onto two surgical call-site patches
  (`id 36016+0xdcb → id 40334`, `id 19372+0x606 → id 40333`).
- Redesigned the stale-owner reaper around `WaitGraph::SnapshotEdges`.

## [1.0.0] — 2026-05-21

### Added
- Initial release: surgical hook on `BSSpinLock::Acquire`, lock-free wait-for
  graph, time-based confirmation, force-release via
  `InterlockedCompareExchange`.

### Notes
- Retired in v2.3.0 after `Phase4Defer` made the runtime breaker redundant.
