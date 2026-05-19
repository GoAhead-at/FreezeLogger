# WorkerSpinLockFix

SKSE plugin for **Skyrim SE 1.5.97** that breaks an AB-BA spinlock
inversion in the worker dispatcher by serializing two engine functions
through plugin-side mutexes.

This is the companion fix plugin for `FreezeLogger`. The bug it
addresses is documented in
`../docs/case-study/06-root-cause.md`.

## Status

**Pre-release / experimental.** Use only with `FreezeLogger`
running so any regression is captured.

### Version history / retractions

- **v0.1** (retracted): two per-function mutexes. Failed - introduced
  a fresh AB-BA cycle between `mtx_19369` and engine LockB.
  Reproduced the freeze on first deployment
  (`freeze_2026-05-19_140941`).
- **v0.2** (retracted): single shared mutex, but only covered
  `id 19369` and `id 40706`. Failed - LockB has FOUR more static
  acquirers (`id 40285`, `id 40333`, `id 40334`, `id 40335`) which
  v0.2 did not gate. A worker thread could acquire LockB through
  any of those, then later block on `g_section` while a different
  thread inside `id 19369` held LockA and spun on LockB. Same
  freeze (`freeze_2026-05-19_142140`).
- **v0.3** (retracted): added id 40285, id 40334, id 40335 but
  wrongly assumed id 40333 is reached only via id 19369. In fact
  id 40333 has 18 direct CALL/JMP sites (id 19000 calls it twice
  directly, plus 16 others). Same freeze
  (`freeze_2026-05-19_144128`) - TID 25832 acquired LockB through
  id 40333 outside any hook.
- **v0.4** (retracted): full id 40333 + missing id 40334 JMP.
  Same freeze on save load. The cause: `id 40335` is a dual-mode
  helper called by `id 18638` once to acquire LockB and once to
  release it. Between those two calls `id 18638`'s body holds
  LockB across many other calls without g_section. If any of
  those calls reaches a hooked function on a thread while another
  thread holds g_section, AB-BA returns.
- **v0.5** (current):
  1. Hooks `id 18638` itself (2 call sites). Now g_section spans
     the entire LockB-held interval inside `id 18638`.
  2. Adds CONTENTION LOGGING: every time a thread enters a hook
     and finds `g_section` already held by another thread, it
     logs both threads with their hook names BEFORE attempting
     to take the mutex. If the game freezes again, the
     `WorkerSpinLockFix.log` will identify exactly which hooks
     each blocked thread came from (no more guessing from
     corrupted stack walks).

**If you are running v0.1 - v0.4, replace the DLL with v0.5 or
set `plugin.enabled = false` in `WorkerSpinLockFix.toml` and
restart.**

## What it does

The freeze documented by FreezeLogger is an AB-BA cycle between
two `BSSpinLock` globals inside `SkyrimSE.exe`:

- `LockA` at `+0x2eff8e0`, taken inside `id 19369`.
- `LockB` at `+0x2f3b8e8`, taken inside `id 40706` (via
  `[arg + 0x150]`).

Two worker threads can hold one lock and wait on the other,
producing an indefinite freeze with main thread parked on the
worker-ack event.

