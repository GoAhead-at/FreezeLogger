# 04 - Plugin Evolution

This file documents what the plugin gained at each iteration, why each
addition was needed, and the design constraints that shaped the
implementation.

## v0.1.0 baseline (May 14)

### Goal
Catch a freeze in progress and write a structured text report covering
threads, modules, system, Papyrus, animgraph, engine state, and a recent-
activity ringbuffer.

### Architecture

```
+-----------------------+        +------------------+
| Main::Update hook     |---+    | Watchdog thread  |
+-----------------------+   |    +------------------+
                            |             |
+-----------------------+   |    +------------------+
| Renderer::Present hook|---+--->| Heartbeat counters|
+-----------------------+        +------------------+
                                            |
                                            v
                                +---------------------+
                                | Reporter            |
                                |  -> Threads section |
                                |  -> System section  |
                                |  -> ...             |
                                |  -> MiniDump (opt)  |
                                +---------------------+
```

### Key design decisions

- **Dual heartbeat hooks**, not just `Main::Update`. `Renderer::Present`
  catches render-thread stalls that don't show up in main.
- **Watchdog runs on its own native thread.** The whole point is that the
  observer must survive a main-thread stall.
- **`SuspendThread` -> `GetThreadContext` -> `ResumeThread` for every walked
  thread.** Required for `StackWalk64` to give meaningful results.
- **An RAII `SuspendGuard`** so even an exception unwinding through the
  walk always resumes the thread. Mis-resuming would freeze the user's
  game *forever* with no recovery short of killing the process.
- **`Symbols::Lock` mutex around DbgHelp.** DbgHelp is documented as not
  thread-safe; we walk many threads back-to-back, so the lock is held
  for the entire stack-walk.
- **`SymOptions = SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES | SYMOPT_AUTO_PUBLICS | SYMOPT_UNDNAME`**. Deferred
  loads make `SymInitialize` cheap; we only pay the cost for modules that
  actually appear in a stack frame.
- **Microsoft public symbol server is OFF by default.** A network
  round-trip while threads are suspended can extend the in-game freeze
  by many seconds; users opt in via the TOML when they specifically need
  Windows/CRT frames symbolicated.
- **Minidump is OFF by default in v0.1.0.** Reverted later (see below).

### Watchdog state machine

The watchdog is small but tested under 27 Catch2 unit cases. It tracks:
threshold, check interval, snapshot cooldown, and an
`annotate_on_resolve` flag that makes a transient stall annotate the
existing report rather than emit a duplicate.

## Iteration 1: `MainWaitProbe` (May 17, post-freeze 1)

### Why
The first real freeze had main parked in `WaitForSingleObjectEx`. The
threads section gave us the call stack but not what the wait *meant*.
We needed to read the singleton's `pending` flag and the wait-event's
state.

### What changed
- Added `src/snapshot/MainWaitProbe.cpp`.
- Constants pinning Singleton-A and the lock primitive's RVA range.
- Helpers:
  - `TryReadQword`, `TryReadFields` (SEH-isolated readers).
  - `LoadNtQueryEvent` to obtain the ntdll private API.
  - `TrySawWaitReturnAddr` to detect Site A by scanning RSP.

### Lessons learned

- **`__try` and C++ object unwinding don't mix under `/EHsc`.** All SEH
  blocks must be isolated in `noexcept` helpers that take primitive
  out-parameters; the formatting (which uses `std::format` and
  `std::string`) is done outside.
- **`NtQueryEvent` (instead of `WaitForSingleObject(h, 0)`) matters.**
  An auto-reset event whose state we sneak-peeked with a zero-timeout
  wait would have its signal consumed; the deadlock would self-heal at
  freeze-report time and we would have inadvertently fixed the bug we
  were trying to diagnose. `NtQueryEvent` is read-only.

## Iteration 2: BSSpinLock probe (May 18 morning)

### Why
We had main's wait pinned but not the worker side. If the worker is
spinning in a BSSpinLock, we needed to enumerate which thread that is
and which lock they are spinning on.

### What changed (initial version)
- Walk every thread; if its top frame is `id 12210 +0x8a` (the
  `BSSpinLock::Acquire` slow path), capture the thread's CONTEXT.
- Use `RDI` as the lock pointer (the calling convention used by
  `BSSpinLock::Acquire`).
- Print the lock's `{owner, state}` and flag if the lock is held by
  the main thread.

### What broke
The May 18 PM freeze showed `RDI = 0` for every spinner. Walking
`BSSpinLock::Acquire` more carefully showed `KERNELBASE!SleepEx` is
called *between* the spin loop and the `NtDelayExecution` syscall, and
`SleepEx` clobbers the non-volatile RDI register before the syscall.
By the time we sample the suspended thread, RDI no longer points at
the lock.

### Iteration 2b: Heuristic candidate detector

- New helper `LooksLikeLock(uintptr_t v) -> {owner, state}`: validates
  that an address falls inside SkyrimSE.exe and that the qword at that
  address parses as `{plausible TID, state in 0..2}`.
- `CollectLockCandidates(CONTEXT&) -> vector<LockCandidate>`: scans all
  16 GP registers (`Rax..R15`) and a 1 KiB stack window for plausible
  lock pointers. Each unique candidate is recorded with its source
  ("RSI=", "stack=" + offset).
- `WriteSpinlockOwners()` rewrites the output to print every plausible
  candidate per spinning thread.
- The state check was tightened from `<= 4` (initial) to `<= 2` after
  inspecting one freeze where false positives were noisy.

## Iteration 3: Site B detection (May 18 PM)

### Why
The May 18 13:16 freeze had main in a different wait inside the same
`Main::Update` function: `+0x5b34fe -> +0xc38130` instead of `+0x5b35dd
-> +0x5765ff`. Probe was blind to the new site.

