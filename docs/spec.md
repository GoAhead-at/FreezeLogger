# FreezeLogger — SKSE Plugin Specification

**Project name:** `FreezeLogger`
**Type:** SKSE64 native DLL plugin
**Language:** C++20
**Target runtime:** Skyrim Special Edition **1.5.97** (hard-pinned)
**Engine binding:** CommonLibSSE-NG (covers 1.5.97; future SE/AE/VR support is a port, not a rebuild)
**Status:** Draft v0.2 (post-design-grilling)

---

## 0. Resolved Decisions (cheat sheet)

| Branch | Decision |
|---|---|
| Audience scope | Personal-first; release on Nexus is a stretch goal |
| Target runtime | SE 1.5.97 only, runtime version verified at load |
| Engine binding | CommonLibSSE-NG via vcpkg manifest |
| Heartbeat hooks | **Dual** — `Main::Update` *and* `BSGraphics::Renderer::Present` |
| Watchdog timing | `threshold = 5000 ms`, `check = 500 ms`, `cooldown = 60 s` |
| Resolved-freeze handling | Annotate the existing report with `Resolved at T+Xs`; do not emit a duplicate |
| Mini-dump | Code in v1, **default OFF**, TOML toggleable, retain last 5 |
| Snapshot scope (v1) | §1 Header, §2 System, §3 Threads + stacks, §4 Modules, §5 Papyrus VM, §6 *lite* (player only), §7 Engine, §8 Recent-activity ring buffer |
| Deferred to v2 | §6 *full* (nearest-N actors), JSON sidecar |
| Synthetic trigger | Debug-build hotkey (`VK_PAUSE`) **+** env-var one-shot, both gated by `FL_DEBUG_TRIGGERS=ON`. Optional `FL_FAKE_HEARTBEAT=1` for testing the pipeline before hooks are pinned. |
| Symbol resolution | Microsoft public symbol server, local cache, URL embedded in every report header |
| Address Library | Hard required; plugin refuses to load on any runtime ≠ 1.5.97 |
| Source control | Local git only (no remote) |
| Test framework | Catch2 v3 (vcpkg) for unit tests; integration tests via the synthetic trigger |
| License | Deferred until / unless we publish |
| Retention | 50 text reports, 5 minidumps, unbounded symbol cache |

---

## 1. Problem Statement

Skyrim suffers from intermittent, non-reproducible **hard freezes**. The game becomes
fully unresponsive (no audio progression, frozen frame, no input) but the process
does not crash. Recovery requires force-killing the process.

Standard tooling does not catch this:

- **Crash loggers** only fire on access violations / unhandled exceptions; a
  frozen-but-alive process never trips them.
- **Papyrus scripts** can't reliably observe these freezes — when the engine
  stalls (native deadlock, render-thread hang, Havok stall, animgraph deadlock,
  infinite loop in a third-party DLL, memory corruption), the Papyrus VM stops
  scheduling, so a Papyrus watchdog freezes alongside the game it watches.
- **Windows Event Viewer / WER** only react after the user kills the hung
  process and produce a minidump with limited engine context.

We need an **in-process native-thread observer** that survives a stall of the
main thread and emits enough state to diagnose the cause post-mortem.

---

## 2. Goals

1. Detect when Skyrim's main thread *or* render thread has stopped advancing
   for a configurable amount of time (default: **5 s**), and identify which
   thread froze.
2. Capture a rich diagnostic snapshot from a separate native thread that is
   unaffected by the stall.
3. Persist the snapshot to disk in a human-readable format so the user can
   share it after force-quitting the game.
4. Be safe: never cause a crash, never deadlock, never measurably affect FPS
   in steady state.

## 3. Non-Goals (v1)

- Automatic recovery from the freeze.
- Replacing crash loggers for actual crashes.
- Modifying gameplay, save files, or any persistent game state.
- Fixing the underlying mod conflicts — diagnostic only.
- Cross-runtime support (SE/AE/VR). Architecture leaves the door open; v1 ships
  for 1.5.97 only.

---

## 4. Approach: Dual Heartbeat + Watchdog Thread

The plugin maintains **two atomic timestamps** — one updated from
`Main::Update` (game tick) and one from `IDXGISwapChain::Present`
(end-of-frame submit; we hook the DXGI swap chain directly via vtable
detour, see §5.1). A third native thread watches both:

