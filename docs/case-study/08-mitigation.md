# 08 - Mitigation Options

The bug is in `SkyrimSE.exe` itself - we cannot recompile the engine.
Mitigation has to come from a runtime patch shipped as an SKSE plugin.

This file evaluates the realistic options.

## Option ranking

| Option                                              | Risk        | Effort  | Quality of fix                                                      |
|-----------------------------------------------------|-------------|---------|---------------------------------------------------------------------|
| **#1** Serialise `id 19369` and `id 40706`          | Low-Medium  | Low     | Eliminates AB-BA. Small perf cost in highly contended workloads.    |
| **#2** Lock-order arbiter in `BSSpinLock::Acquire`  | High        | High    | Generic; intercepts every spinlock acquire.                         |
| **#3** Hook only the inner acquires (LockB in P1, LockA in P2) | Medium | Medium | Surgical. Requires careful hook plumbing.                            |
| **#4** Trigger avoidance (mod-side throttle)        | Low (mod)   | High    | Brittle. Needs to identify the dispatching event source.            |
| **#5** Do nothing; rely on FreezeLogger             | None        | None    | Diagnostics only. Gives the user a `.dmp` to attach to a bug report.|

## Option #1 - Serialise `id 19369` and `id 40706` (recommended)

### Idea
Wrap both functions in a single global SRWLOCK or `std::mutex`
acquired at function entry and released at exit. While our mutex is
held, only one thread can be inside *either* function. The engine's
own BSSpinLocks become trivially uncontended within the section
because there is only ever one thread to acquire them.

### Why this works
The AB-BA cycle requires two threads. If the plugin-side mutex
forces strictly serial entry, there is no second thread to invert
the order with. Both `BSSpinLock::Acquire` calls succeed instantly
because the lock is always free for the single in-flight thread.

### Implementation sketch

The hook plumbing is a standard CommonLibSSE-NG `REL::Relocation` +
trampoline pattern:

```
namespace WorkerLockOrderFix {

    using id_19369_t = bool (*)(void* self, void* arg, int x, int y, void* extra);
    using id_40706_t = void (*)(void* self);

    REL::Relocation<id_19369_t> g_orig_19369{ REL::ID(19369) };
    REL::Relocation<id_40706_t> g_orig_40706{ REL::ID(40706) };

    // One global mutex for both functions. SRWLOCK is non-recursive
    // and lighter than std::mutex, but if id_19369 can re-enter
    // itself recursively (it can - see static analysis), we need a
    // thread-recursive lock. Easiest: std::recursive_mutex.
    std::recursive_mutex g_section;

    bool hooked_19369(void* a, void* b, int c, int d, void* e) {
        std::scoped_lock lock(g_section);
        return g_orig_19369(a, b, c, d, e);
    }

    void hooked_40706(void* self) {
        std::scoped_lock lock(g_section);
        g_orig_40706(self);
    }

    void Install() {
        SKSE::AllocTrampoline(64);
        auto& tr = SKSE::GetTrampoline();
        tr.write_call<5>(g_orig_19369.get(), reinterpret_cast<std::uintptr_t>(hooked_19369));
        tr.write_call<5>(g_orig_40706.get(), reinterpret_cast<std::uintptr_t>(hooked_40706));
    }
}
```

Real prototypes for the two function signatures need to be derived
from the disassembly - the snippet above is illustrative. The
`std::recursive_mutex` is required because:

- `id 19369` calls itself recursively at offset `+0x9d`. Without
  re-entrance support our mutex would self-deadlock the moment a
  thread recurses.
- `id 40706` is called from inside the worker dispatch chain that
  is also entered from `id 19369`'s descent (P1). If our mutex were
  not recursive, a P1 thread that already holds it could not enter
  `id 40706` later, defeating the purpose.

### Performance considerations

In normal operation the BSSpinLocks are cheap because contention is
rare. With this mutex in place:

- All entries to `id 19369` are serialised globally. Worst-case is
  the rate at which workers want to enter that function concurrently,
  multiplied by the average duration of a held section. From the
  freeze logs, the duration is dominated by deeper engine work
  (recursive `id 19369`, descent into `id 40333`, etc.) measured in
  microseconds at most.
- All entries to `id 40706` are serialised on the same mutex.

For Nolvus on a 32-CPU machine this is unlikely to be measurable.
For lower-end machines the impact is also small because the same
serialisation already happens organically inside the BSSpinLocks
when the cycle does not deadlock.

