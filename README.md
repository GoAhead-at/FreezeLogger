# FreezeLogger

SKSE plugin that detects Skyrim hard freezes from a healthy watchdog thread
and writes a diagnostic report (threads, modules, Papyrus VM, animgraph,
engine state, recent activity) when the main thread or render thread stops
advancing.

Personal-first. SE 1.5.97 only. Design rationale: see [`docs/spec.md`](docs/spec.md).

---

## Prerequisites

- Visual Studio 2022 (with the **Desktop development with C++** workload, MSVC v143)
- CMake ≥ 3.26 (bundled with VS2022)
- vcpkg cloned somewhere on disk, with `VCPKG_ROOT` set as an environment variable
- Skyrim Special Edition **1.5.97** installed (any other runtime → plugin refuses to load)
- Address Library for SKSE Plugins (1.5.97 bin) installed via MO2

### Local tooling expected on this dev machine

| Tool | Path |
|---|---|
| Ghidra 12.0.4 | `D:\Programme\ghidra_12.0.4` |
| Unpacked SkyrimSE 1.5.97 binary | `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe` |
| Stock game folder | `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\` |

---

## First-time setup

```powershell
# Pin the vcpkg ports baseline so builds are reproducible.
# Run this ONCE after cloning. It writes "builtin-baseline" into vcpkg.json.
vcpkg x-update-baseline --add-initial-baseline
```

## Build

```powershell
# Release (synthetic triggers OFF; what you'd ship)
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release

# Debug (synthetic triggers ON; for development)
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

Output: `build/<release|debug>/Release/FreezeLogger.dll` (and `.pdb`).

## Run unit tests

```powershell
ctest --preset windows-x64-debug
```

---

## Install into MO2 (development workflow)

1. Build (above).
2. Copy `FreezeLogger.dll` + `FreezeLogger.pdb` + `dist/FreezeLogger.toml`
   into MO2's `mods\FreezeLogger\SKSE\Plugins\` (create the mod first if needed).
3. Enable the mod, launch Skyrim through MO2.
4. Plugin log: `Documents\My Games\Skyrim Special Edition\SKSE\FreezeLogger.log`
5. Freeze reports: `Documents\My Games\Skyrim Special Edition\SKSE\FreezeLogger\`

---

## Verifying the watchdog catches a freeze (debug build)

After launching with the **debug** build (synthetic triggers ON):

- **Hotkey path**: in-game, press `Pause/Break`. The next `Main::Update`
  sleeps 10 s. Within ~5.5 s the watchdog must produce a
  `freeze_<timestamp>_main.log` in the FreezeLogger output dir.

- **Env-var path** (unattended): launch the game with
  ```powershell
  $env:FL_TEST_FREEZE_AFTER_S = '30'
  $env:FL_TEST_FREEZE_DURATION_S = '10'
  ```
  set in the launching shell. A freeze fires 30 s after `kMessage_DataLoaded`.

- **Kill-switch**: set `FL_DISABLE=1` to load the plugin but install no hooks.

### Smoke-testing the watchdog without launching Skyrim through MO2

The hooks are pinned, but if you want to validate the rest of the pipeline
(watchdog → snapshot → report write → retention) without a running game
instance — e.g. to iterate on the snapshot/reporter code — set
**`FL_FAKE_HEARTBEAT=1`**: a 10 Hz background thread bumps both heartbeats
on the plugin's behalf, and synthetic stalls pause that thread instead of
the main game thread.

```powershell
# Build the debug preset (synthetic triggers compiled in)
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug

# Launch Skyrim through MO2 with these env vars set:
$env:FL_FAKE_HEARTBEAT       = '1'
$env:FL_TEST_FREEZE_AFTER_S  = '15'
$env:FL_TEST_FREEZE_DURATION_S = '10'
```

After 15 s the fake heartbeat pauses, the watchdog trips, and you get a
freeze report under `…\SKSE\FreezeLogger\`. The report's threads section
will *not* show our synthetic Sleep (because nothing is actually blocking
the game's main thread — that's the whole point of fake-heartbeat mode),
but every other section is exercised end-to-end against the real running
process. Use this to validate the snapshot code, the reporter formatting,
the retention policy, and the DbgHelp symbol resolution before spending
Ghidra time.

Once the real hooks are installed, drop `FL_FAKE_HEARTBEAT` and use the
hotkey / env-var paths above to test against actual main-thread blocking.

---

## Bring-up TODOs (current work)

The two heartbeat hooks are now **pinned and verified** for 1.5.97:

- `RE::Main::Update` → `REL::ID(35551)` + `0x11F` (CALL site, NG-idiom
  `stl::write_thunk_call`)
- `IDXGISwapChain::Present` → vtable slot 8, installed after
  `BSGraphics::Renderer::Init_InitD3D` (`REL::ID(75595)` + `0x50`) runs

Both IDs are cross-referenced from two independent commits of
`doodlum/skyrim-community-shaders` (a high-profile, actively-maintained NG
plugin); inline citations live in `src/MainHook.cpp` and `src/RenderHook.cpp`.

The remaining `TODO_RE` items are not bring-up blockers — the plugin builds
and runs without them; they only widen the diagnostic surface:

1. **Papyrus log sink mechanism** → `src/PapyrusLogTap.cpp`
2. **AnimGraph lite accessor** → `src/snapshot/AnimGraph.cpp`
3. **Engine snapshot accessors** marked `TODO_RE` in `src/snapshot/Engine.cpp`
   — verify CommonLibSSE-NG field names compile against your local NG version.

For these, see `docs/ghidra-bring-up.md` (and the
`tools/ghidra/find_present_callers.py` helper script) — kept as reference
for any future maintenance, e.g. adding AE/VR support.

---

## Repository layout

```
.
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md                       ← this file
├── docs/
│   └── spec.md                     ← architecture + design rationale
├── src/                            ← plugin source
│   └── snapshot/                   ← per-section diagnostic capture
├── tests/                          ← Catch2 unit tests
├── dist/
│   └── FreezeLogger.toml           ← default config shipped with the plugin
└── packaging/
    └── make_release.ps1            ← builds + packages an MO2-ready RAR
```

License: deferred (personal-first; not yet published).