```
   Main thread                          Render thread
   ───────────                          ─────────────
   every Main::Update:                  every Present:
     mainHeartbeat = now()                renderHeartbeat = now()
                  │                                      │
                  └────────────► both visible ◄──────────┘
                                       │
                            Watchdog thread (separate OS thread)
                            ─────────────────────────────────────
                            loop:
                              sleep(check_interval_ms)
                              age_main   = now - mainHeartbeat
                              age_render = now - renderHeartbeat
                              if max(age_main, age_render) > threshold_ms
                                 and not in cooldown:
                                  classify which thread froze first
                                  capture snapshot
                                  write report
                                  enter cooldown
                              else if frozen-thread heartbeat resumed:
                                  annotate latest report with Resolved at T+Xs
```

Why dual:

- A render-thread hang (driver wait, GPU lockup) leaves `Main::Update` ticking;
  catching this requires a `Present` heartbeat.
- A main-thread freeze (Havok / Papyrus / animgraph deadlock) usually leaves
  `Present` *also* stalled, but identifying the original culprit needs the
  classification logic.
- The report header gains a `Stalled thread:` line that is invaluable triage.

Key properties:

- Both heartbeats are `std::atomic<uint64_t>` storing `GetTickCount64()`.
  Lock-free, ~1 ns per update.
- Snapshot capture runs **on the watchdog thread**, which is healthy. It uses
  Win32 / DbgHelp APIs that operate on other threads (`SuspendThread`,
  `GetThreadContext`, `StackWalk64`).
- Snapshot is **rate-limited** by `snapshot_cooldown_s` so a permanent freeze
  doesn't generate duplicate reports.

---

## 5. Architecture

### 5.1 Components

| Component | Responsibility |
|---|---|
| `PluginEntry` | SKSE plugin bootstrap, version handshake, init/shutdown |
| `Config` | Load `FreezeLogger.toml` from `Data/SKSE/Plugins/` |
| `Heartbeat` | Two atomic timestamps (`main`, `render`) + accessors |
| `MainHook` | Hooks `Main::Update`, bumps `mainHeartbeat` |
| `RenderHook` | Detours `IDXGISwapChain::Present` (vtable slot 8) after `BSGraphics::Renderer::Init_InitD3D` runs; bumps `renderHeartbeat` on every Present |
| `Watchdog` | Background thread; detects stalls; classifies; orchestrates snapshots; handles annotate-on-resolve |
| `Symbols` | DbgHelp init with MS symsrv, mutex-guarded resolver |
| `Snapshot::Threads` | Stack-walks every thread in the process |
| `Snapshot::Modules` | Enumerates loaded DLLs (versions, paths, base addresses) |
| `Snapshot::Papyrus` | Captures VM stats: active stacks, suspended scripts, queue |
| `Snapshot::AnimGraph` | Captures **player-only** animation-graph state (lite) |
| `Snapshot::Engine` | Captures cell, worldspace, player position, time of day, pause flags |
| `Snapshot::System` | Captures memory, CPU load, GPU info, working set |
| `Snapshot::MiniDump` | `MiniDumpWriteDump` (default off, gated by config) |
| `RingBuffer` | Thread-safe ring of recent Papyrus log lines + SKSE messages |
| `PapyrusLogTap` | Hooks/registers as Papyrus log sink, feeds the ring buffer |
| `SkseMessageTap` | Listens on SKSE messaging interface, feeds the ring buffer |
| `Reporter` | Formats and writes the human-readable report file |
| `Logger` | Plugin's own log via `SKSE::log` (spdlog under the hood) |
| `DebugTriggers` | (compiled out unless `FL_DEBUG_TRIGGERS=ON`) hotkey + env-var synthetic stall |

### 5.2 Threading Model

- **Main thread** writes only `mainHeartbeat`. No locks on the hot path.
- **Render thread** writes only `renderHeartbeat`. No locks on the hot path.
- **Watchdog thread** is a `std::jthread` started in `kMessage_PostLoad`,
  joined on shutdown. Owns all snapshot work.
- **No re-entry**: the watchdog sets an `in_progress` flag; second triggers
  during a snapshot are coalesced.
