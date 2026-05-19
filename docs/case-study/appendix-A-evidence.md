# Appendix A - Raw Evidence

This appendix collects the verbatim outputs that the case study's
narrative relies on. Each section is self-contained so it can be
quoted directly in a follow-up bug report or fix-plugin commit
message.

All data is from the freeze captured on **2026-05-19 12:04:44** in
`freeze_2026-05-19_120444_both.log` unless explicitly noted.

## A.1 Report header

```
================================================================
FreezeLogger v0.1.0
Captured:        2026-05-19 12:04:44 (local)
Runtime:         Skyrim SE 1.5.97
Stalled thread:  both
Main age:        15500 ms
Render age:      15485 ms
Threshold:       15000 ms
Symbol server:   <local cache only>
Address Library: loaded - 778674 entries from D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\Data\SKSE\Plugins\version-1-5-97-0.bin
================================================================
```

The Address Library successfully loaded, which is what makes the
per-frame `[id NNNN +0xN]` annotations possible.

## A.2 Main thread frames (TID 13584)

```
TID 13584 [main game thread]
  #00 0x00007ff82dfc1d24  ntdll.dll!NtWaitForSingleObject+0x14
  #01 0x00007ff82a97e40f  KERNELBASE.dll!WaitForSingleObjectEx+0xaf
  #02 0x00007ff612b465ff  SkyrimSE.exe+0x5765ff  [id 34554 +0x2f]
  #03 0x00007ff612b835dd  SkyrimSE.exe+0x5b35dd  [id 35565 +0x5ed]
  #04 0x00007fff4a632dfe  hdtsmp64.dll+0x42dfe
  #05..#08 (hdtSMP trampoline + return)
  nv-regs: RBX=0x00000000000029c0 RBP=0x0 RSI=0xffffffff RDI=0x0
           R12=0x0 R13=0x0 R14=0x0 R15=0x0
  waiting on: HANDLE=0x29c0 [NotificationEvent (manual), NOT signaled]
```

Site A signature, with the new probes:

- Frame `#02 [id 34554 +0x2f]` is the lock primitive's wait call.
- `RBX = 0x29c0` is the kernel handle main is waiting on.
- `NtQueryEvent(0x29c0)` reports `NotificationEvent (manual), NOT signaled`.
- The `nv-regs` line confirms no other useful pointer survives in
  main's non-volatile registers.

## A.3 Singleton-A field readback

From the MainWaitProbe section of the same report:

```
Singleton ptr global address:        0x00007ff6154f6668
Singleton ptr global value:          0x00007ff6154f6680

Site-A probe (Singleton-A @ SkyrimSE+0x2f26680):
  [+0x58] worker-wake handle:        0x00000000000029bc
  [+0x60] worker-ack  handle (echo): 0x00000000000029c0
  [+0x68] work-id:                   0x00000000 (0)
  [+0x6c] pending flag:              0x00000001 (1)
  [+0x70] flag2:                     0x00
  [+0x71] flag3:                     0x01
  [+0x72] flag4:                     0x01
  interpretation: pending - wait was scheduled - main is/was inside WaitForSingleObjectEx

Worker-wake event state ([+0x58] kernel event):
  Event type:                          1 (SynchronizationEvent (auto-reset))
  Event state:                         0 (NOT signaled)
```

Notes on what this proves:

- `worker-ack handle (echo) == main RBX` (`0x29c0`). Main is correctly
  parked in the dispatch protocol, not at some unrelated wait.
- `pending = 1` means main scheduled a job and has not seen
  completion. The producer side (a worker setting the ack-event) has
  not run yet.
- `worker-wake state = NOT signaled` (auto-reset). This was already
  consumed: a worker did pick up the wake. The worker is somewhere
  else in the process; we find it next.

## A.4 BSSpinLock candidate detector (the pivotal output)

From the same report:

