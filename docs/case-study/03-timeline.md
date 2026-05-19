# 03 - Investigation Timeline

This is the chronological narrative. Each entry corresponds to one or more
real freeze events captured by FreezeLogger; in-between entries the plugin
was extended to address whatever data the previous freeze had been missing.

The matching report files live in
`E:/SHARED/_STAEUBER/MyFiles/Documents/My Games/Skyrim Special Edition/SKSE/FreezeLogger/`.
Reports are named `freeze_YYYY-MM-DD_HHMMSS_<which>.log` where `<which>` is
`main`, `render`, or `both` depending on which heartbeat threads stalled.

## 2026-05-14 - Spec, scaffold, pipeline

- Wrote `docs/spec.md` after a structured design grilling that resolved
  ~16 open questions (runtime pin, hook sites, snapshot scope, retention,
  trigger mechanism, etc.).
- Built v0.1.0 scaffold: dual heartbeat hooks
  (`RE::Main::Update`, `BSGraphics::Renderer::Present`), watchdog thread
  monitoring two heartbeat counters, snapshot writer covering threads,
  modules, system info, Papyrus VM, animgraph (player only), engine
  state, and a recent-activity ringbuffer.
- Verified the pipeline using the synthetic-stall hotkey
  (`VK_PAUSE` in debug builds) and `FL_FAKE_HEARTBEAT=1`. Confirmed reports
  were written and 27/27 unit tests passed.

## 2026-05-17 02:34 - First real freeze

- File: `freeze_2026-05-17_023447_both.log`.
- Heartbeat both stalled. Main thread top frames showed
  `KERNELBASE!WaitForSingleObjectEx` -> `SkyrimSE+0x5765ff` ->
  `SkyrimSE+0x5b35dd` -> `hdtsmp64.dll+0x42dfe`.
- We did not yet have the Address Library loader in the plugin, so RVAs
  were resolved offline with `addrlib_lookup.py`. The result:
  - `+0x5765ff` is `id 34554 +0x2f`.
  - `+0x5b35dd` is inside `id 35565` (`RE::Main::Update`).
- WinDbg confirmed `RBX = 0x2ac8` and that handle was an
  unsignaled `NotificationEvent`.

This established **Site A**: an infinite wait on a notification-style event
inside `Main::Update`, accessed through a singleton at
`SkyrimSE+0x2f26680`. We named the singleton "Singleton-A".

## 2026-05-17 evening - Singleton-A field discovery

- Disassembled `id 34554` cluster in Ghidra plus the helper IDs nearby
  (`id 34547..id 34563`).
- Identified the singleton struct layout:
  - `[+0x58] HANDLE worker-wake` (auto-reset event, main -> worker)
  - `[+0x60] HANDLE worker-ack`  (manual-reset event, worker -> main)
  - `[+0x68] uint32 work-id`     (1 or 2)
  - `[+0x6c] uint32 pending`     (1 = wait scheduled and not drained)
  - `[+0x70..+0x72] flag bytes`  (set by sibling helper functions)
- Wrote `MainWaitProbe::Write()` to read those fields after stack-walking
  main. Added a "deadlock signature match" verdict: `pending=1` plus
  `worker-ack EventState=0` plus a four-byte explanation. Captured next
  freeze with this in place.

## 2026-05-17 18:43 - Confirmed Site A signature

- File: `freeze_2026-05-17_184345_both.log`.
- Probe output:
  ```
  pending=1, worker-ack EventState=0
  ===> DEADLOCK SIGNATURE MATCH <===
  ```
- This proved main is parked correctly: it set `pending=1`, scheduled the
  worker-wake auto-reset event, called `WaitForSingleObjectEx(worker-ack, INFINITE)`,
  but no worker ever signalled the ack. The "what worker did" remained open.

## 2026-05-18 13:16 - Site B surprise

- File: `freeze_2026-05-18_131625_both.log`.
- Main thread top frame was *different* this time:
  `+0x5b34fe` (still inside `id 35565` aka `Main::Update`) calling
  `+0xc38130` (a small wrapper). Previous Site A (`+0x5b35dd -> +0x5765ff`)
  was nowhere on the stack.
- Site A probe complained `interpretation: NOT pending` because main
  was not at Site A this time.
- Disassembled `+0xc38130`: 12-byte tail-jump that walks
  `(*(SkyrimSE+0x2f26a70))[+8][idx0]->vtable[idx1]` and tail-jumps to
  `KERNEL32!WaitForSingleObject`. We named the new singleton "Singleton-B".
- Confirmed via `enumerate_imports.py` that the IAT slot at
  `SkyrimSE+0x1509288` is indeed `KERNEL32!WaitForSingleObject`.

## 2026-05-18 evening - Refactor for two wait sites

- `MainWaitProbe::Write()` was substantially rewritten:
  - Detect main's wait site by checking RIP and scanning RSP for the
    return addresses of both Site A (`+0x5b35dd`) and Site B (`+0x5b34fe`).
  - Extract Site A logic into a conditional branch.
  - Add Site B branch: pointer-chain walk through `Singleton-B`, hex dump
    of the singleton instance, co-consumer search (other threads with the
    same RBX handle), toucher-thread search (threads referencing
    `Singleton-B` anywhere), wider stack dump.