- **DbgHelp serialization**: a single mutex inside `Symbols` guards every
  DbgHelp call (DbgHelp is not thread-safe).
- **Ring buffer**: feeders (Papyrus log tap, SKSE messaging tap) push under a
  short mutex; watchdog reads under the same mutex during snapshot.

### 5.3 Data Flow

```
[Main::Update]      ──► MainHook::tick   ──► mainHeartbeat = now
[Present]           ──► RenderHook::tick ──► renderHeartbeat = now
[Papyrus log line]  ──► PapyrusLogTap    ──► RingBuffer.push(papyrus, line)
[SKSE message]      ──► SkseMessageTap   ──► RingBuffer.push(skse, msg)
                                       │
                                       ▼
              Watchdog (every check_interval_ms):
                ages = (now - mainHeartbeat, now - renderHeartbeat)
                if max(ages) > threshold and not cooling:
                    classify (which thread crossed first / by how much)
                    Snapshot orchestrator:
                      Threads → Modules → Papyrus → AnimGraph
                      → Engine → System → RingBuffer → MiniDump?
                    Reporter.write(report)
                    enter cooldown
                else if cooling and frozen-thread resumed:
                    Reporter.annotate_resolved(latest_report, T+Xs)
```

Output target:

```
Documents/My Games/Skyrim Special Edition/SKSE/FreezeLogger/
    freeze_YYYY-MM-DD_HHMMSS_<thread>.log         # primary report
    freeze_latest.log                             # rewritten on every snapshot
    minidumps/freeze_YYYY-MM-DD_HHMMSS.dmp        # if enabled
    symbols/                                      # MS symsrv local cache
```

`<thread>` is `main`, `render`, or `both` — matches the classification.

---

## 6. Diagnostics Captured per Snapshot

Section order in the report:

1. **Header**
   - Plugin name + version, build SHA, timestamp (UTC + local).
   - Skyrim runtime version, FreezeLogger config in effect.
   - `Stalled thread:` (`main` / `render` / `both`), heartbeat ages at trip
     time, configured threshold.
   - `Symbol server:` line carrying the symsrv URL string for downstream
     re-symbolication.

2. **System**
   - OS version, total/available RAM, page-file usage.
   - Process working set, private bytes, handle count.
   - CPU model, core count, current load.
   - GPU adapter & driver version (best-effort via DXGI).

3. **Threads**
   - For every thread: TID, OS thread name (if set via `SetThreadDescription`),
     priority, suspend count.
   - **Full call stack** with symbol resolution. The two known threads — the
     hooked `Main::Update` thread and the hooked `Present` thread — are
     labeled (`[main game thread]`, `[render thread]`) so they're trivial to
     find in the report.
   - 256-frame cap, 4096-char-per-symbol cap.

4. **Loaded modules**
   - Path, base address, size, file version, PE timestamp.
   - Modules under `Data\SKSE\Plugins` are flagged so SKSE plugin DLLs are
     visible at a glance.

5. **Papyrus VM**
   - Active stack count, suspended count, frozen count.
   - Top N longest-running stacks (script + function name).
   - Pending event queue depth.

6. **Animation graph (lite)**
   - Player only.
   - Current behavior graph file, current animation event, last animation
     event, time in current animation state.
   - Flag if player has been in a transition for > 1 s.

7. **Engine state**
   - Current cell + worldspace EditorIDs.
   - Player position / orientation.
   - In-game time, real time since startup.
   - Pause flags (menu open, console open, fast-travel in progress, etc.).
   - Save-blocking flags.

8. **Recent activity (ring buffer)**
   - Last **100** Papyrus log lines (timestamped at capture).
   - Last **50** SKSE messaging events (with type code + sender).

9. **Mini-dump status** *(only present if mini-dump is enabled)*
   - Path, byte size, MiniDump flags actually used.
   - Failure reason if `MiniDumpWriteDump` returned an error.

Every section is wrapped in SEH + C++ exception handlers; if a section faults,
it is replaced with `<unavailable: caught SEH 0x...>` and the rest of the
report still emits. Each section flushes immediately so a force-kill mid-write
still leaves a partial-but-readable file.

---

## 7. Implementation Notes

### 7.1 SKSE Integration