```
BSSpinLock-owner search (threads spinning at SkyrimSE+0x132c5a):
  TID  5096:  [RSI=0x000000842a444000 owner=0 state=0]
              [stack=0x00007ff61550b8e8 owner=18456 state=1]
              [stack=0x00007ff615507ea0 owner=256 state=0]
              [stack=0x0000025c4b8ba838 owner=80 state=0]
              [stack=0x0000025e53acd080 owner=0 state=0]
              [stack=0x000000842f7ff318 owner=3 state=0]
              [stack=0x0000025e53b377a0 owner=50 state=0]
              [stack=0x0000025c00000000 owner=2 state=0]
              [stack=0x00007ff61448f850 owner=0 state=0]
              [stack=0x0000025e53b42e60 owner=0 state=0]
              [stack=0x0000025e00000000 owner=0 state=0]

  TID 18456:  [RSI=0x000000842a446000 owner=0 state=0]
              [stack=0x0000025c4b8c7d20 owner=0 state=0]
              [stack=0x00007ff6154cf8e0 owner=5096 state=1]
              [stack=0x0000025e00000000 owner=0 state=0]
              [stack=0x0000026000000000 owner=0 state=0]
              [stack=0x0000025f00000000 owner=0 state=0]
              [stack=0x000000842f8ff838 owner=0 state=0]
              [stack=0x00007ff615507ea0 owner=256 state=0]
              [stack=0x0000025f1a2a9d50 owner=0 state=0]

  TID 13052:  [RSI=...]
              [stack=0x00007ff61550b8e8 owner=18456 state=1]   <-- LockB
              ...

  TID 28176:  [RSI=...]
              [stack=0x00007ff6154cf8e0 owner=5096 state=1]    <-- LockA
              ...

  Total spinning threads: 4
```

Strong-signal lines (state=1, owner=valid live TID, address inside
SkyrimSE.exe):

| Address              | RVA            | Lock | Owner       |
|----------------------|---------------:|------|-------------|
| `0x00007ff61550b8e8` | `+0x2f3b8e8`   | LockB | TID 18456  |
| `0x00007ff6154cf8e0` | `+0x2eff8e0`   | LockA | TID 5096   |

Everything else is heuristic-detector noise: pointers that happen to
look like lock structures because their qword decodes within range.
The signal is unmistakable when the owner field equals an actual
spinner's TID.

## A.5 Wait graph verdict

```
## Wait graph
248 threads parked in WaitForSingleObject* (process 15996).

... per-handle table ...

  HANDLE 0x29c0 [NotificationEvent, NOT signaled] - 1 waiter(s):
    TID 13584 [MAIN]
  HANDLE 0x29f4 [SynchronizationEvent, NOT signaled] - 1 waiter(s):
    TID 16708

Summary:
  main TID 13584 waiting on HANDLE 0x29c0 [NotificationEvent, NOT signaled]; 0 other waiters reference it.
  >>> classic dispatch+wait deadlock: nobody is holding the producer side of main's handle.
```

The `0 other waiters reference it` count is computed across every
non-volatile register of every waiter. Combined with "NOT signaled",
this is the canonical "no producer in flight" pattern.

## A.6 Spinning workers' call stacks

### TID 5096 (holds LockA, spins on LockB)

```
TID 5096
  #00 ntdll.dll!NtDelayExecution+0x14
  #01 ntdll.dll!RtlDelayExecution+0x34
  #02 KERNELBASE.dll!SleepEx+0x91
  #03 SkyrimSE.exe+0x132c5a  [id 12210 +0x8a]    -- BSSpinLock spin
  #04 SkyrimSE.exe+0x6d9750  [id 40333 +0x30]    -- LockB acquire returns here
  #05 SkyrimSE.exe+0x297ddb  [id 19372 +0x60b]
  #06 SkyrimSE.exe+0x22d176  [id 17521 +0x626]
  #07 SkyrimSE.exe+0x2971a4  [id 19369 +0x5a4]   -- still inside (LockA held)
  #08 SkyrimSE.exe+0x6025fb  [id 36854 +0x1eb]   -- bridge frame
  #09 SkyrimSE.exe+0x61c780  [id 37388 +0xa0]
  #10 SkyrimSE.exe+0x6ef480  [id 40706 +0x250]
  #11 SkyrimSE.exe+0x6d468a  [id 40289 +0x6a]    -- worker dispatch
  #12 SkyrimSE.exe+0x5d842e  [id 36360 +0xfe]
  #13 SkyrimSE.exe+0x5d6b16  [id 36356 +0x2d6]
  #14 SkyrimSE.exe+0x6d4a31  [id 40292 +0x1b1]
  #15 SkyrimSE.exe+0x6d4b5a  [id 40293 +0x6a]
  #16 SkyrimSE.exe+0xc32a81  [id 68010 +0xd1]
  #17 SkyrimSE.exe+0xc34c48  [id 68058 +0x318]
  #18 SkyrimSE.exe+0xc0d6bd  [id 67147 +0x3d]    -- worker thread entry
  #19 KERNEL32.DLL!BaseThreadInitThunk+0x17
  #20 ntdll.dll!RtlUserThreadStart+0x2c
  nv-regs: RBX=0x000000842f7ff030 ...
```

