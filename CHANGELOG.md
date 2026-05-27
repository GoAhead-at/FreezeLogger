# Changelog

All notable changes to **FreezeLogger** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Diagnostic-only output format changes are not treated as a breaking SemVer event
unless they remove or rename an existing section — fields may grow without notice.

## [0.2.0] — 2026-05-28

This release is the first iteration informed by a real captured freeze corpus
(see `docs/case-study/27-hdtsmp-deadlock-report.md`). The headline change is a
new **Freeze classification** block at the top of every report so the human
reader sees a verdict before scrolling through hundreds of KB of thread dumps.

### Added
- **`Snapshot::Verdict`** module (`src/snapshot/Verdict.{h,cpp}`) emitting a
  10–15 line freeze-classification block immediately after the report header.
  Recognised classes:
  - `BSSpinLock AB-BA (WorkerSpinLockFix domain)` — Site A + a spinner whose
    observed lock owner is the main thread.
  - `HDT-SMP / Site-B Papyrus event-source wait` — Site B + an `hdtsmp64.dll`
    frame on main's stack.
  - `Site-B Papyrus event-source wait (no HDT-SMP fingerprint)` — same bug
    class, different proximate caller.
  - `Site-A worker-ack wait` — Site A without a BSSpinLock cycle.
  - `Unrecognised` — main is in a kernel wait we don't have a fingerprint for.
  Each verdict carries a `Confidence:` (`high`/`medium`/`low`) and a
  `Suggested triage:` link pointing at the most relevant `docs/case-study/*.md`.
- **`Verdict::Classify`** as a pure, unit-testable function — given a
  synthesised `Observations` POD, returns the expected class + confidence.
- **HDT-SMP stack-presence detector** (cheap probe inside `Verdict`): scans a
  2 KiB window above main's RSP for a saved return address inside any loaded
  `hdtsmp64.dll`-named module's range. Reports the first match's RVA.
- **HDT-SMP worker-pool aggregation**: counts threads whose top frame RIP is
  inside `hdtsmp64.dll` and, when uniform, prints the modal RVA
  (`N idle (all parked on hdtsmp64.dll+0xXXXXXX)`).
- **Writer-still-live double-sample probe**: at Site B, the Singleton-B chain
  is walked twice with a ~50 ms gap. The verdict block annotates the result
  as `(writer: settled)` vs `(writer: still mutating)`.
- **Singleton-B hex-dump ASCII annotation** in `MainWaitProbe::DumpMemoryWindow`.
  Engine event-source-holder keys (`Weekday`, `Water`, `Ranged`, `Cast Magic
  Event`, `Crime Gold Event`, …) live inline in the singleton instance and
  were previously visible only as raw qwords like `0x007961646b656557`. They
  now render as `0x007961646b656557 "Weekday"` directly in the report.
- **`Loaded modules` FileVersion enrichment**: every row now includes
  `(FileVersion A.B.C.D)` (where the binary has a `VS_FIXEDFILEINFO`
  resource), making it trivial to tie a freeze to the exact build of
  `hdtsmp64.dll`, `SkyrimSE.exe`, etc.

### Changed
- Plugin version: `0.1.0 → 0.2.0`. SKSE plugin metadata (generated via
  `add_commonlibsse_plugin`) and the `FL_VERSION_*` defines propagate
  automatically from `CMakeLists.txt`.
- Spec status: `Draft v0.2 → Draft v0.3 (post first-real-freeze corpus)`.
- Report section order: **Verdict → System → Threads → Modules → Papyrus →
  AnimGraph → Engine (includes MainWaitProbe) → WaitGraph → Recent activity →
  Mini-dump**. Previously Verdict did not exist and the report opened with
  `System`.
- `docs/spec.md` re-numbered §6 to insert *Freeze classification* as item 2,
  and elevated the long-form `MainWaitProbe` and `WaitGraph` to first-class
  sections (items 9 and 10).

### Build
- New runtime dependency: `Version.lib` (already on the Windows SDK; no
  additional install required) for `GetFileVersionInfo*` / `VerQueryValueW`.