- CommonLibSSE-NG, vcpkg manifest mode.
- `SKSEPluginVersion` declares:
  - Plugin name `"FreezeLogger"`, version follows `MAJOR.MINOR.PATCH.BUILD`.
  - `usesAddressLibrary = true`.
  - `compatibleVersions = { SKSE::RUNTIME_SSE_1_5_97 }`.
- Plugin load also checks the actual runtime version at startup; any mismatch
  → log error, refuse to install hooks. (Belt + braces with the
  `compatibleVersions` declaration.)

Init order:

1. `SKSEPluginLoad` → install logger, parse `FreezeLogger.toml`, init DbgHelp
   with the MS symsrv path.
2. `kMessage_PostLoad` → install both frame hooks; start the watchdog thread.
3. `kMessage_DataLoaded` → enable Papyrus snapshotter, install Papyrus log tap,
   enable animgraph lite snapshotter; start `DebugTriggers` (if compiled in).
4. On process exit / SKSE unload → request watchdog stop, join, flush logs.

### 7.2 Frame Hooks

Both hook bodies are minimal — single relaxed atomic store, then tail-call
the original via the trampoline:

```cpp
g_mainHeartbeat.store(GetTickCount64(), std::memory_order_relaxed);
return _originalMainUpdate(self, deltaTime);
```

```cpp
g_renderHeartbeat.store(GetTickCount64(), std::memory_order_relaxed);
return _originalPresent(this, syncInterval, flags);
```

Hook IDs are resolved through Address Library (`REL::ID(...)`); the exact IDs
are TBD and pinned during initial bring-up.

### 7.3 Stack Walking & Symbols

- `SymInitialize(GetCurrentProcess(), search_path, FALSE)` once at plugin load.
- `search_path` is composed as
  `"<output_dir>\\symbols;SRV*<output_dir>\\symbols*https://msdl.microsoft.com/download/symbols"`.
- `SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES | SYMOPT_AUTO_PUBLICS)`.
- All DbgHelp calls go through `Symbols::with_lock(...)` which holds a single
  process-wide mutex.
- Per-thread walk: `OpenThread` → `SuspendThread` → `GetThreadContext` →
  `StackWalk64` loop → `ResumeThread`. Skip the watchdog's own TID.
- The plugin's own `FreezeLogger.pdb` is copied next to `FreezeLogger.dll` so
  our frames symbolicate without any download.

### 7.4 Configuration

`Data/SKSE/Plugins/FreezeLogger.toml`:

```toml
[watchdog]
threshold_ms        = 5000
check_interval_ms   = 500
snapshot_cooldown_s = 60
annotate_on_resolve = true

[snapshot]
include_threads     = true
include_modules     = true
include_papyrus     = true
include_animgraph   = true        # lite (player only) in v1
include_engine      = true
include_system      = true
include_ringbuffer  = true
max_threads         = 64

[ringbuffer]
papyrus_lines       = 100
skse_events         = 50

[minidump]
enabled             = false
flags               = "normal+threadinfo+indirect"   # or "normal" / "fullmemory"
retain_last_n       = 5

[output]
directory           = ""          # empty = Documents/My Games/.../SKSE/FreezeLogger
keep_last_n_reports = 50

[symbols]
use_ms_symbol_server = true
cache_directory      = ""         # empty = <output>/symbols

[logging]
level               = "info"      # trace | debug | info | warn | error
```

### 7.5 Safety & Robustness

- Every snapshot routine is wrapped in **both** `__try`/`__except` (SEH) and
  `try`/`catch` so a fault in diagnostic code never takes the game with it.
- Kill-switch env var: `FL_DISABLE=1` skips watchdog start so the user can
  boot Skyrim "with the plugin installed but inert" without uninstalling.
- The plugin never writes save-game state, never modifies forms, never holds
  any RE:: pointer past the snapshot routine.
- Force-kill safety: every section flushes (`fflush` + `FlushFileBuffers`)
  immediately after writing; no more than one section's worth of data can be
  lost.

### 7.6 Performance Budget

- Steady-state per-frame cost: **< 10 ns per hook** (one relaxed atomic each).
- Watchdog wake cost: **< 50 µs every 500 ms** (default).
- Ring-buffer push cost: **< 100 ns** (mutex-guarded enqueue).
- Snapshot cost: not budgeted — only runs after the game is already frozen,
  rate-limited by `snapshot_cooldown_s`.