### What changed
- Added constants for Singleton-B at `SkyrimSE+0x2f26a70` and the
  wait-wrapper RVA range.
- New helper `WalkSingletonB`: walks the pointer chain
  `(*S)[+8][idx0]->vtable[idx1]` to recover the wait HANDLE, with each
  step SEH-isolated.
- `Write()` was substantially refactored:
  - Site detection up front: classify main as Site-A, Site-B, or neither.
  - RBX-as-HANDLE probe runs unconditionally.
  - Site-A and Site-B branches gated by detection result.
  - BSSpinLock probe runs unconditionally (no longer hidden behind
    Site-A success).
  - Final-verdict block tailored per site.

### Subtle issue
The first Site-B freeze report showed the Singleton-B sub-array pointer
was *null* during the probe even though main had clearly used it before
entering the wait. We added: hex dump of the singleton instance,
co-consumer search (other threads with the same RBX handle), toucher-
thread search (threads with Singleton-B's address anywhere in their
non-volatile registers). These features remain useful even though they
did not personally answer the Site-B "why was the producer null"
question - they will when Site B reappears in a future freeze.

## Iteration 4: The "yes" batch (May 19)

### Why
Each iteration to date had been "one feature per freeze". The user
correctly observed that multi-day debugging cycles were untenable;
the goal was to make the *next* freeze report self-sufficient.

### What changed

- **Address Library at runtime.** New `src/AddrLib.{h,cpp}` parses
  `Data/SKSE/Plugins/version-1-5-97-0.bin` (meh321 v1 format, the same
  one CommonLibSSE-NG itself loads) and exposes a sorted RVA-keyed
  vector. `Resolve(rva)` does a binary search for the predecessor and
  returns `{id, base_rva, delta}`. `FormatAnnotation(rva)` returns
  `"[id NNNN +0xN]"` or `""` if the delta exceeds 16 KiB (suppression
  prevents misleading "nearest ID" labels for very-large outlier
  functions).
- **Init order**: `Symbols::Init()` then `AddrLib::Init()`, both inside
  `SKSEPlugin_Load`. AddrLib failure is non-fatal: missing or malformed
  bin means `FormatAnnotation` always returns empty string.
- **Per-frame annotation in Threads section.** Every frame line now
  reads `#NN 0x... module!sym+0xN  [id NNNN +0xN]`. Pasteable into
  Ghidra/IDA without offline post-processing.
- **Per-thread non-volatile register line.** After the frames, two-line
  `nv-regs:` summary with `RBX/RBP/RSI/RDI` followed by `R12/R13/R14/R15`.
  Each register is correlated against `&Singleton-A.ptrSlot`, the
  Singleton-A struct address, and `&Singleton-B.ptrSlot`; matches are
  annotated `(=Singleton-A struct)` etc.
- **Per-thread "what are you waiting on?".** When a thread's top frame
  matches `WaitForSingleObject` / `ZwWaitForSingleObject` /
  `NtWaitForSingleObject`, we run `NtQueryEvent(rbx)` and print
  `waiting on: HANDLE=0x... [type, signaled-or-not]`. Multi-object
  waits are silently skipped (the array of handles cannot be probed
  from one register).
- **New `WaitGraph` section.** Cross-cuts every parked thread, groups
  by handle, prints a per-handle table with the count of waiters and
  the kernel event state, and identifies "classic dispatch+wait"
  topologies. The verdict line at the end of the section can match a
  one-thread-on-not-signaled-event scenario in O(threads) time.
- **Minidump default flipped to ON.** Default flags
  `Normal | WithThreadInfo | WithIndirectlyReferencedMemory` give a
  30-90 MiB dump, retained-last-5 by default. The previous
  conservative default was off-by-default with a TOML toggle; with
  `keep_last_n_reports=50` and `retain_last_n_dumps=5`, disk usage is
  bounded and the offline replay capability is now always available.

### Test coverage
27/27 Catch2 tests still pass. The new code paths in `Threads.cpp` and
`WaitGraph.cpp` are not currently unit-tested - they require a live
Skyrim process - but they are SEH-guarded and exception-safe by
construction.

### Result
The very next freeze (12:04) was diagnosed end-to-end from the report
without further plugin changes. Total identifications visible in that
single report:

- Main thread's exact wait site, handle, event type, and signaled state.
- For every other thread parked in a single-handle wait: its handle and
  event state, with one line per thread.
- A handle-grouped table identifying the dispatch+wait deadlock and a
  one-line "no other thread references this handle" verdict.
- Four spinlock candidates for each spinning worker, with owner TIDs
  and held/free state, immediately revealing the AB-BA cycle.
- A `.dmp` next to the `.log` for any deeper offline replay.

The remaining ambiguity (which functions take which lock and in what
order) is a static-analysis problem, not a live-debugging one - addressed
in `05-static-analysis.md`.

## What we'd build differently next time

- **The wait-graph section is verbose.** With 248 threads in
  `WaitForSingleObject*` it occupies ~300 lines. A future change should
  collapse "obviously idle thread pools" (32 waiters on one
  NotificationEvent with no other references) to a single summary line
  unless `--verbose` is set.
- **The heuristic spinlock detector reports too many false positives.**
  Tightening the address range to "must be inside `.data` or
  `.rdata`" plus rejecting `state == 0 && owner == 0` would cut roughly
  half the noise. The strong signals are still obvious in practice
  but a noisier reader would have a harder time.
- **AddrLib::FormatAnnotation could surface the function name.** Right
  now we emit `[id NNNN +0xN]`. We could augment with a known-symbol
  table (the public CommonLibSSE-NG offset constants) to print
  `[Main::Update +0x5ed]` for IDs we have human names for. Low priority.
