# 09 - Lessons Learned

Generalisable takeaways from the FreezeLogger investigation. Listed
roughly in order of how often they came up.

## On building in-process post-mortem tooling

### 1. Always have an SEH-safe primitive reader.

A diagnostics plugin reads memory whose validity is dictated by code
you do not own. Any dereference can fault. The lesson is structural:

- Wrap every memory read in `__try / __except` and return a status
  (`bool ok`, `DWORD seh_code`).
- Functions that contain `__try` blocks under `/EHsc` cannot also
  contain non-trivial C++ objects (string formatting, `std::format`,
  `std::vector` construction). Refactor those into an outer caller.
- The two-layer pattern: `noexcept` SEH primitives at the bottom,
  formatting/composition above.

### 2. Suspended-thread sampling needs an RAII guard.

`SuspendThread` without a paired `ResumeThread` permanently freezes
the user's game. We learned this once when an early version threw a
DbgHelp exception out of the walk and skipped the resume; recovery
required killing the process.

The pattern is:

```cpp
class SuspendGuard {
    HANDLE _t;
    bool   _suspended;
public:
    explicit SuspendGuard(HANDLE t) : _t(t), _suspended(::SuspendThread(t) != -1) {}
    ~SuspendGuard() { if (_suspended) ::ResumeThread(_t); }
};
```

Even a `std::bad_alloc` thrown three frames up is fine because the
destructor runs on the unwind.

### 3. DbgHelp is single-threaded and slow on first call.

DbgHelp (`SymInitialize`, `SymFromAddr`, `StackWalk64`) is documented
as not thread-safe; serialise with one mutex held for the whole
walk. The first symbol resolution per module is also slow because
it can trigger PDB downloads from a symbol server. Default
`use_ms_symbol_server = false`; downloads happen offline if needed.

### 4. `NtQueryEvent` is the right primitive for inspecting events.

`WaitForSingleObject(h, 0)` looks innocuous but consumes the signal
on auto-reset events. `NtQueryEvent` returns `{type, signaled}`
without modifying state. Worth the `GetProcAddress` lookup.

### 5. Heuristic data-structure recovery beats register-precise extraction.

We initially tried to recover lock pointers from a single register
(RDI, the convention in `BSSpinLock::Acquire`). Worked until
`KERNELBASE!SleepEx` clobbered RDI before the syscall, and then it
failed silently with `RDI=0`.

The rewrite: scan all 16 GP registers and a 1 KiB stack window for
qwords that *look like* the data structure we want. It is uglier
but vastly more robust, because anything stack-allocated or saved
to a non-volatile register survives `SleepEx`.

The pattern generalises: when extracting a struct from a suspended
thread,

- enumerate all live registers AND a window of the stack;
- apply a structural validator (`LooksLikeXxx`) to each candidate;
- print every match, let the human triage false positives;
- tighten the validator iteratively when noise becomes a problem.

### 6. A "wait graph" cross-cut compresses thousand-thread reports.

A 250-thread snapshot is unreadable as a flat list. Group threads by
the kernel handle they are waiting on, then per-group print the
event state and the count of waiters; an O(threads) verdict line at
the end identifies the canonical "main waits, no one signals"
pattern in one sentence. We went from "where do I even look?" to
"the verdict is right there" with about 200 lines of code.

### 7. Always-on minidump is cheap insurance.

The text report is the first read; the minidump is the offline
replay. Once retention is bounded (default `retain_last_n = 5`),
disk cost is negligible for the diagnostic value. The historical
default of "off-by-default" cost us several iterations because we
could not replay without re-triggering. Flip on.

## On reverse-engineering Skyrim engine bugs

### 8. The Address Library is the only stable identifier.

RVAs change between (theoretically) any patch. IDs are stable
because the meh321 toolchain matches by structure across all
1.5.97 builds. Resolve frames to IDs at freeze time so the report
is the same shape regardless of which Steam build the user has.

### 9. Disassembling 320 instructions of a target function is enough
for ~70% of questions.

The `analysis/disasm_targets.py` script defaults to a 0x800-byte
chunk and 320-instruction cap. That is enough to see the prologue,
the first lock acquisition, and the first call out. When that is
not enough, just bump the cap; do not over-engineer the script.