### 7.7 Debug Triggers (compiled out of release)

When CMake is configured with `-DFL_DEBUG_TRIGGERS=ON`:

- **Hotkey**: `VK_PAUSE`. Pressing it from the game causes the next
  `Main::Update` to `Sleep(10 * 1000)`. Heartbeat stops; watchdog trips.
- **Env vars**:
  - `FL_TEST_FREEZE_AFTER_S=N` → schedule a synthetic stall N seconds after
    `kMessage_DataLoaded`.
  - `FL_TEST_FREEZE_DURATION_S=M` → length of the stall (default 10).
  - `FL_FAKE_HEARTBEAT=1` → spawn a 10 Hz background thread that bumps
    both heartbeats on behalf of the (un-installed) hooks. Synthetic
    stalls pause this thread instead of the main game thread.
    Purpose: validate the watchdog → snapshot → reporter → retention
    pipeline before the real REL::IDs are pinned in `MainHook` /
    `RenderHook`. See the test-before-pinning workflow in `README.md`.
- Both stall paths use the same `InduceStall(seconds)` function so we
  exercise exactly one stall mechanism, regardless of mode.

---

## 8. Project Layout

```
freeze-detector/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json                       # commonlibsse-ng, spdlog, tomlplusplus, catch2
├── README.md                        # dev-only: how to build, how to trigger
├── .gitignore
├── docs/
│   ├── spec.md                      # ← this file
│   └── ghidra-bring-up.md           # how to pin Main::Update / Present via Ghidra
├── tools/
│   └── ghidra/
│       └── find_present_callers.py  # Ghidra Jython script: enumerates Present callers
├── src/
│   ├── main.cpp                     # SKSE entry points
│   ├── Config.{h,cpp}
│   ├── Heartbeat.{h,cpp}            # two atomics + accessors
│   ├── MainHook.{h,cpp}
│   ├── RenderHook.{h,cpp}
│   ├── Watchdog.{h,cpp}
│   ├── Symbols.{h,cpp}              # DbgHelp + symsrv init
│   ├── Reporter.{h,cpp}
│   ├── Logger.{h,cpp}               # SKSE::log wrapper
│   ├── RingBuffer.{h,cpp}
│   ├── PapyrusLogTap.{h,cpp}
│   ├── SkseMessageTap.{h,cpp}
│   ├── DebugTriggers.{h,cpp}        # compiled out unless FL_DEBUG_TRIGGERS=ON
│   └── snapshot/
│       ├── Threads.{h,cpp}
│       ├── Modules.{h,cpp}
│       ├── Papyrus.{h,cpp}
│       ├── AnimGraph.{h,cpp}        # player-only, lite
│       ├── Engine.{h,cpp}
│       ├── System.{h,cpp}
│       └── MiniDump.{h,cpp}
├── tests/
│   ├── CMakeLists.txt
│   ├── test_heartbeat.cpp           # Catch2
│   ├── test_watchdog_timing.cpp
│   └── test_ringbuffer.cpp
├── dist/
│   ├── FreezeLogger.toml            # default config shipped with the mod
│   └── README.txt                   # short user-facing readme (added when going public)
└── packaging/
    └── make_release.ps1             # builds + assembles MO2 RAR archive
```

---

## 9. Build

- **Toolchain:** Visual Studio 2022 (MSVC v143), Windows SDK 10.0.22621+,
  CMake ≥ 3.26.
- **Dependency manager:** vcpkg in **manifest mode**, `builtin-baseline`
  pinned in `vcpkg.json` for fully reproducible builds.
- **Dependencies** (`vcpkg.json`):
  - `commonlibsse-ng`
  - `spdlog` (transitive via `commonlibsse-ng`, listed explicitly for clarity)
  - `tomlplusplus`
  - `catch2` (test-only feature)
- **CMake options:**
  - `FL_DEBUG_TRIGGERS` — default `OFF`. Enables hotkey + env-var stall.
  - `FL_BUILD_TESTS` — default `ON` for local dev.
