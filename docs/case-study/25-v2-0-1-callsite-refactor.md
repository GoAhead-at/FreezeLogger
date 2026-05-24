# 25. v2.0.1 - Call-Site Refactor (Function-Wraps -> Surgical Patches)

**Date:** 2026-05-24.
**Status:** Internal build only -- the v2.0.1 label is preserved
because the artefact has not been officially released.
**Predecessor:**
[`24-v2-0-1-skyshard-regression-fix.md`](24-v2-0-1-skyshard-regression-fix.md)
(the first cut of v2.0.1, which kept the function-wrap topology and
fixed the calling convention bug).
**Trigger:** Comparative analysis of GarrixWong's
[`skyrim-freeze-fix`](https://github.com/garrixwong/skyrim-freeze-fix).

This document records a structural refactor inside `Phase4Defer`
that changes how the LockB-acquirer gates are installed, **without
changing the gate semantics or the cycle-break invariant**. It also
clarifies why this plugin and `skyrim-freeze-fix` are intentionally
two separate mods despite both addressing engine-level deadlocks.

---

## 1. Background

The first cut of v2.0.1 (doc 24) installed three `safetyhook`
inline hooks at function entries:

- `id 19369` -- LockA acquirer wrap (entry + exit needed for the
  thread-local depth counter).
- `id 40333` -- `AddToTempChangeList` entry gate.
- `id 40334` -- `RemoveFromTempChangeList` entry gate.

This worked, the skyshard regression was fixed, and the structural
fix engaged on real cycles. But the gates' blast radius was wider
than necessary. From the v2.0.1 first-cut diagnostic log
(25 minutes of play, the session captured in doc 24):

```
phase4: queued=12 drained=12 passthrough=26109
```

26,109 of 26,121 gate hits were passthroughs -- calls into
`id 40333` / `id 40334` that arrived without LockA being held by
the calling thread. Those calls do not threaten the cycle; the
gate just notes that and tail-calls the original.

Two costs accumulate at this volume:

1. **Hot-path overhead.** Every engine call into either function
   pays a TLS load (`tl_lockA_depth`) plus a branch, even though
   the deferral path runs less than 0.05% of the time.
2. **Conflict surface with other mods.** Any other mod that
   inline-hooks `id 40333` or `id 40334` directly competes with
   our hook on the function prologue. The first-installed hook
   wins; the loser silently misses every call. Two well-behaved
   mods can break each other.

## 2. GarrixWong's `skyrim-freeze-fix` -- what it teaches

GarrixWong's mod targets a *different* engine deadlock (see §4
below for why the two mods coexist). Its hooking strategy,
however, is the lesson worth absorbing:

```cpp
// skyrim-freeze-fix/src/main.cpp
void Hook(const REL::Relocation<std::uintptr_t>& target, std::uintptr_t offset)
{
    auto& trampoline = SKSE::GetTrampoline();
    SKSE::AllocTrampoline(14);

    auto Callback = +[]( ... ) {
        // serialise the engine's call to BSReadWriteLock with an
        // external BSSpinLock, then forward to the original.
    };

    trampoline.write_call<5>(target.address() + offset, Callback);
}
```

Two patterns stand out:

1. **Patch the call instruction, not the function entry.** The
   target function (`BSReadWriteLock::LockForWrite` /
   `UnlockForWrite`) is left pristine. Other mods that hook those
   functions still see every other caller in the binary. Only the
   one specific path the author cares about gets the wrap.
2. **Use the SKSE trampoline, not a `safetyhook` inline hook.**
   `Trampoline::write_call<5>` rewrites a 5-byte CALL with a
   trampoline-relocated rel32 and returns the original target
   address. It is the right tool when the unit-of-interception is
   "this specific call", not "this function".

The engine-level rationale this plugin already documented in
Phase 4.1 makes the same case: every call into `id 40333` /
`id 40334` that we *care about* arrives via exactly one of two
hand-decoded call sites inside the cycle hub.

## 3. The refactor

### 3.1 What changes

The two function-wrap gates are replaced by two call-site
patches. The LockA acquirer wrap on `id 19369` is unchanged --
that one still requires whole-function bracketing for the depth
counter.

