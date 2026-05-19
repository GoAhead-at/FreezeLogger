# 05 - Static Analysis

After `FreezeLogger` told us *which threads* were stuck and *on which
locks*, the remaining question was structural: in what order does each
thread acquire those locks, and where does the inversion happen?

This file documents the static-analysis path from "we have lock RVAs"
to "we have call-graph confirmation".

## 5.1 Inputs from the live run

The 12:04 freeze report gave us:

- `LockA` lives at `SkyrimSE+0x2eff8e0` and is currently held by TID 5096.
- `LockB` lives at `SkyrimSE+0x2f3b8e8` and is currently held by TID 18456.
- TID 5096's BSSpinLock-spin frame returns to `id 40333 +0x30`.
- TID 18456's BSSpinLock-spin frame returns to `id 19369 +0x3d`.
- Both threads' deeper stacks pass through `id 36854 +0x1eb`, but at
  different *depths* in the recursion.

## 5.2 Toolchain

Two pieces did the work:

- The unpacked `SkyrimSE.exe.unpacked.exe` (Steam stub stripped, full
  `.text` available).
- `analysis/xref_locks.py`, a focused fork of `xref_scan.py` that scans
  every RIP-relative reference in `.text` for targets in
  `[LockA, LockA+8)` or `[LockB, LockB+8)`. Implementation detail: the
  capstone disassembler is run with `skipdata=True` so jump tables
  inside `.text` do not abort the linear scan.

The linear scan walks ~6 million instructions (about 22 MiB of `.text`)
in roughly 30 seconds and prints every match grouped by Address Library
ID.

## 5.3 What `xref_locks.py` reported

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
    id40285         6 site(s)
    id40333         6 site(s)
    id40334         6 site(s)
    id40335         6 site(s)