- **Output:** `FreezeLogger.dll` (Release/x64) + `FreezeLogger.pdb`.
- **PDB shipping:** PDB is copied next to the DLL during install so DbgHelp
  symbolicates our own frames without any download.

### 9.1 Local Tooling Available on the Dev Machine

These are paths on the developer workstation; document them here so future-us
(and any agent picking up the work) can resolve the open questions in §12
without re-discovering the environment.

| Tool | Path | Used for |
|---|---|---|
| **Ghidra 12.0.4** | `D:\Programme\ghidra_12.0.4` | Reverse-engineering Skyrim's binary to pin REL::IDs (`Main::Update`, `BSGraphics::Renderer::Present`); inspecting Papyrus VM internals for the log-sink hook; verifying function signatures before writing trampolines. |
| **SkyrimSE.exe (unpacked)** | `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe` | The Steam-unpacked 1.5.97 binary (decrypted, no Steam DRM stub) — the only form Ghidra can analyse cleanly. Drop this into a Ghidra project once and reuse the resulting database for every reverse-engineering session. |
| **Stock game directory** | `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\` | Source of `version-1-5-97-0.bin` (Address Library), reference for any other companion binaries. |

Bring-up workflow for resolving REL::IDs (§12 open question 1) — concrete
step-by-step instructions live in [`docs/ghidra-bring-up.md`](ghidra-bring-up.md).
Summary:

1. Import `SkyrimSE.exe.unpacked.exe` into a Ghidra project once; let auto-analysis finish.
2. Locate the target function (`Main::Update` / `Renderer::Present`) by RTTI,
   `WinMain` trace, or — for `Present` — by cross-referencing the
   `IDXGISwapChain::Present` import (automatable via
   `tools/ghidra/find_present_callers.py`).
3. Note the function's RVA (absolute address minus image base, `0x140000000`).
4. Plug the result into `src/MainHook.cpp` / `src/RenderHook.cpp` either as
   an Address Library ID (`REL::ID(N)`) or as a direct RVA (`REL::Offset`).
   Both code paths are already wired; pick whichever is convenient.

The Ghidra database also doubles as a forensic resource when reading freeze
reports: a stack frame at `skyrim_se.dll+0xNNNNNN` can be looked up directly
in Ghidra to identify which engine subsystem was on the stack at freeze time.

---

## 10. Packaging for Mod Organizer 2

A release artifact is a single **RAR** archive whose internal directory
structure is exactly what MO2 expects when you drop a mod in via
*Install a new mod from an archive*:

```
FreezeLogger_v<version>.rar
└── SKSE/
    └── Plugins/
        ├── FreezeLogger.dll
        ├── FreezeLogger.pdb
        └── FreezeLogger.toml
