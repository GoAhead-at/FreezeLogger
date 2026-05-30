# Changelog

All notable changes to **FreezeLogger** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Diagnostic-only output format changes are not treated as a breaking SemVer event
unless they remove or rename an existing section — fields may grow without notice.

## [0.4.0] — 2026-05-29

Coverage release. The Papyrus VM and Animation-graph report sections had
been placeholders since v0.1 (they printed only the singleton pointer + a
`<not yet wired up>` line). Both are now implemented against the
CommonLibSSE-NG 1.5.97 headers, broadening FreezeLogger from "native
`WaitForJobTask` hang" diagnosis (the case-study 27 family) toward
**script-side and animation freezes** as well.

### Added
- **`[test_mode]` config section** (`capture_on_pause`, `hotkey_vk`) — a
  runtime, always-compiled developer/QA toggle for internal builds. When
  `capture_on_pause = true`, the watchdog spawns a lightweight thread that
  polls `hotkey_vk` (default `0x13` = VK_PAUSE); each fresh press writes a
  full report **on demand without stalling the game** (`Reporter::CaptureManual`).
  The report is labelled `Capture type: MANUAL` and named
  `freeze_<ts>_manual.log`. Distinct from the compile-time `FL_DEBUG_TRIGGERS`
  hotkey (debug builds only, induces a real stall); this works in release
  builds and never blocks the main thread. Default OFF; leave off for public
  releases. Report writes are now serialized by a mutex since the watchdog
  and hotkey threads can both reach the capture path.
- **Papyrus VM stats** (`Snapshot::Papyrus`). Reads the
  `BSScript::Internal::VirtualMachine` 1.5.97 layout directly, lock-free:
  - Flags: `initialized`, `overstressed`.
  - Counts: running stacks, waiting-latent returns, attached-script
    handles, live script arrays, pending function messages (+ overflow),
    suspend overflow A/B, queued unbinds. All are `size()`-style reads
    (`_capacity - _free`) — no map traversal, and **no VM spinlock is
    taken** (taking an engine lock from the watchdog at freeze time would
    be the very lock-order inversion this project exists to prevent).
  - Best-effort, lock-free running-stack walk (capped 512 visited / 16
    detailed): per-stack `stackID`, `State`, `FreezeState`, frame count,
    plus a state histogram. Racy by construction; the outer `Section`
    SEH guards any torn-map fault.
  - Reading guide in-section: a large pending-func-msg / waiting-latent
    backlog points at a script-side stall, distinct from the native hang.
- **Animation graph (player, lite)** (`Snapshot::AnimGraph`). Resolves
  `BSAnimationGraphManager` via
  `IAnimationGraphManagerHolder::GetAnimationGraphManager`, then reports
  graph count, active graph index (`RUNTIME_DATA::activeGraph`), the active
  `BShkbAnimationGraph` project name / anim-bone count / foot-IK, and the
  active `hkbBehaviorGraph` activity flags (`isActive`, `isLinked`,
  `updateActiveNodes`, `stateOrTransitionChanged`, static-node count,
  root-generator presence).

### Changed
- `docs/spec.md` v0.5 → v0.6: §6 (Papyrus VM) and §7 (Animation graph)
  rewritten from intent to implemented behavior; component table updated;
  §12 open questions "AnimGraph lite accessor" and Papyrus VM stats marked
  **RESOLVED** with the verified accessor chains.
- `CMakeLists.txt`: project version bumped to **0.4.0**.

### Notes
- The "current animation event / time-in-state" idea from the original
  v0.1 sketch was intentionally dropped from the lite scope: extracting it
  needs a Havok `hkbBehaviorGraph` state-machine walk (the footgun
  traversal deferred in spec §13). The behavior-graph activity flags answer
  "is the graph wedged?" without that risk.
- Neither section is on the critical path for the case-study 27
  `WaitForJobTask` hang; they exist to catch the *other* freeze families.

---

## [0.3.0] — 2026-05-28

Forward-looking diagnostic release. v0.2.x finally pinpointed the wait
site as Skyrim's `WaitForJobTask` (case-study 27 §0), but every
post-freeze sample we have shows the task-pool chain *already torn
down* — singleton `+0x08` zeroed, sub-array gone, handle table empty —
so there's no signal in the frozen state alone about *which* job got
stuck. v0.3 closes that gap by capturing a **healthy baseline of the
task-pool chain ≈1 Hz** while the game is running normally, then
**diffs it against the frozen state** at freeze time. The next captured
freeze should be the first one where we can name the slot that died.

No behaviour change for callers; the new section is additive in the
report and the new per-frame work on the main thread is gated behind a
1-in-60 atomic increment + modulo on non-capture frames (sub-nanosecond
in the steady state).

### Added
- `FreezeLogger::TaskPoolBaseline` — lock-protected ring-of-1 holding
  the most recently captured healthy state of Singleton-B
  (`SkyrimSE+0x2f26a70`). Written from the `Main::Update` hook on a
  1-in-60 throttle (≈1 Hz at 60 fps), read by `Snapshot::TaskPool` at
  freeze time. Captures 32 qwords of the singleton instance, 16 qwords
  of the sub-array, and per-entry: 8 qwords of the dispatch struct +
  8 qwords of its handle table. SEH-bounded; faults zero the affected
  slots, never the whole sample. Sub-microsecond mutex hold on both
  writer and reader paths.