### TID 18456 (holds LockB, spins on LockA)

```
TID 18456
  #00 ntdll.dll!NtDelayExecution+0x14
  #01 ntdll.dll!RtlDelayExecution+0x34
  #02 KERNELBASE.dll!SleepEx+0x91
  #03 SkyrimSE.exe+0x132c5a  [id 12210 +0x8a]    -- BSSpinLock spin
  #04 SkyrimSE.exe+0x296c3d  [id 19369 +0x3d]    -- LockA acquire returns here
  #05 SkyrimSE.exe+0x6025fb  [id 36854 +0x1eb]   -- bridge frame (same offset!)
  #06 SkyrimSE.exe+0x61c780  [id 37388 +0xa0]
  #07 SkyrimSE.exe+0x6ef480  [id 40706 +0x250]   -- still inside (LockB held)
  #08 SkyrimSE.exe+0x6d468a  [id 40289 +0x6a]    -- worker dispatch
  #09..#16 (same dispatch chain as TID 5096)
  #17 KERNEL32.DLL!BaseThreadInitThunk+0x17
  #18 ntdll.dll!RtlUserThreadStart+0x2c
  nv-regs: RBX=0x000000842f8ff5b0 ...
```

The two stacks share the same `id 36854 +0x1eb` bridge frame and the
same dispatch chain below it; they diverge only in *which* function
called `id 36854` (`id 19369` for TID 5096 vs. direct from
`id 37388` for TID 18456).

## A.7 `xref_locks.py` output

Run on `SkyrimSE.exe.unpacked.exe` (Steam stub stripped). Truncated
for legibility - full output in `analysis/xref_locks_output.txt`.

```
LockA  +0x2eff8e0 (RVA 0x2eff8e0):
    id19369         6 site(s)
      @ 0x296c2b  +0x0  lea rcx, [rip + 0x2c68cae]
      @ 0x297250  +0x0  cmp dword ptr [rip + 0x2c6868a], eax
      @ 0x297258  +0x4  cmp dword ptr [rip + 0x2c68685], 1
      @ 0x297263  +0x0  mov dword ptr [rip + 0x2c68677], ecx
      @ 0x297271  +0x4  lock cmpxchg dword ptr [rip + 0x2c6866b], ecx
      @ 0x29727b  +0x4  lock dec dword ptr [rip + 0x2c68662]

LockB  +0x2f3b8e8 (RVA 0x2f3b8e8):
    id40285         6 site(s) (lea, cmp/cmp/mov, cmpxchg, dec - inline acquire+release)
    id40333         6 site(s)
    id40334         6 site(s)
    id40335         6 site(s)
```

The asymmetry (LockA private to one function, LockB shared by four)
is the structural fingerprint of the bug.

## A.8 Disassembly excerpts

### `id 19369` prologue

```
0x00296c00  push rbp ; push rsi ; push rdi
0x00296c12  sub  rsp, 0xc0
0x00296c25  mov  [rax+0x10], rbx
0x00296c2b  lea  rcx, [rip + 0x2c68cae]   ; rcx = LockA
0x00296c32  mov  [rbp+0x1f], rcx          ; stash for RAII
0x00296c36  xor  edx, edx
0x00296c38  call 0x140132bd0              ; BSSpinLock::Acquire(LockA)
0x00296c3d  nop                           ; <-- TID 18456 here
```