- `src/snapshot/Verdict.{cpp,h}` added to `SOURCES` / `HEADERS` in
  `CMakeLists.txt`.

### Notes for the corpus replay
- Freezes captured under v0.1.x can be re-read against the new spec, but the
  Verdict block will be absent. A clean replay requires recapturing under
  v0.2.0.
- The freeze-class fingerprinting is version-agnostic for HDT-SMP — it
  detects `hdtsmp64.dll` by module-base name, not by hard-coded RVAs — so
  the verdict works across all `Faster HDT-SMP-UP` builds the case-study
  corpus has seen so far.

---

## [0.1.0] — 2026-05-14

Initial development cut. Detects main- and render-thread stalls via a dual
heartbeat and a separate watchdog thread; on a stall, snapshots the process
and writes a human-readable report.

### Added
- `Heartbeat` module (two `std::atomic<uint64_t>` timestamps, one each for
  `Main::Update` and `IDXGISwapChain::Present`).
- `MainHook` (Address Library `REL::ID(35551)` + offset `0x11F`, written via
  `stl::write_thunk_call`) and `RenderHook` (DXGI swap-chain vtable slot 8,
  installed after `BSGraphics::Renderer::Init_InitD3D` at
  `REL::ID(75595)` + offset `0x50`).
- `Watchdog` thread (`std::jthread`) — wakes every `check_interval_ms`,
  detects stalls, classifies (`main` / `render` / `both`), orchestrates the
  snapshot, and annotates the latest report on resolve.
- `Symbols` — DbgHelp initialised with the Microsoft public symbol server +
  local cache; mutex-guarded resolver.
- Snapshot sections:
  - `System` (OS, memory, CPU, GPU).
  - `Threads` (per-thread stack walk with symbol resolution, 120-frame cap,
    `[main game thread]` / `[render thread]` labels).
  - `Modules` (loaded DLLs, base/size/path, `[skse]` flag for files under
    `Data\SKSE\Plugins`).
  - `Papyrus` (VM stats, top stacks, queue depth).
  - `AnimGraph` (player-only, lite).
  - `Engine` (cell, worldspace, player position, calendar, pause flags;
    long-form `MainWaitProbe` appended here).
  - `MainWaitProbe` (Site-A id 34554 lock primitive walk, Site-B
    `+0xc38130` event-source wrapper walk, BSSpinLock-owner cycle scan).
  - `WaitGraph` (cross-thread handle wait table, signaller-candidate
    search).
  - `RingBuffer` (last 100 Papyrus lines + 50 SKSE messaging events with
    `GetTickCount64()` stamps).
  - `MiniDump` (default off, gated by config, retain last 5).
- `Reporter` writes a UTF-8 text report at
  `Documents/My Games/Skyrim Special Edition/SKSE/FreezeLogger/freeze_<ts>_<thread>.log`
  and rewrites `freeze_latest.log` on every snapshot.
- `Config` (TOML, `Data/SKSE/Plugins/FreezeLogger.toml`).
- `DebugTriggers` (compiled in when `-DFL_DEBUG_TRIGGERS=ON`): `VK_PAUSE`
  hotkey, `FL_TEST_FREEZE_AFTER_S` / `FL_TEST_FREEZE_DURATION_S` env vars,
  `FL_FAKE_HEARTBEAT` for testing the pipeline before hooks are pinned.
- Unit tests (Catch2 v3): `Heartbeat`, `Watchdog`, `RingBuffer`.
- Packaging: `packaging/make_release.ps1` produces a single `.rar` archive
  with the MO2-expected `SKSE/Plugins/` layout.

### Known issues
- No top-of-report freeze classification — the analyst has to read the
  long-form `MainWaitProbe` section to decide what bug class a freeze
  belongs to. Addressed in v0.2.0.
- `Loaded modules` table omits `FileVersion`. Addressed in v0.2.0.

[0.2.0]: ./docs/spec.md
[0.1.0]: ./docs/spec.md
