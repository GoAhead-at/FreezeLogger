# 02 - Cast of Tools

This investigation deliberately avoided black-box "crash analyzer" tooling in
favour of small, composable pieces. Each tool earned its place by addressing
a specific hole the previous one left.

## Build and runtime stack

| Tool                              | Role |
|-----------------------------------|------|
| **Visual Studio 2026 Community**  | C++23 compiler, MSVC linker, Windows SDK headers, DbgHelp, MiniDumpWriteDump. The bundled CMake (`Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`) is what we drive from PowerShell because plain `cmake` is not on `PATH` in this workspace. |
| **CMake 3.26+ presets**           | `windows-x64-release`, `windows-x64-debug`, `windows-x64-test`. Single-source for build configurations; Visual Studio 18 generator with x64 architecture. |
| **vcpkg (manifest mode)**         | Pulls `commonlibsse-ng`, `tomlplusplus`, `catch2`, with the `x64-windows-static` triplet so the shipped DLL has no extra runtime dependencies. |
| **CommonLibSSE-NG**               | The de-facto standard SKSE engine binding. Provides `REL::Module`, `REL::Relocation`, the address-library plumbing, the SKSE plugin entry-point macros, and stable `RE::*` types. |
| **Catch2 v3**                     | 27 unit tests for the watchdog state machine, ringbuffer behaviour, and snapshot classification. Lets us refactor the diagnostic code without losing the parts that don't depend on a live game. |
| **WinRAR**                        | Final packaging step - the staged `SKSE/Plugins/*.dll/.pdb/.toml` tree is wrapped in `FreezeLogger_v0.1.0.rar` for direct MO2 install. |
| **Mod Organizer 2**               | Test deployment target. The Nolvus Awakening instance lives at `D:/SPIELE/nolvus/Instances/Nolvus Awakening/`. |

## In-process diagnostic primitives

These are not external tools - they are the Win32 / NT facilities we lean on
inside the plugin itself:

- **DbgHelp** - `SymInitialize`, `SymFromAddr`, `StackWalk64`, `MiniDumpWriteDump`. Used under a single
  serialising `std::mutex` because DbgHelp is not thread-safe.
- **TlHelp32** - `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, ...)` to enumerate
  every thread in the process, including ones not owned by Skyrim.
- **`SuspendThread` / `GetThreadContext` / `ResumeThread`** - the only way to
  reliably walk a stack and read CPU state of a thread other than your own.
  Wrapped in an RAII guard so even an unwinding C++ exception always resumes
  the thread.
- **`NtQueryEvent`** (ntdll private API) - read-only inspection of a kernel
  event handle: returns `{type, signaled}`. Critical: this does not consume
  the signal. `WaitForSingleObject(h, 0)` would steal the signal from
  whichever thread was scheduled to receive it next.
- **Structured Exception Handling (SEH)** - `__try`/`__except` blocks gate
  every memory dereference of game-controlled pointers. Required because
  `MSVC /EHsc` cannot mix `__try` with C++ object unwinding, so SEH-safe
  primitive readers are isolated in `noexcept` helpers.
- **`MiniDumpWriteDump`** with flags `Normal | WithThreadInfo | WithIndirectlyReferencedMemory`.
  Produces 30-90 MiB dumps that replay completely in WinDbg.

## Static analysis stack

| Tool                                   | Role |
|----------------------------------------|------|
| **Ghidra 12.0.4** (`D:/Programme/ghidra_12.0.4`) | Loaded the unpacked Steam binary `SkyrimSE.exe.unpacked.exe` to read function bodies, follow xrefs, and confirm calling conventions. Used during the early Site-A investigation. |
| **Capstone (Python bindings)**         | Linear disassembly of arbitrary RVA ranges and pattern matching for inline lock-acquire sequences. Faster to script than Ghidra for "show me every RIP-relative reference to address X". |
| **WinDbg [Microsoft.WinDbg] v1.2603**  | Opened the early minidumps to confirm thread states. Once the in-plugin probes matured this was rarely needed; the `.dmp` files are still emitted as a safety net. |
| **Address Library for SKSE Plugins**   | The meh321 RVA-to-ID database (`version-1-5-97-0.bin`). Without it, every disassembly excerpt would refer to volatile RVAs; with it, the engine functions in this case study are stable across all 1.5.97 builds. |
| **The unpacked Steam binary**          | `D:/SPIELE/nolvus/Instances/Nolvus Awakening/STOCK GAME/SkyrimSE.exe.unpacked.exe`. The ENB+stub-stripped copy that Ghidra and capstone can statically analyse. The shipped Steam binary is wrapped in a Steam DRM stub which obscures the .text section. |
| **PE parsing helpers**                 | Python `struct` snippets to parse the PE header, locate `.text`, and translate RVA to file offset. Used in every `analysis/*.py` script. |

## Custom analysis scripts

Every script in `analysis/` was written for this investigation. They are
small enough to throw away and rewrite, and they target one question each.

| Script                     | Question it answers |
|----------------------------|---------------------|
| `addrlib_lookup.py`        | Given a list of RVAs, which Address Library IDs are nearest? Used to label every stack frame in early reports before the in-plugin AddrLib loader existed. |
| `addrlib_quicklookup.py`   | Reverse: given an ID, what RVA does it map to in 1.5.97? |
| `enumerate_imports.py`     | Walk the PE Import Address Table to identify which `kernel32`/`ntdll` API a particular IAT slot calls. Used to confirm that `SkyrimSE+0x1509288` is `KERNEL32!WaitForSingleObject`. |
| `disasm_targets.py`        | Linear disassembly of N target functions plus optional "site anchored" 64-byte windows. Iteratively re-targeted as the investigation moved from Site A -> Site B -> spinlocks. |
| `xref_scan.py`             | Find every RIP-relative reference in `.text` whose target lands inside a configurable data region. First used for Singleton-A / Singleton-B coordinates; later widened. |
| `xref_locks.py`            | A specialisation of `xref_scan.py` for the two BSSpinLock globals identified in the final freeze. Output drove the AB-BA confirmation. |
| `xref_debug.py`            | A debugging helper for `xref_scan` itself - prints addressing-mode breakdowns when the scan misses an expected reference. |
| `find_pending_setevent.py` | Hunt for the producer side of the wait: every function that writes `1` to a singleton's `+0x6c` "pending" flag and calls `SetEvent` on the matching handle. Confirmed that the producer pattern is `id 34553`. |

## What we considered and rejected

- **Bethesda Crash Logger** - only triggers on hard exceptions. A
  WaitForSingleObject loop is well-behaved code from the OS perspective and
  does not cause an exception, so it cannot be caught here.
- **Papyrus-side watchdog** - any Papyrus VM observer freezes alongside the
  game when the engine main loop stalls.
- **A pure offline post-processor** consuming SKSE crashlog `.log` files -
  rejected once we realised most of the data we needed is gone by the time
  the user kills the process. We need an in-process snapshot taken *while*
  the freeze is happening.
- **Hooking BSSpinLock::Acquire to log every acquisition** - tempting, but
  the per-acquire overhead would be many millions of log lines per second.
  Used briefly during the heuristic-detector design phase as a thought
  experiment; the heuristic candidate detector achieves the same goal at
  zero runtime cost (it only runs at freeze time).
- **Recursion FPS Fix as the explanation** - early hypothesis. Ruled out
  after we confirmed Recursion Fix never fired and the freezes happened
  with it disabled too.
- **HDT-SMP as the explanation** - confirmed via source review and stack
  inspection that `hdtsmp64.dll` is a passthrough hook for `Main::Update`,
  not the holder of the lock that main is blocked on.