### 10. RIP-relative xrefs find direct uses but miss indirect ones.

`xref_locks.py` finds every instruction with `[rip + disp]` whose
target is the lock. It will *not* find acquires of the form
`lea rcx, [rsi + 0x150]` where `rsi` was loaded earlier from a
RIP-relative move.

For lock-pair analysis the gap is bridged by the runtime evidence
(the heuristic candidate detector reads the lock's actual bytes,
which is unambiguous). For static-only investigations, a future
enhancement would walk back from `[reg + disp]` references to the
register's RIP-relative origin.

### 11. Skyrim's BSSpinLock is recursive on the holding thread.

The release pattern in every BSSpinLock release we have read
checks `dword [LockA] == EAX (current TID)` before releasing. Same-
thread re-entrance is handled. This means a recursive function
(like `id 19369`) that holds a BSSpinLock can recurse without
self-deadlocking - useful both for understanding and for designing
mitigations.

### 12. Sequential Address Library IDs frequently mean "method cluster
on the same class".

`id 40285, 40333, 40334, 40335` are sequential and all touch
LockB. Highly likely they are virtual or static methods of the
same class, all guarded by the same lock. The cluster pattern is a
strong hint when grouping xref output.

## On project methodology

### 13. "One feature per freeze" is too slow.

The investigation iterated 4-5 times across 5 days. Each iteration
shipped one diagnostic improvement, captured one freeze with it,
and stopped. The cost was real wall-clock days waiting for the
next freeze.

The fix that finally cut the cycle was to ship five improvements
*at once*, designed before the next freeze, on the assumption
that several of them would all be needed simultaneously. They
were. The next freeze was diagnosed end-to-end without further
plugin changes.

The lesson: when iteration time is dominated by waiting on a
non-deterministic event, batch your diagnostic improvements. If
some of them are unused on the next event, that is fine; their
cost is in lines of code, not in time.

### 14. Bound everything that retains data.

Reports retained by count (50). Minidumps retained by count (5).
Symbol cache retained by manual cleanup. Each bound was set early
and we have not had to revisit them. A diagnostic that "just keeps
writing" is one OS-disk-full away from making the user's problem
worse.

### 15. Catch2 unit tests for the watchdog state machine were a
disproportionate win.

Every behavioural change to the watchdog (cooldown handling,
classifier, annotate-on-resolve semantics) was driven by a unit
test. Refactoring the classifier from `bool` to `enum class` plus
the suppression flag would have been terrifying without the 27
green tests. The tests are pure logic - no Skyrim required - so
they run in milliseconds.

The diagnostic code paths (`MainWaitProbe`, `WaitGraph`) are *not*
unit-tested because they require a live game; this is a known gap.
SEH guards and exception safety mitigate but do not eliminate
the risk.

## On debugging in general

### 16. Read the source of every loaded mod before blaming it.

HDT-SMP appeared on a critical stack frame and was suspect for
about an hour. Reading `hdtSMP64/src/Hooks.cpp` showed that
`MainHooks::Update` is a wrap-around hook (`call _Update; dispatch
FrameEvent`) that takes no locks before the original engine call.
The HDT-SMP frame visible on main's stack at freeze time is just
the saved return address into the wrapper's post-`_Update`
portion, which has not yet executed because `_Update` is still
blocked. Cheaper to read the hook source than to run another two
freeze cycles guessing.

### 17. "Process is running" is not the same as "thread is running".

Windows reports Skyrim as healthy throughout these freezes because
the main thread is sleeping in `WaitForSingleObject`. Process-level
liveness checks are the wrong layer; per-thread heartbeat counters
are the right layer.

### 18. The producer side of a wait is rarely on the same thread as
the wait.

Main waits for an ack-event. The producer is "whichever worker
will SetEvent on it". That worker can be anywhere among hundreds
of threads. Snapshot scope must include them all (`max_threads =
1024`), and any wait-graph or wait-handle search must enumerate
non-game threads as well as game ones.

### 19. Live memory reads beat reasoning about reachability.

The cleanest evidence in the entire investigation is the heuristic
detector's `{owner: 18456, state: 1}` readout. That single line
proved LockB was held by TID 18456 at the moment of the snapshot,
without any indirection through stack reasoning or call-graph
guessing. When in doubt, read the bytes.