| Before (v2.0.1 first cut)                              | After (this refactor)                                                              |
| ------------------------------------------------------ | ---------------------------------------------------------------------------------- |
| `safetyhook::create_inline(id_19369, &HookedLockAAcquirer)` | unchanged                                                                          |
| `safetyhook::create_inline(id_40333, &HookedAddToTempChangeList)`  | `SKSE::GetTrampoline().write_call<5>(id_19372 + 0x606, &HookedAddInsideAddWrapper)` |
| `safetyhook::create_inline(id_40334, &HookedRemoveFromTempChangeList)` | `SKSE::GetTrampoline().write_call<5>(id_36016 + 0xdcb, &HookedRemoveAtCycleHub)`    |

### 3.2 Why those two specific call sites

Per doc 22, the LockA -> LockB cycle direction has exactly one
shape:

```
id 19369 (LockA) -> id 19371 -> id 35974 -> id 36016 (cycle hub)
                                            +0xdcb -> id 40334
                                            +0xfa3 -> id 19372
                                                       +0x606 -> id 40333
```

Both call instructions are direct (`E8 rel32`), verified pre-patch
against the address-library entry points of `id 40333` /
`id 40334`. No vtable hop, no indirection, no other path: if
control reaches `id 40333` or `id 40334` while `tl_lockA_depth >
0`, it has just executed one of those two CALLs.

Outside the cycle, callers reach `id 40333` / `id 40334` via
many other paths (the 26,109 passthroughs in the v2.0.1 first-cut
log are the tail of that distribution). Those callers never had
LockA held in the first place; intercepting them costs us TLS
loads and risks competing with other mods' hooks for zero benefit.

### 3.3 Calling back to the original

After `write_call<5>` rewrites the 5 bytes at the patched call
site, the original entry of `id 40333` / `id 40334` is unmodified.
The drain (and the no-LockA-held passthrough branch) calls the
originals directly:

```cpp
g_orig_id40333 = reinterpret_cast<ProcessListsFn>(
    REL::Relocation<std::uintptr_t>{REL::ID(40333)}.address());
g_orig_id40334 = reinterpret_cast<ProcessListsFn>(
    REL::Relocation<std::uintptr_t>{REL::ID(40334)}.address());
```

If another mod has placed an inline hook on `id 40333` or
`id 40334`, our drain calls into *their* hook (because their
hook lives at the function prologue). The drain forwards the
deferred call onto whatever mod chain expects it. The previous
function-wrap design would have stomped that other mod's hook
on `Install()`.

### 3.4 Pre-patch verification

To avoid silently breaking another mod that has *already*
patched the same call site, `Phase4Defer::Install` reads the
5 bytes at each call site before patching and aborts if either
of:

- the first byte is not `0xE8`, or
- the rel32 does not target the expected address-library entry.

A failed verification logs a critical message and downgrades the
plugin to "v1.0 runtime-breaker only" without leaving partial
patches in place.

### 3.5 Trampoline allocation

`SKSE::AllocTrampoline(64)` is now called once at
`SKSEPlugin_Load` time (in `main.cpp`). Each `write_call<5>`
consumes 14 bytes; the 64-byte budget covers the two
Phase4Defer patches plus headroom for any future call-site
patches without re-allocating.

### 3.6 What does *not* change

- The cycle-break invariant. While `tl_lockA_depth > 0`, both
  LockB-acquiring call sites defer; LockB is never acquired
  inside a LockA window, so the AB-BA pairing cannot form.
- The synchronous `kInTempChangeList` bit toggle. The skyshard
  fix from doc 24 is preserved as defensive scaffolding -- it
  costs one atomic op per gated call and idempotently aligns
  with the drain's later original-function call.
- The drain semantics. `DrainDeferredOnExit` still moves the
  TLS queue into a local copy and iterates FIFO. Per-thread call
  ordering is preserved.
- The 6-arg `bool`-returning wrap on `id 19369`. Still the
  load-bearing skyshard fix.
- The runtime breaker (`AcquireHook` / `WaitGraph` / `Breaker` /
  `Reaper`) remains installed as defence-in-depth; if any cycle
  path slips past the structural fix, the runtime breaker still
  catches and force-releases.

## 4. Why this plugin and `skyrim-freeze-fix` are separate mods

The two mods address two unrelated engine deadlocks. A user can
run both simultaneously; neither replaces the other.