### Risks

- **Wrong function signature.** If we install hooks with the wrong
  prototype, the original function will be called with bogus
  registers and the game will crash. Confirm the prototype with a
  short logging hook before going live.
- **Recursive re-entrance still possible into a *third* function.**
  If `id 19369` calls some helper that takes yet another lock, and
  that helper is *also* called from within `id 40706`'s region, we
  may merely be hiding another inversion behind this one. Mitigation:
  monitor `FreezeLogger`'s wait-graph output once the patch is
  shipped. If a new pair appears, repeat the static analysis.
- **AB initialisation order.** The hooks must be installed before
  the worker pool starts dispatching. Standard SKSE plugin install
  point is `kPostLoad`, which runs before any in-game tick.

### Confidence
High. This is the same shape as several existing SKSE engine fixes,
and the topology in the freeze report is unambiguous.

## Option #2 - Lock-order arbiter in `BSSpinLock::Acquire`

### Idea
Hook `id 12210` (`BSSpinLock::Acquire`). Maintain a per-thread
"locks held" set. On entry, check whether the requesting lock would
form a cycle with any currently-held lock; if so, force one side
to back off (yield, retry, or abort).

### Why we are not recommending this
- `BSSpinLock::Acquire` is a hot path. Adding bookkeeping every
  acquisition has measurable cost across the entire engine.
- Reliably detecting a cycle without false positives requires a
  global lock-graph that updates atomically with every acquire and
  release. Threading bugs in *that* code would be far worse than the
  ones we are trying to fix.
- Backing off the wrong lock is just as bad as the original
  deadlock - it produces unpredictable behaviour rather than a
  predictable freeze.

This option exists for completeness; for production use, prefer #1.

## Option #3 - Hook only the inner acquires

### Idea
Instead of serialising the whole functions, only hook the
*second* acquisition in each path:

- In `id 40333` (LockB acquire on path P1): if any other thread is
  inside `id 19369` (i.e. holds LockA), spin-yield until either we
  no longer want LockB or LockA becomes free.
- In `id 19369` (LockA acquire on path P2): symmetric guard.

### Why we are not recommending this as the default
- Detecting "inside `id 19369`" requires a per-thread flag set on
  entry/exit, which is the same plumbing as Option #1 plus more.
- The win over Option #1 is that workers can run in *full
  parallelism* outside the deadlock-prone section, but the
  deadlock-prone section is already short, so the parallelism win
  is small.

If profiling under Option #1 shows actual contention this would be
the next refinement to try.

## Option #4 - Trigger avoidance

### Idea
Identify what game state schedules these two worker chains
concurrently and avoid that state. Possibilities (we have not
investigated):

- A specific cell-load pattern.
- A specific NPC/AI script burst.
- An interaction between two mods that both produce dispatch work
  at the same tick.

### Why we are not recommending this
- The dispatcher chain is generic. Many game systems funnel work
  through `id 67147 -> id 68058 -> id 68010 -> id 40289`. We do not
  yet know which subsystem is producing the conflicting work.
- Even if we identified one trigger, there is no reason to expect
  it is the *only* trigger. We would be playing whack-a-mole.

## Option #5 - Do nothing

### Idea
Ship `FreezeLogger` as the production diagnostic. When a freeze
happens, the user has a `.dmp`, a wait-graph verdict, and a clear
stack-trace pair to attach to a bug report. They can re-launch the
game and resume from the last save.

### Why this is not strictly worse
- The bug is rare on a per-session basis even if it is a known
  hazard for long-running play.
- A real fix has its own risks (Option #1 risks above). For users
  who would rather take the rare freeze than risk a destabilising
  hook, this is the safest path.

If a fix plugin is built (Option #1), users should still keep
`FreezeLogger` installed during the trial period to confirm the fix
is effective and to catch any new pair the fix accidentally
exposes.

## Recommended path

1. Ship `FreezeLogger` v0.1.0 as it is. The diagnostics are sound
   and the report format is now self-sufficient for any future
   freeze.
2. Build a small companion plugin `WorkerLockOrderFix` implementing
   Option #1. Keep it under 100 lines. Test by running a synthetic
   stress: heavy combat in a populated cell with both mods that
   produce dispatch work, for several hours. With Option #1 active,
   the AB-BA cycle should be impossible.
3. If a new freeze occurs with `WorkerLockOrderFix` active, the
   `FreezeLogger` report will tell us where the new lock pair is.
   Iterate the fix from there.