### `id 40333` prologue

```
0x006d9720  push rbx ; push rbp ; push rsi ; push rdi ; push r14
0x006d9727  sub  rsp, 0x40
0x006d9737  mov  rdi, rcx
0x006d973a  lea  rcx, [rip + 0x28621a7]   ; rcx = LockB
0x006d9741  mov  [rsp+0x88], rcx          ; stash
0x006d9749  xor  edx, edx
0x006d974b  call 0x140132bd0              ; BSSpinLock::Acquire(LockB)
0x006d9750  nop                           ; <-- TID 5096 here
```

### `id 40706` first-acquire site

```
0x006ef230  push rbp ; push rsi ; push rdi ; push r12-r15
0x006ef241  sub  rsp, 0x110
0x006ef259  mov  r14, rcx                 ; r14 = first arg
0x006ef291  lea  rbx, [r14 + 0x150]       ; rbx = arg + 0x150 (= LockB on TID 18456)
0x006ef298  mov  [rbp+0x58], rbx          ; stash
0x006ef29c  xor  edx, edx
0x006ef29e  mov  rcx, rbx
0x006ef2a1  call 0x140132bd0              ; BSSpinLock::Acquire([arg+0x150])
0x006ef2a6  nop                           ; LockB now held by this caller
... (calls down through id 37388 -> id 36854 -> id 19369) ...
0x006ef480  ...                           ; <-- TID 18456 here
```

### `id 36854` call to `id 19369`

```
0x006025dd  c644242800                    mov   byte [rsp+0x28], 0
0x006025e2  c744242001000000              mov   dword [rsp+0x20], 1
0x006025ea  4533c9                        xor   r9d, r9d
0x006025ed  4533c0                        xor   r8d, r8d
0x006025f0  488bd6                        mov   rdx, rsi
0x006025f3  488bcb                        mov   rcx, rbx
0x006025f6  e80546c9ff                    call  0x140296c00       ; <-- call id 19369
0x006025fb  4533f6                        xor   r14d, r14d        ; <-- return target
```

The same call instruction is reached on both threads' stacks (with
different argument bindings), confirming `id 36854` is the bridge
function.

## A.9 Cross-reference: every freeze report so far

For completeness:

| File                                       | Stalled | Notes                                                           |
|--------------------------------------------|---------|-----------------------------------------------------------------|
| `freeze_2026-05-17_023447_both.log`        | both    | First freeze. Site A discovered manually via WinDbg.            |
| `freeze_2026-05-17_140627_main.log`        | main    | Site A signature confirmed.                                      |
| `freeze_2026-05-17_160831_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-17_181358_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-17_184345_both.log`        | both    | Site A. First report with `MainWaitProbe` field readback.        |
| `freeze_2026-05-17_202040_both.log`        | both    | Site A.                                                          |
| `freeze_2026-05-17_211200_both.log`        | both    | Site A. First BSSpinLock probe.                                  |
| `freeze_2026-05-17_214902_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-18_104114_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-18_112604_both.log`        | both    | Site A.                                                          |
| `freeze_2026-05-18_131306_main.log`        | main    | Site A.                                                          |
| **`freeze_2026-05-18_131625_both.log`**    | both    | **Site B discovered.**                                           |
| `freeze_2026-05-18_150412_both.log`        | both    | Site B confirmed.                                                |
| `freeze_2026-05-18_165613_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-18_210047_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-19_084306_main.log`        | main    | Site A.                                                          |
| `freeze_2026-05-19_110220_both.log`        | both    | Site A. RDI=0 in BSSpinLock probe; motivated heuristic detector. |
| **`freeze_2026-05-19_120444_both.log`**    | both    | **Diagnostic round complete. AB-BA inversion confirmed.**       |

The vast majority are Site A; Site B is rare (two reports out of
~18). This is consistent with Site B being a different code path
that just happens to also live inside `Main::Update`. As of this
writing the AB-BA fix would address all Site A freezes; Site B
would need its own investigation if/when it recurs.