This plugin redirects every direct `CALL` to the six known
LockA/LockB acquirers through a serializing thunk by patching
the 5-byte relative-call displacement at each site
(CommonLibSSE-NG's `trampoline.write_call<5>`). All thunks gate
on a **single shared `std::mutex`** with manual TID-tracked
re-entrance.

Site coverage (from `analysis/xref_calls.py` + cross-checked
with `analysis/find_all_lockb_callers.py`):

| Function | Lock taken | CALL | JMP | Total |
|----------|------------|-----:|----:|------:|
| `id 19369` | LockA      | 50 | 0 | 50 (incl. recursive `id 19369 +0x9d`) |
| `id 40706` | LockB (indirect via `[arg+0x150]`) |  4 | 0 |  4 |
| `id 40285` | LockB | 10 | 0 | 10 |
| `id 40333` | LockB | 17 | 1 | 18 |
| `id 40334` | LockB |  5 | 1 |  6 |
| `id 40335` | LockB (dual-mode helper, dl=1 acquire / dl=0 release) | 2 | 0 | 2 |
| `id 18638` | (only caller of `id 40335`; holds LockB across body) | 2 | 0 | 2 |

**Total: 90 CALL + 2 JMP = 92 patch sites across 7 functions.** At most one thread is ever inside any
of these functions at a time, which means LockA and LockB are
always taken sequentially by a single thread inside the same
critical section, so they cannot invert against each other.

Indirect calls (via vtable or function pointer) are NOT covered.
None have been observed for these functions in the analysed
binary, but they cannot be ruled out for future engine paths or
third-party mods.

The engine's BSSpinLocks still exist, but contention on them is
eliminated because at most one thread is ever inside the relevant
section at a time.

## Building

Prerequisites:

- Visual Studio 2026 with the C++23 toolchain.
- vcpkg, with `VCPKG_ROOT` set in the environment.
- Address Library for SKSE (only required at runtime, not build time).

From a PowerShell shell at the project root:

```powershell
$env:VCPKG_ROOT = "d:/Programme/Microsoft Visual Studio/18/Community/VC/vcpkg"
& "d:/Programme/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --preset windows-x64-release
& "d:/Programme/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build/release --config Release
```

The output DLL is `build/release/Release/WorkerSpinLockFix.dll`. It
must be installed together with `dist/WorkerSpinLockFix.toml`.

## Installing (Mod Organizer 2)

1. Build (above).
2. Copy `WorkerSpinLockFix.dll`, `WorkerSpinLockFix.pdb`, and
   `dist/WorkerSpinLockFix.toml` into a new MO2 mod folder under
   `SKSE/Plugins/`:

   ```
   <MO2 mod>/SKSE/Plugins/WorkerSpinLockFix.dll
   <MO2 mod>/SKSE/Plugins/WorkerSpinLockFix.pdb
   <MO2 mod>/SKSE/Plugins/WorkerSpinLockFix.toml
   ```

3. Activate the mod, launch via SKSE.
4. Confirm the log file at
   `Documents/My Games/Skyrim Special Edition/SKSE/WorkerSpinLockFix/WorkerSpinLockFix.log`
   contains `Hooks installed: id 19369 at 0x... id 40706 at 0x...`.

## Configuration

`WorkerSpinLockFix.toml`:

| Key                         | Default | Meaning |
|-----------------------------|---------|---------|
| `plugin.enabled`            | `true`  | Master kill-switch. `false` loads the plugin idle (no hooks). |
| `log.contention_warn_ms`    | `1`     | Emit a WARN line if a hook blocked at least this many ms. `0` disables. |
| `log.stats_interval_s`      | `60`    | Periodic counter-dump interval. `0` disables. |
| `log.trace_each_call`       | `false` | Per-call trace logging. SPAMMY. |

## Safety notes

- **Hard runtime pin.** The plugin refuses to install hooks on any
  build other than 1.5.97. Address Library IDs 19369 and 40706 only
  match this version.
- **Direct CALL sites only.** We patch every `E8 imm32` CALL whose
  target is the entry of `id 19369` (50 sites) or `id 40706`
  (4 sites). Indirect calls (vtable or register-relative) are not
  covered. None were observed during static analysis but a future
  Skyrim patch or a third-party mod that introduces an indirect
  call would bypass the mutex.
- **Wide opaque hook prototypes.** The two functions' exact C++
  prototypes are not fully confirmed. The hooks declare 8
  integer-class register/stack arguments and forward all of them
  to the original. This handles every plausible Skyrim function
  with up to 8 integer args. The residual risk is XMM register
  arguments, which we have not seen evidence of for these two
  functions.
- **Manual TID-tracked recursion.** `id 19369` is recursive. The
  per-thread depth counter prevents self-deadlock without paying
  for `std::recursive_mutex` overhead per call.
- **No dependency on FreezeLogger.** The two plugins are
  independent. Run them together for diagnostic + mitigation
  during the trial period; either can be uninstalled
  independently.

## Recovery / disable

If the plugin causes any problem (crash, perf regression, suspect
behaviour), set `plugin.enabled = false` in
`WorkerSpinLockFix.toml` and restart the game. The plugin will
load idle and the engine will run with its original (deadlock-prone)
worker dispatch.

## License

Same as the parent FreezeLogger project (currently unspecified;
deferred until publication).