```

The asymmetry is striking and important:

- **LockA is private to `id 19369`.** Six references, all inside one
  function. The pattern (lea + cmp/cmpxchg/dec on owner+state) is the
  textbook inline-acquire/release that typical Skyrim code uses around
  a `BSSpinLock::Acquire` slow-path call.
- **LockB is shared across four functions.** `id 40285`, `id 40333`,
  `id 40334`, `id 40335` are sequential IDs, suggesting a class with
  several methods that all guard the same shared resource.

This means LockA and LockB are different in kind: LockA protects state
private to whatever class hosts `id 19369`; LockB protects something
referenced by multiple methods on a different class. A deadlock between
two such locks is only possible if a code path from class-A reaches
class-B *while still holding* class-A's lock, and another code path
from class-B reaches class-A while still holding class-B's lock.

## 5.4 Disassembly: how each function takes its lock

### `id 19369` (LockA acquirer)

```
0x00296c00  push rbp ; push rsi ; push rdi ; sub rsp, 0xc0     ; standard prologue
0x00296c25  mov [rax+0x10], rbx                                ; save rbx (caller's responsibility)
0x00296c28  mov rsi, rcx                                       ; rsi = arg1 (this)
0x00296c2b  lea rcx, [rip + 0x2c68cae]                         ; rcx = &LockA  (= SkyrimSE+0x2eff8e0)
0x00296c32  mov [rbp+0x1f], rcx                                ; stash for stack-based RAII destructor
0x00296c36  xor edx, edx                                       ; arg2 = 0
0x00296c38  call 0x140132bd0                                   ; <<< call BSSpinLock::Acquire(LockA)
0x00296c3d  nop                                                ; return target = id 19369 +0x3d
```

Confirms: `id 19369`'s very first non-prologue action is to acquire
LockA. Then it does whatever its body does and eventually releases
LockA via the inline `cmpxchg/dec` pattern at offsets 0x650+.

`id 19369` is also **recursive**: at offset `+0x9d` (`0x296c9d`) it
makes a `call 0x140296c00` (itself), which means a single invocation
can be deep on the stack while still holding LockA via the *outer*
frame. This is consistent with `BSSpinLock::Acquire` being recursive
or holding a re-entry counter, but is not relevant to the AB-BA bug
beyond explaining why TID 5096 is at offset `+0x5a4` (deep inside the
recursion) while TID 18456 is at offset `+0x3d` (shallow).

### `id 40333` (LockB direct-lea acquirer)

```
0x006d9720  push rbx ; push rbp ; push rsi ; push rdi ; push r14 ; sub rsp, 0x40
0x006d9737  mov rdi, rcx                                       ; save arg1
0x006d973a  lea rcx, [rip + 0x28621a7]                         ; rcx = &LockB  (= SkyrimSE+0x2f3b8e8)
0x006d9741  mov [rsp+0x88], rcx                                ; stash for stack RAII
0x006d9749  xor edx, edx                                       ; arg2 = 0
0x006d974b  call 0x140132bd0                                   ; <<< call BSSpinLock::Acquire(LockB)
0x006d9750  nop                                                ; return target = id 40333 +0x30
```

Same structure as `id 19369`: lea-stash-call. The freeze report's
`+0x30` return offset for TID 5096 is exactly the instruction after
this call, which proves TID 5096 is still spinning inside
`BSSpinLock::Acquire` for LockB.

The body of `id 40333` then runs to the `cmpxchg` at offset 0x143 and
the `lock dec` at offset 0x14d - the inline release - and returns at
offset 0x15e. So the LockB-protected region of `id 40333` is roughly
0x130 bytes.

### `id 40706` (LockB indirect-lea acquirer)

```
0x006ef230  push rbp ; push rsi ; push rdi ; push r12..r15 ; sub rsp, 0x110
0x006ef259  mov r14, rcx                                       ; save arg1 ("this")
0x006ef291  lea rbx, [r14 + 0x150]                             ; rbx = (this + 0x150) = some lock
0x006ef298  mov [rbp+0x58], rbx                                ; stash for stack RAII
0x006ef29c  xor edx, edx
0x006ef29e  mov rcx, rbx
0x006ef2a1  call 0x140132bd0                                   ; <<< call BSSpinLock::Acquire([this+0x150])
0x006ef2a6  nop                                                ; return target = id 40706 +0x76
```

This is the trickier acquisition. The lock argument is an offset off
the function's first parameter, not a global address. The
`xref_locks.py` scan **does not** find this site because the addressing
mode is `[reg + disp]`, not `[rip + disp]`.

So why are we sure `[r14 + 0x150]` resolves to LockB at runtime? Three
pieces of evidence:

1. The 12:04 freeze report's heuristic candidate detector reads the
   live memory at `0x7ff61550b8e8` (= `SkyrimSE+0x2f3b8e8` = LockB)
   and reports `{owner: 18456, state: 1}`. That is a direct read of
   LockB's two 32-bit fields; LockB is held with TID 18456 as owner.
2. TID 18456's deepest non-LockA-related frame is `id 40706 +0x250`,
   which is well past the `+0x71` acquire site but before the second
   acquire at `+0x436` (a *different* lock - see below). So if any
   lock is held by TID 18456, it was acquired at `+0x71`.
3. There is no other lock-acquire on TID 18456's stack between
   `id 40289` and `id 40706 +0x250`. The four `xref_locks.py` direct-
   lea LockB sites all live in functions that are not on TID 18456's
   stack.

By elimination, the lock at `[r14 + 0x150]` *is* LockB on this
particular call. Either `r14` is the same global object whose direct
lea appears in `id 40333` (in which case `id 40333` and `id 40706`
share their locked object), or it is a different object whose lock
field happens to land at the same kernel address. The first
interpretation is overwhelmingly likely and is what the call-graph
confirms.

Note that `id 40706` also takes a *second* lock at offset `+0x436`:

```
0x006ef655  lea rbx, [rcx + 0x150]                             ; same offset, different rcx path
0x006ef666  call 0x140132bd0                                   ; <<< second BSSpinLock::Acquire
```

This is not the bug; it is on a code path past TID 18456's current
position. Worth noting only because future analysis of `id 40706`
might confuse the two acquires.

### `id 36854` (the bridge)

```
0x00602410  push rbp ; push rsi ; push rdi ; push r14 ; push r15 ; sub rsp, 0x50
0x0060242d  mov rdi, rdx                                       ; save arg2
0x00602430  mov rsi, rcx                                       ; save arg1
... lots of work that does NOT take any lock ...
0x006025f6  call 0x140296c00                                   ; CALL id 19369  (LockA acquirer)
0x006025fb  ...                                                ; return target = id 36854 +0x1eb
```

`id 36854` does not itself hold any lock. It is a "bridge" function:
it is the immediate caller of `id 19369` on both threads' stacks,
which is why the same `+0x1eb` return offset appears in both freeze-
log entries. Whatever lock `id 36854` is operating *under* came from a
caller (`id 37388 -> id 40706` for TID 18456) or the function being
called (`id 19369 -> ...` for both threads).

## 5.5 Reconstructing the call topology

Combining the frames from both threads:

```
                                    +--- (worker pool dispatch root: id 67147 -> id 68058 -> id 68010 -> id 40289)
                                    |
                                    v
                       +-----------------------------+
                       | id 40706                   |
                       |  +0x71  ACQUIRE LockB       | <-- TID 18456 holds here
                       |  ...                        |
                       |  +0x250 call id 37388       |
                       |     -> id 36854             |
                       |        -> id 19369          |
                       |           +0x38 ACQUIRE A   | <-- TID 18456 SPINS here
                       +-----------------------------+

                       +-----------------------------+
                       | id 36854                    |
                       |  +0x1e6 call id 19369       |
                       |     +0x38 ACQUIRE LockA     | <-- TID 5096 holds here
                       |     +0x59f call id 17521    |
                       |        -> id 19372          |
                       |           -> id 40333       |
                       |              +0x2b ACQUIRE B| <-- TID 5096 SPINS here
                       +-----------------------------+
```

The two diagrams collapse into one cycle if you draw the dependency
edges:

```
TID 5096   [holds LockA]  ----wants---->  [LockB]
                                              ^
                                              |
                                              +---- [holds LockB]   TID 18456
                                                                       |
                                                                       v
                                                                    [LockA]
                                                                       ^
                                                                       |
                                                                  ----wants----  TID 5096
```

That is the textbook AB-BA: each thread holds one lock and is
indefinitely waiting on the other. Neither thread can make progress.

## 5.6 Why this isn't visible in `xref_locks.py` directly

`xref_locks.py` only finds acquire sites that use a RIP-relative
addressing mode. `id 40706` uses `[reg + disp]` for its acquire, so
the lock-pair connection requires:

1. The runtime evidence that `[r14 + 0x150]` resolves to LockB on at
   least one observed invocation (heuristic detector output).
2. Static evidence that `id 40706` is on TID 18456's stack at offset
   `+0x250`, between its two acquire sites.

A more thorough static scan would also walk `[reg + disp]` references
back to the `lea reg, [rip + disp_global]` that initialises `reg`
inside the same function. We did not need that here because the
runtime-side heuristic detector already nailed the answer; for future
investigations, that "follow indirect bases" enhancement would close
this loophole.

## 5.7 Confidence summary

| Claim                                                                                                   | Confidence | Source                                                                |
|----------------------------------------------------------------------------------------------------------|------------|------------------------------------------------------------------------|
| `id 19369` acquires LockA at offset `+0x38`.                                                            | Certain    | Disassembly (lea+call sequence).                                       |
| `id 40333` acquires LockB at offset `+0x2b`.                                                            | Certain    | Disassembly (lea+call sequence).                                       |
| `id 40706` acquires a `[arg+0x150]` lock at offset `+0x71`.                                             | Certain    | Disassembly.                                                           |
| `[arg+0x150]` resolves to LockB on TID 18456's invocation.                                              | High       | Heuristic detector + elimination over TID 18456's stack frames.        |
| Call graph: `id 36854 -> id 19369 -> id 17521 -> id 19372 -> id 40333` for TID 5096.                    | Certain    | Stack frames in 12:04 report match the disassembled call instructions. |
| Call graph: `id 40289 -> id 40706 -> id 37388 -> id 36854 -> id 19369` for TID 18456.                   | Certain    | Same.                                                                  |
| The cycle is the actual cause of *all* observed freezes (not just the 12:04 one).                       | Medium     | Site A signature matches across most freezes; whether *every* Site-A freeze is the same lock pair has not been re-confirmed against historical reports. Likely yes. |