- Extended `ThreadProbe` to capture every general-purpose register, not
  just RIP/RSP/RBX, in preparation for future correlation queries.

## 2026-05-19 - The "yes" round

- Captured `freeze_2026-05-19_110220_both.log`. Site A again.
- BSSpinLock probe was reporting `RDI = 0` for spinning workers.
  Re-disassembled `BSSpinLock::Acquire` (`id 12210`, `+0x132bd0`) and
  confirmed `KERNELBASE!SleepEx` clobbers RDI before `NtDelayExecution`.
  When we sample a thread that is already inside `SleepEx`, RDI no
  longer points at the lock.
- Replaced the RDI-only logic with a heuristic candidate detector:
  scan all 16 GP registers and a 1 KiB window of the thread's stack,
  look for any 8-byte qword that decodes as
  `{owner: plausible TID, state: 0..2}` with the address landing
  inside `SkyrimSE.exe`'s data region. Prints every match.
- Tightened the state check from `<= 4` to `<= 2` (BSSpinLock states are
  free=0, locked=1, contended=2; >= 3 is almost certainly unrelated data).

## 2026-05-19 11:36 - User asks for diagnostic completeness

The user expressed (correctly) that adding one feature per freeze is too
slow. The agreed-on batch was:

1. Address-Library symbolication of every frame at freeze time.
2. Per-thread `NtQueryEvent` annotation for every thread parked in
   `WaitForSingleObject*`.
3. Full non-volatile register dump per thread, with auto-correlation
   against known-interesting addresses.
4. A printed wait-graph that groups threads by handle and identifies
   "classic dispatch+wait" topologies.
5. Always-on minidump generation.

Implementation sketch (full details in `04-plugin-evolution.md`):

- New `src/AddrLib.{h,cpp}` parses `version-1-5-97-0.bin` once at plugin
  load; `Resolve(rva) -> {id, base_rva, delta}`. Only matches inside
  `SkyrimSE.exe`. Annotation suppressed when `delta` exceeds 16 KiB.
- `src/snapshot/Threads.cpp` now appends an `[id NNNN +0xN]` annotation
  per frame, dumps `RBX/RBP/RSI/RDI/R12-R15` on a one-line summary, and
  for any thread whose top frame matches the wait substrings, runs
  `NtQueryEvent(rbx)` and prints `{type, signaled}`.
- New `src/snapshot/WaitGraph.{h,cpp}` enumerates every parked thread,
  groups them by RBX handle, prints a per-handle table, and emits a
  one-line verdict if the main thread waits on a NotificationEvent that
  no other thread references.
- Default minidump enabled-by-default.
- All 27 unit tests still pass; `FreezeLogger_v0.1.0.rar` repackaged.

## 2026-05-19 12:04 - The diagnostic freeze

- File: `freeze_2026-05-19_120444_both.log`.
- Address Library loaded 778,674 entries. Every frame in the threads
  section is annotated with its ID.
- Main TID 13584 at Site A (`id 34554 +0x2f`), waiting on
  `HANDLE 0x29c0 [NotificationEvent (manual), NOT signaled]`.
- Wait-graph verdict:
  > main TID 13584 waiting on HANDLE 0x29c0 [NotificationEvent, NOT signaled]; 0 other waiters reference it.
  > >>> classic dispatch+wait deadlock: nobody is holding the producer side of main's handle.
- BSSpinLock heuristic detector found four spinning workers and printed
  exact lock-pair information:
  - TID 5096 sees LockB at `0x7ff61550b8e8` with `owner=18456 state=1`.
  - TID 18456 sees LockA at `0x7ff6154cf8e0` with `owner=5096 state=1`.
  - TIDs 13052 and 28176 are tail-blocked behind those two.
- This is the report that closes the case for what we *can see* in the
  game; everything past this point is static analysis to confirm the
  call-graph that produces the inversion.

## 2026-05-19 - Static analysis confirmation

- Wrote `analysis/xref_locks.py`. Found that LockA is privately referenced
  only by `id 19369` (six instructions, all inline acquire/release
  pattern), while LockB is referenced by four functions:
  `id 40285, id 40333, id 40334, id 40335`.
- Disassembled `id 19369`, `id 40333`, `id 36854`, `id 37388`, `id 40706`,
  `id 40289`. Found:
  - `id 19369` at `+0x38` calls `BSSpinLock::Acquire(LockA)` directly.
  - `id 40333` at `+0x2b` calls `BSSpinLock::Acquire(LockB)` directly.
  - `id 40706` at `+0x71` calls `BSSpinLock::Acquire(arg + 0x150)`.
    On TID 18456's invocation, `arg + 0x150 == LockB`.
- The call chains then form the AB-BA cycle:
  - **TID 5096:** `id 36854 -> id 19369 [LockA] -> id 17521 -> id 19372 -> id 40333 [spin LockB]`.
  - **TID 18456:** `id 40706 [LockB via arg+0x150] -> id 37388 -> id 36854 -> id 19369 [spin LockA]`.

The bug is now fully characterised: a Skyrim engine bug in the worker
dispatcher where two acquire orderings reach the same pair of locks.

The remaining work is the mitigation, covered in `08-mitigation.md`.