```

> Notes
> - Top-level folder is `SKSE/`, **not** `Data/SKSE/`. MO2 mounts the mod's
>   root onto the game's `Data/` directory.
> - Default config is included so the plugin works out of the box.
> - Address Library 1.5.97 is a runtime dependency; we declare this in the
>   release notes, we do not bundle it.
> - The PDB is in the archive (~1 MB) so the user's own frames symbolicate.

`packaging/make_release.ps1`:

1. Build Release/x64 via CMake.
2. Stage the tree under `build/stage/SKSE/Plugins/`.
3. Copy `FreezeLogger.dll`, `FreezeLogger.pdb`, `dist/FreezeLogger.toml`
   into the stage.
4. Invoke `rar.exe a -r -m5 FreezeLogger_v<ver>.rar SKSE` from the stage dir.

Personal-first note: while we're not publishing yet, we still keep the
packaging script in-repo so the muscle memory exists when we do.

---

## 11. Validation Plan

1. **Synthetic stall — hotkey path**
   - Build with `-DFL_DEBUG_TRIGGERS=ON`.
   - Boot Skyrim, load any save.
   - Press `VK_PAUSE`. The next `Main::Update` sleeps 10 s.
   - Within `5000 + 500 ms` the watchdog must produce
     `freeze_<ts>_main.log` with non-empty Threads/Modules/Papyrus sections.

2. **Synthetic stall — env-var path**
   - Launch with `FL_TEST_FREEZE_AFTER_S=30 FL_TEST_FREEZE_DURATION_S=10`.
   - Confirm an unattended freeze + report after `kMessage_DataLoaded + 30 s`.

3. **Resolve annotation**
   - Use a 10 s synthetic stall; confirm the report grows a
     `Resolved at T+10.x s` line and that **no second report file** appears
     (only the existing one is annotated).

4. **Real-freeze regression**
   - Once a real organic freeze is captured, verify the report contains:
     (a) a non-trivial main-thread stack with at least one symbolicated
     `ntdll!` / `kernel32!` frame, (b) a populated module list,
     (c) Papyrus VM stats, (d) system memory line, (e) 100 ring-buffer
     Papyrus lines.

5. **No false positives**
   - Long load screens, alt-tab, and the in-game menu must not trip the
     watchdog. (Hooking `Main::Update` should make this automatic — both
     menus and load screens still call `Main::Update`.)

6. **Stability soak**
   - 24 h of normal play with `level = "debug"`. Acceptance criteria:
     zero crashes, < 5 MB plugin log, no measurable FPS regression
     (≤ 0.5 % in a fixed benchmark scene).

7. **Unit tests (Catch2)**
   - `Heartbeat`: atomic store/load semantics, monotonicity.
   - `Watchdog`: stall detection given mocked clock + heartbeat;
     cooldown semantics; resolve-annotation logic.
   - `RingBuffer`: thread-safe push/snapshot, ordering, capacity bound.

---

## 12. Open Questions (still to resolve during bring-up)

- **REL::IDs for the two hook targets — RESOLVED for 1.5.97.** Cross-verified
  against two independent commits of `doodlum/skyrim-community-shaders`
  (`08286310`, `783f5024`):
  - `RE::Main::Update` CALL site → `REL::ID(35551)`, offset `0x11F`
    (call-site inside the WinMain main loop; canonical NG idiom is
    `stl::write_thunk_call` rather than entry detour).
  - `BSGraphics::Renderer::Init_InitD3D` CALL site → `REL::ID(75595)`,
    offset `0x50` (used to defer install of the swap-chain detour).
  - `IDXGISwapChain::Present` → vtable slot 8 (Microsoft DXGI contract;
    no REL::ID needed, no Address Library dependency for this hop).
  Pinned values live in `src/MainHook.cpp` and `src/RenderHook.cpp` with
  inline citations. If a future runtime is targeted, see
  `docs/ghidra-bring-up.md`.
- **Papyrus log interception mechanism**: prefer registering as a
  `BSScript::ILogEventSink` if CommonLibSSE-NG exposes it; otherwise hook
  `BSScript::Internal::VirtualMachine::TraceStack`/log emission. Decide
  during PapyrusLogTap implementation. Verify the chosen vtable slot in
  Ghidra if the CommonLibSSE-NG headers are ambiguous.
- **AnimGraph lite accessor**: confirm `RE::Actor::GetAnimationGraphs` (or
  equivalent) returns usable state on 1.5.97.

The remaining items are *implementation* unknowns, not architectural ones,
and shouldn't gate the rest of the work.

---

## 13. Deferred to v2 / Future Extensions

| Item | Why deferred |
|---|---|
| **Cross-runtime support (AE, VR)** | Single target keeps v1 small; CommonLibSSE-NG already abstracts most differences |
| **Animation-graph snapshot for nearest-N actors** | Havok behavior-graph traversal is footgun-prone; player-only answers most freeze questions |
| **JSON sidecar** | Low value for a personal tool; trivial to add later if a downstream tool wants it |
| **Live overlay** (ImGui heartbeat dashboard) | Pure quality-of-life; not diagnostic |
| **Auto-classification of likely culprit DLL** | Needs a corpus of freeze reports first |
| **Crash-logger interop** | Suppress our snapshot if CrashLoggerSSE handler fired in last second; needs cross-plugin coordination |
| **Public README + Nexus page** | Defer until v1 has caught a real freeze |

---

## 14. References

- SKSE64 — <https://skse.silverlock.org/>
- CommonLibSSE-NG — <https://github.com/CharmedBaryon/CommonLibSSE-NG>
- Address Library for SKSE Plugins (po3) — Nexus mod page
- DbgHelp `StackWalk64`, `MiniDumpWriteDump` — Windows SDK documentation
- Microsoft public symbol server —
  <https://msdl.microsoft.com/download/symbols>