- `Snapshot::TaskPool` — new freeze-report section rendered after
  Engine state and before WaitGraph. Compares the most recent healthy
  baseline against a fresh frozen-time capture of the same chain, with
  per-qword `<-- DIFF` markers on lines that changed. Renders four
  layers: (1) the global slot, (2) the singleton instance with v0.2
  ASCII annotation, (3) the sub-array, (4) for each populated entry
  the dispatch struct + handle table. Frozen entries are pointer-matched
  against baseline entries so reallocated slots are flagged. Closes
  with an `Investigation hint` paragraph telling the analyst how to
  cross-reference main's wait HANDLE (from the Threads section) against
  the baseline's handle tables to identify which queue index main was
  waiting on.
- `TaskPoolBaseline::Init()` call wired into `MainHook::Install` so
  baseline capture arms automatically when the Main::Update hook
  installs. Logs the resolved SkyrimSE base + capture cadence.

### Changed
- `docs/spec.md` v0.4 → v0.5:
  - Status line bumped, new "Snapshot additions (v0.3)" row added to the
    Resolved Decisions cheat-sheet.
  - `Snapshot orchestrator` data flow now reads
    `...MainWaitProbe → TaskPool → WaitGraph...`.
  - New parallel section documenting the per-frame
    `TaskPoolBaseline::MaybeCapture()` call inside the Main::Update hook.
  - New §6.10 "Task-pool snapshot" describing the four-layer baseline /
    frozen comparison, the age-of-baseline reporting, and the cost
    budget (~256 SEH-guarded qword reads per capture; sub-nanosecond
    per-frame steady-state cost).
  - WaitGraph / Recent activity / Mini-dump renumbered to 11 / 12 / 13.
  - §8 Project Layout adds `src/TaskPoolBaseline.{h,cpp}` and
    `src/snapshot/TaskPool.{h,cpp}`.
  - §11 Validation Plan: new item 8 for the task-pool snapshot
    (synthetic + real-freeze acceptance criteria).
- `CMakeLists.txt`: project version bumped to **0.3.0**;
  `TaskPoolBaseline.cpp` + `snapshot/TaskPool.cpp` added to `SOURCES`,
  `TaskPoolBaseline.h` + `snapshot/TaskPool.h` added to `HEADERS`.

### Verdict / classification
- Unchanged from 0.2.1. The new section is purely additive; classifier
  enum identifiers, labels, and confidence values are all stable.

### Migration notes
- None. Drop the new DLL in place; existing `FreezeLogger.toml` files
  continue to work unchanged. Old reports remain comparable since
  every existing section is preserved in place and order; the
  Task-pool snapshot is wedged between Engine and Wait graph.

---

## [0.2.1] — 2026-05-28

Reclassification release. The Faster HDT-SMP-UP maintainer reviewed the
v0.2.0 case-study report and identified the Site-B wait
(`SkyrimSE+0xc38130`) as Skyrim's `WaitForJobTask` — i.e. main thread
"waiting on jobs before rendering" — *not* a Papyrus event-source wait.
Frames inside `hdtsmp64.dll` on main's stack are the FSMP `Main::Update`
hook trampoline (upstream infrastructure), not the cause of the wait.
All verdict labels and supporting documentation are corrected to reflect
this. No behavioural change to the probes themselves; only the
interpretation, labels, and diagnosis text changed.

### Changed
- `Snapshot::Verdict` labels (enum identifiers unchanged for log-grep
  stability):
  - `HdtsmpSiteB` → "Skyrim WaitForJobTask hang (FSMP on main's stack is
    the upstream Main::Update hook, not the cause)" (was: "HDT-SMP /
    Site-B Papyrus event-source wait").
  - `SiteBNoHdtsmp` → "Skyrim WaitForJobTask hang (no HDT-SMP / FSMP
    frame on stack)" (was: "Site-B Papyrus event-source wait (no
    HDT-SMP fingerprint)").
- `Snapshot::Verdict::SiteString` for Site B now reads
  `B (Skyrim WaitForJobTask @ SkyrimSE+0xc38130)`.
- `MainWaitProbe` Site-B intro now identifies `+0xc38130` as
  `WaitForJobTask` ("Skyrim's job-pool wait — *are there outstanding
  tasks? if yes, block until they drain*"). Verdict block in the
  long-form audit names Skyrim's task pool as the producer and notes
  the FSMP frame is just the upstream `Main::Update` hook trampoline.
- `docs/spec.md` v0.3 → v0.4. Replaced every "Papyrus event-source"
  wording with "WaitForJobTask" / "job-pool" wording. Refreshed the
  freeze-class enumeration in §6.2 and the validation step in §11.7.
- `docs/case-study/27-hdtsmp-deadlock-report.md` gained a new top-level
  **§0 Maintainer response** section with the verbatim quote, a
  "what this corrects" mapping table, and a note that §§1–10 are
  preserved unedited for historical record but should be read through
  the §0 correction.

### Clarified
- The inline ASCII annotation on the Singleton-B hex dump
  (`"Weekday"`, `"Water"`, `"Ranged"`, `"Cast Magic Event"`, …) is
  **kept as a debug aid** but now carries a one-line caveat in the
  report: those qwords may be coincidental ASCII inside a job-id /
  hash / inline padding rather than real engine event keys. The
  v0.2.0 case-study text reading them as "engine event names" relied
  on the now-superseded event-source-holder interpretation.

### Internal
- Plugin version: `0.2.0 → 0.2.1` (`CMakeLists.txt`).
- No new files, no new dependencies, no API changes. `Class` enum
  identifiers stay the same so any downstream log grep against
  `HdtsmpSiteB` / `SiteBNoHdtsmp` keeps working — only the rendered
  label text moved.

---

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

[0.2.1]: ./docs/spec.md
[0.2.0]: ./docs/spec.md
[0.1.0]: ./docs/spec.md
