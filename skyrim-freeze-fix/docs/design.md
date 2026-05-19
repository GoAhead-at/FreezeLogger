# WorkerSpinLockFix - design

## 1. Bug recap

Skyrim SE 1.5.97 contains an AB-BA inversion between two static
`BSSpinLock` globals in the worker dispatcher:

- `LockA` at `SkyrimSE+0x2eff8e0`, taken inside `id 19369`.
- `LockB` at `SkyrimSE+0x2f3b8e8`, taken inside `id 40706`
  (via `[arg + 0x150]`) and inside `id 40333`.

Two worker threads can hold one lock and wait on the other. Once
both threads are caught in the cycle, neither makes progress, the
worker-ack event the main thread is waiting on is never signalled,
and the game freezes hard. Full evidence:
`../../docs/case-study/06-root-cause.md`.

## 2. Mitigation strategy

We cannot easily replace or instrument the engine's `BSSpinLock`
acquisitions themselves. Instead, we serialise *entry to the two
functions that take them at the top of the deadlock chain*.

### 2a. v0.1 - WHY IT FAILED (retracted)

v0.1 used two separate mutexes:

- `mtx_19369`: at most one thread at a time inside `id 19369`.
- `mtx_40706`: at most one thread at a time inside `id 40706`.

The argument was that the engine's call graph forces canonical
order between them (path P2 always takes `mtx_40706` before
`mtx_19369`; path P1 only takes `mtx_19369`).

The flaw was ignoring the engine's own LockA/LockB acquisitions,
which interleave with the plugin mutexes. Combining all locks:

| Path | acquired (in order) |
|------|---------------------|
| P1   | `mtx_19369` -> LockA -> ... -> LockB |
| P2   | `mtx_40706` -> LockB -> ... -> `mtx_19369` -> LockA |

Between `mtx_19369` and LockB:
- P1 takes `mtx_19369` BEFORE LockB.
- P2 takes LockB BEFORE `mtx_19369`.

That is a brand-new AB-BA cycle. v0.1 reproduced the freeze
on its first deployed session. From the freeze log:

- TID 29980: holds `mtx_19369` + LockA, spinning on LockB.
- TID 25560: holds `mtx_40706` + LockB, blocked on `mtx_19369`.

### 2b. v0.2 - one mutex for both

v0.2 replaces the per-function mutexes with a **single shared
mutex** `g_section_mutex`. Both `Hook_id19369` and `Hook_id40706`
acquire it. Same-thread recursion is allowed via TID-tracked
depth counter.

At most one thread can be inside `id 19369` OR `id 40706` at any
time. That single thread acquires LockA and LockB sequentially
within its critical section, so LockA and LockB cannot invert
against each other (a single thread by itself cannot AB-BA).
Other threads queue at the plugin mutex BEFORE acquiring any
engine lock, so no thread can hold an engine lock while another
holds the plugin mutex.

Concretely, the would-be AB-BA scenario with v0.2 installed:

```
Thread A (path P1): -> g_section (acquired) -> [LockA] -> id 40333 -> [LockB] -> done -> release
Thread B (path P2): -> g_section (BLOCKED at hook)            <-- never reaches LockB
```

When A releases the section, B proceeds, acquires LockB inside
`id 40706`, descends into `id 19369` (recursive entry on same
TID, just bumps depth), acquires LockA, etc. - no contender on
either engine lock because B holds the plugin section.

### Engine call graph (verified)

Verified by `analysis/disasm_targets.py`, `analysis/xref_locks.py`,
and `analysis/xref_calls.py`:

- Path P1: `id 36854` -> `id 19369` (LockA) -> `id 17521` -> `id 19372` -> `id 40333` (LockB).
- Path P2: `id 40706` (LockB via `[arg+0x150]`) -> `id 37388` -> `id 36854` -> `id 19369` (LockA).

## 3. Hook surface

CommonLibSSE-NG's `Trampoline::write_branch<5>` and
`write_call<5>` only patch *existing* 5-byte E8/E9 instructions.
They cannot patch a function entry-point because they read the
existing rel32 displacement to recover the original target.

So we hook every direct CALL site instead:

| Function   | Direct CALL sites | Source                  |
|------------|-------------------|-------------------------|
| `id 19369` | 50 (incl. recursive `id 19369 +0x9d`) | `analysis/xref_calls.py` |
| `id 40706` | 4                                     | `analysis/xref_calls.py` |

Indirect calls (vtable, register-relative) are not covered. The
xref scan turned up zero indirect calls for either function in
the analysed binary.

## 4. Re-entrance

`id 19369` self-recurses (call site `id 19369 +0x9d`). A plain
`std::mutex` would self-deadlock on that recursion if we hooked
the recursive site too. We solve it with manual TID-tracked
re-entrance (see `Hooks.cpp::SectionGuard`):

- On entry, compare current TID against `owner_tid`.
- If equal -> bump depth, take no lock, return.
- Otherwise -> `mtx.lock()`, set `owner_tid`, set `depth = 1`.
- On exit -> if outer (we set owner_tid) -> reset and unlock;
  otherwise just decrement depth.

This is essentially a fast-path recursive mutex. Same correctness
guarantees as `std::recursive_mutex` for our use case, lower
overhead on the hot recursive path.

## 5. Hook prototype

We do not have a fully confirmed C++ prototype for either
function. The disassembly suggests integer-only register/stack
arguments. We declare a wide opaque prototype:

```cpp
using FuncWide_t = std::uintptr_t (*)(
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
```

This preserves `rcx`/`rdx`/`r8`/`r9` (register-class args 1..4)
and the first four stack slots ([rsp+0x20..0x38] - args 5..8)
through the standard Microsoft x64 calling convention.

**Residual risk**: if the engine actually passes float/double
arguments in `xmm0..xmm3` to these functions, our integer-only
declaration loses them and the original function gets garbage.
We have not seen evidence of XMM arguments for either function,
but we have not exhaustively proven their absence.

## 6. Lifecycle

- `SKSEPlugin_Load` (called once at game startup):
  1. Init logger.
  2. Verify runtime is exactly SE 1.5.97; if not, do nothing
     and stay loaded but inert.
  3. Read `WorkerSpinLockFix.toml`.
  4. If `plugin.enabled = false`, stay inert.
  5. `SKSE::AllocTrampoline(64)` (write_call<5> does not
     actually consume trampoline bytes, but SKSE requires
     this call before any patch).
  6. Patch all 50 + 4 call sites.
  7. Start the periodic stats dump thread.

- Periodic stats dump (every `log.stats_interval_s` seconds):
  - One info-level line per hooked function, listing total
    entries, total contentions, total long-waits, average
    wait, and max wait.

- Per-thread contention warning (synchronous, on each contended
  acquire): info-level line with TID and the wait duration in
  ms, only when the wait was >= `log.contention_warn_ms`.

## 7. What success looks like

Run the build with `FreezeLogger` active. Play normally. After
each session:

1. `FreezeLogger.log` should show no `worker-ack` Site A or
   Site B INFINITE waits.
2. `WorkerSpinLockFix.log` should show:
   - `Hooks installed: id 19369 -> 50/50 call sites,
     id 40706 -> 4/4 call sites.`
   - Periodic `stats[id19369]` and `stats[id40706]` lines.
   - `contentions=0` would mean we've never contended, which
     might mean the parallelism cost is zero. Higher numbers
     are fine; what matters is that no contended wait grows
     unbounded.

If a freeze still happens, FreezeLogger's wait-graph snapshot
will tell us whether it's the same AB-BA (in which case the
plugin failed to install on the responsible call site) or a
different bug.