| Concern                  | `WorkerSpinLockFix` (this plugin)                                                              | `skyrim-freeze-fix` (GarrixWong)                                                  |
| ------------------------ | ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| Lock primitive           | `BSSpinLock`                                                                                   | `BSReadWriteLock` (with an external `BSSpinLock` used as a serialisation guard)   |
| Deadlock pattern         | AB-BA inversion between LockA (`SkyrimSE+0x2eff8e0`) and LockB (`SkyrimSE+0x2f3b8e8`)          | AB-BA between two RW locks during cell loading + a recursive read-then-write     |
| Trigger                  | Heavy combat / archery / casting / Papyrus delay functors on a populated worldspace            | Cell loading                                                                      |
| Function targeted        | LockA acquirer (`id 19369`) + cycle-hub call sites at `id 36016+0xdcb` and `id 19372+0x606`    | `id 40287` (`FUN_1406d3ff0`) at offsets `+0x39C` and `+0x415`                     |
| Mechanism                | Defer LockB acquires while LockA is held; drain on LockA release                               | Serialise the engine's RW write-acquire/release with an external spinlock         |
| Compatibility            | Neither plugin patches the other's targets. Both can run in the same load order.               | Same.                                                                             |

This refactor *also* updates the plugin README to declare
explicitly that `WorkerSpinLockFix` does **not** address
cell-loading freezes, the recursive RW-lock pattern, or any
`BSReadWriteLock` deadlock, so a user who is hitting those
symptoms knows to install `skyrim-freeze-fix` (or its successor)
in addition to this plugin rather than instead of it.

## 5. Verification

### 5.1 Build

```
cmake --build C:/sk/wslf/r --config Release
```

Clean build, single object file rebuilt
(`Phase4Defer.cpp`, `main.cpp`).

### 5.2 Banner

After this refactor the install banner reads:

```
[Phase4Defer] structural fix armed (v2.0.1 call-site edition:
6-arg bool-returning wrap on id 19369 + two surgical call-site
patches inside the cycle hub). Hooks: id 19369 (LockA acquirer
wrap) at 0x..., id 36016+0xdcb (-> HookedRemoveAtCycleHub) at
0x..., id 19372+0x606 (-> HookedAddInsideAddWrapper) at 0x....
Function entries of id 40333 / id 40334 are NOT patched, so
other mods that hook those functions still work. LB->LA
direction (id 40285 / id 36614 / id 38413) is intentionally not
hooked. diagnostic_logging=OFF
```

### 5.3 Steady-state telemetry

Expected shape on the periodic `phase4:` line:

- `queued` and `drained` increment in lock-step, both > 0 during
  combat / archery / heavy Papyrus.
- `passthrough` is **dramatically lower** than the v2.0.1 first
  cut (orders of magnitude, since we no longer intercept calls
  outside the cycle hub).
- `breaks_done = 0` -- the runtime breaker never fires, because
  the structural fix preempts every cycle.

### 5.4 Skyshards (regression check from doc 24)

Skyshards continue to work. The 6-arg wrap and the synchronous
bit toggle that fixed the regression are unchanged.

## 6. Trade-offs

The refactor narrows blast radius at the cost of taking on a
**call-site-stability** dependency. If a future Skyrim engine
update (or another mod's call-site patch on the same byte) shifts
the rel32 inside `id 36016+0xdcb` or `id 19372+0x606`, the
pre-patch verification rejects the install and the plugin
downgrades to v1.0 runtime-breaker mode. That is a louder failure
mode than the function-wrap design (which would silently install
on any function whose entry is still 5+ bytes of patchable
prologue) but easier to diagnose: the rejection logs the actual
bytes and the expected target.

Skyrim SE 1.5.97 is a frozen runtime; this risk is theoretical
for as long as the plugin pins to `Runtime::SE` and version
1.5.97.

## 7. References

- `skyrim-freeze-fix/src/main.cpp` (GarrixWong) -- the
  inspiration for `Trampoline::write_call<5>` adoption.
- [`22-v2-phase4-1-cycle-hub-characterisation.md`](22-v2-phase4-1-cycle-hub-characterisation.md)
  -- the cycle-hub decode that identifies `id 36016+0xdcb` and
  `id 19372+0x606` as the only LockA-held entry points to LockB.
- [`24-v2-0-1-skyshard-regression-fix.md`](24-v2-0-1-skyshard-regression-fix.md)
  -- the v2.0.1 first cut, where the 6-arg wrap landed.
