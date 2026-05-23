# 24. v2.0.1 - Skyshard / Scripted-Animation Activation Regression Fix

**Date:** 2026-05-23.
**Status:** Internal build only -- v2.0.1 was never officially
released. The version label is retained because the artefact
never left the development tree.

The skyshard regression went through three diagnostic cuts before
landing on the actual cause. The first two were wrong; the third
is supported by direct log evidence from a play session with
`diagnostic_logging = true`.

| Cut | Hypothesis | Outcome |
|---|---|---|
| 1 | Deferred `kInTempChangeList` bit toggle leaves `Actor::boolBits` in pre-call state during the deferral window; downstream readers of bit 9 see stale state and skip the animation. | **Wrong.** The user reported the bit-toggle split-defer did not fix skyshards. |
| 2 | The wrap on `id 19369` was declared `void`, but `id 19369`'s epilogue (`movzx eax, bl; ret`) returns `bool`. Callers gating on the activation chain's success see whatever junk our `--tl_lockA_depth` left in `eax`. | **Wrong.** The user reported the bool-return preserving wrap did not fix skyshards either. The bool return *is* real and observable in the log, but it wasn't load-bearing for skyshards. |
| 3 | The wrap was declared with 4 args (`rcx, rdx, r8b, r9`), but `id 19369`'s body reads stack args 5 and 6 at `[rbp+0x77]` (dword) and `[rbp+0x7f]` (byte). MSVC's outgoing call frame from the 4-arg `unsafe_call` placed our hook's uninitialised stack-arg slots where the trampoline expected the engine's stack args. The original ran with garbage for arg 5 and arg 6 every invocation. | **Correct.** Confirmed by direct log evidence (see §3.1). User confirmed: "skyshard works now." |

The current v2.0.1 carries all three fixes layered together:

1. **6-arg wrap signature** - the load-bearing fix.
2. **`bool` return capture via `unsafe_call<bool>`** - the wrap
   forwards the original's `eax` to its caller. Independently
   correct; in this codebase it is mostly defensive (most
   observed engine callers don't gate hard on it) but cheap to
   keep right.
3. **Synchronous `kInTempChangeList` toggle in the gates** -
   defensive scaffolding. Idempotent w.r.t. the deferred drain.
   No-op for the skyshard case but eliminates a class of
   potential future regressions (any reader of bit 9 from inside
   the LockA scope) for one atomic op per gate hit.

**Predecessor docs:**
- [`22-v2-phase4-1-cycle-hub-characterisation.md`](22-v2-phase4-1-cycle-hub-characterisation.md)
  §4 - the original correctness audit. Was sufficient for the
  cycle's *immediate* call-site followup but did not extend to
  the wrap's calling convention.
- [`23-v2-release.md`](23-v2-release.md) - v2.0.0 release note.

---

## 1. Symptom

Reported by the user during routine play after installing
`WorkerSpinLockFix v2.0.0`:

> i made an interesting observation. with our fix loaded i can't
> interact with skyshards. when i disable the mod it works.
> skyshards are objects placed around skyrim which can be found
> by the player. when they interact with it (E) the player gets a
> perk point. once interacted a small animation plays. but with
> the mod enabled nothing happens. but i can interact with other
> NPCs pressing E and open treasure boxes.

Reproduction: at any skyshard, with v2.0.0 active, pressing E
produces no animation, no perk point, no log entry on the
Papyrus side. Disabling `[phase4_defer]` (leaving the v1.0
runtime breaker armed) immediately restores the behaviour.

NPC dialogue and treasure-chest activation are unaffected.

The bit-toggle and the bool-return fixes shipped in earlier
v2.0.1 builds did not change behaviour: skyshards still failed.

## 2. Why this only affects scripted-animation activators

Different activator classes route through different engine code
paths:

| Activator | Path through engine |
|---|---|
| NPC dialogue | Native dialogue UI; never enters `id 19369`. |
| Container open | Native container UI; never enters `id 19369`. |
| **Skyshard / similar scripted activator** | Papyrus `OnActivate` -> `Game.GetPlayer().PlayIdle(...)` or `Debug.SendAnimationEvent(player, "...")` -> animation event dispatch -> `id 19369` -> `id 19371` -> `id 35974` -> `id 36016` -> handler. |

The first two never enter `id 19369`'s scope, so the wrap on
`id 19369` is never traversed and v2.0.0's bug is invisible.
The third does, and the wrong wrap signature corrupted the
stack args that drive the activation logic.

## 3. Root cause - dropped stack arguments on `id 19369`

`id 19369`'s prologue (from `analysis/out_id19369_full.txt`):

```
+0x0   mov    rax, rsp
+0x3   mov    qword ptr [rax + 0x20], r9    ; home arg 4 (qword)
+0x7   mov    byte ptr  [rax + 0x18], r8b   ; home arg 3 (byte only)
+0xb   push   rbp
+0xc   push   rsi
+0xd   push   rdi
+0xe   lea    rbp, [rax - 0x4f]
+0x12  sub    rsp, 0xc0
```

After the prologue, `rbp = orig_rsp - 0x4f`. The standard MS x64
home-space layout puts:

| Offset from rbp | Offset from orig_rsp | Slot |
|---|---|---|
| `[rbp + 0x57]` | `[orig_rsp + 0x08]` | home for rcx (arg 1, qword) |
| `[rbp + 0x5f]` | `[orig_rsp + 0x10]` | home for rdx (arg 2, qword) |
| `[rbp + 0x67]` | `[orig_rsp + 0x18]` | home for r8 (arg 3, byte) |
| `[rbp + 0x6f]` | `[orig_rsp + 0x20]` | home for r9 (arg 4, qword) |
| `[rbp + 0x77]` | `[orig_rsp + 0x28]` | **stack arg 5** |
| `[rbp + 0x7f]` | `[orig_rsp + 0x30]` | **stack arg 6** |

The function body reads from those last two positions:

```
+0x47   movzx  eax, byte ptr  [rbp + 0x7f]   ; arg 6 (byte)
+0x87   mov    eax, dword ptr [rbp + 0x77]   ; arg 5 (dword)
+0x2f5  cmp    byte ptr       [rbp + 0x7f], 0
+0x41f  mov    r9d, dword ptr [rbp + 0x77]
+0x583  mov    edx, dword ptr [rbp + 0x77]
+0x5c1  mov    r9d, dword ptr [rbp + 0x77]
```

and forwards both stack args verbatim through the recursive
self-call at +0x9d. There are **no accesses higher than
`[rbp + 0x7f]`**, so the function takes exactly six args.

v2.0.0 / the first two cuts of v2.0.1 declared the wrap with
only four:

```cpp
bool __fastcall HookedLockAAcquirer(
    void* rcx, void* rdx, std::uint8_t r8b, std::uintptr_t r9)
{
    ++tl_lockA_depth;
    const bool result = g_hook_lockA_acquirer.unsafe_call<bool>(
        rcx, rdx, r8b, r9);                  // <-- 4 args only
    ...
}
```

When the engine calls `id 19369` with 6 args, the inline-hook
JMP at `id 19369`'s entry transfers control to our wrap. The
engine's outgoing stack args are sitting at `[engine_rsp+0x28]`
(arg 5) and `[engine_rsp+0x30]` (arg 6). Our wrap reads only
the four register args, then calls `unsafe_call<bool>(...)`
with four args. MSVC arranges that 4-arg call from **our**
hook's stack frame, leaving our outgoing-arg slots at
`[hook_rsp+0x28]` and `[hook_rsp+0x30]` uninitialised
(the compiler only writes them when the called function takes
that many args).

The trampoline's first instructions are the original
`id 19369` prologue. After that prologue, when the original's
body reads `[rbp+0x77]` and `[rbp+0x7f]`, it's reading from
**our** hook's outgoing-arg slot positions, which contain
stack-allocated garbage from our hook's locals or whatever
prior call left there.

Result: every invocation of `id 19369` runs with corrupted
arg 5 (a 32-bit value) and arg 6 (a byte). The function then
makes decisions based on garbage, producing whatever its body
does for arbitrary arg values - which for at least the
animation-event path that drives skyshards meant "do nothing
visible".

### 3.1 Direct log evidence

With the 6-arg fix shipped and `diagnostic_logging = true`,
every `id 19369` invocation is logged with its real arguments:

```
[Phase4Defer.diag] LockA-WRAP-ENTER tid=28956 rcx=0x16e9f2f8a40
  rdx=0x16e9f2f8a40 r8b=0x00 r9=0x16e13c51900
  arg5=0x00000001 arg6=0x00 depth_in=0
[Phase4Defer.diag] LockA-WRAP-EXIT  tid=28956 result=0 depth_out=0
```

Across thousands of invocations in a single session
`arg5=0x00000001` and `arg6=0x00` are passed consistently by
the engine. With the prior 4-arg wrap, both values would have
been replaced with whatever happened to be at our hook's
`[rsp+0x28]` / `[rsp+0x30]` - effectively random.

Before the 6-arg fix:
- Skyshard activations: silently fail.

After the 6-arg fix (same session as the log):
- Skyshard activations: succeed. User confirmed.
- 9 deferred `id 40333` calls observed across one minute of
  play, every one of them drained correctly:
  ```
  ADD-DEFER   tid=... pl=0x7ff7aa797ea0 actor=... depth=1 qsize=1
  DRAIN       tid=... count=1
    drained kind=ADD pl=... actor=...
  ```
  The structural fix is engaging on real cycles
  (`phase4: queued=9 drained=9 passthrough=8725`) and the v1
  runtime breaker never has to fire (`breaks_done=0`).

## 4. The fix

### 4.1 6-arg wrap signature (the load-bearing fix)

```cpp
bool __fastcall HookedLockAAcquirer(
    void*           rcx,         // arg 1
    void*           rdx,         // arg 2
    std::uint8_t    r8b,         // arg 3
    std::uintptr_t  r9,          // arg 4
    std::uint32_t   stack_arg5,  // arg 5 at orig_rsp+0x28
    std::uint8_t    stack_arg6)  // arg 6 at orig_rsp+0x30
{
    ++tl_lockA_depth;
    const bool result = g_hook_lockA_acquirer.unsafe_call<bool>(
        rcx, rdx, r8b, r9, stack_arg5, stack_arg6);
    const int depth = --tl_lockA_depth;
    if (depth == 0) DrainDeferredOnExit();
    return result;
}
```

MSVC reads the stack args from `[rsp+0x28]` / `[rsp+0x30]`
when the wrap is entered (where the engine put them) and writes
them to its own outgoing-call slots when calling
`unsafe_call<bool>` (so the trampoline finds them where its
prologue expects).

### 4.2 `bool` return preservation

Capturing the trampoline's return via `unsafe_call<bool>` and
returning it from the wrap propagates the engine's expected
result code through. Without this, `eax` on wrap exit is
clobbered by `--tl_lockA_depth`. Independently correct.

### 4.3 Synchronous `kInTempChangeList` toggle (defensive)

```cpp
void __fastcall HookedAddToTempChangeList(void* pl, void* actor) {
    if (tl_lockA_depth > 0) {
        if (actor) {
            BoolBitsAtomic(actor)->fetch_or(
                kInTempChangeListMask,
                std::memory_order_acq_rel);
        }
        tl_deferred.push_back({ DeferKind::kAdd, pl, actor });
        ...
    }
    ...
}
```

`HookedRemoveFromTempChangeList` mirrors with `fetch_and(~mask)`.

The drain calls the original under LockB; both writes converge
to the same final value (idempotent fetch_or / fetch_and). Kept
because:

- Idempotent w.r.t. the drain.
- Eliminates any stale-flag window for any future caller in the
  engine that might query bit 9 inside the LockA scope.
- Cheap (one atomic op per gate hit, no lock contention) and
  removes a class of potential future regressions for free.

### 4.4 Diagnostic logging

`[phase4_defer].diagnostic_logging` (default `false`) gates
per-call info logs in the LockA wrap and the two LockB gates,
plus a header + per-call line on each drain. Volume is high
during gameplay (~150 LockA-WRAP-ENTER lines per minute,
hundreds of ADD-PASS lines per minute), bounded by spdlog's
async buffering, and trivially toggled. Used to diagnose this
exact regression class.

Workflow:
1. Set `diagnostic_logging = true` in the TOML.
2. Reproduce the symptom once (e.g. press E on a skyshard).
3. Quit, set `diagnostic_logging = false`, read
   `SKSE\Logs\WorkerSpinLockFix.log`.

## 5. Versioning

The user explicitly requested the version label not be bumped:

> interaction with skyshards still do not work. fix 2.0.1.
> keep the version as it isn't officially released

`CMakeLists.txt` stays at `VERSION 2.0.1` across all three cuts
of the fix. The shipping artefact is repackaged in place at
`dist-out/WorkerSpinLockFix_v2.0.1.rar`.

The startup banner reflects the final state:

```
[Phase4Defer] structural fix armed (v2.0.1: 6-arg bool-returning
wrap on id 19369 + synchronous kInTempChangeList toggle +
deferred bucket-array op). Hooks: id 19369 ... id 40333 ...
id 40334 ... LB->LA direction ... is intentionally not hooked.
diagnostic_logging=ON
```

## 6. What this exposes about the original audit methodology

Three cumulative methodology lessons:

1. **Doc 22 §4's audit (v2.0.0):** "does the immediate next
   instruction read what we just deferred?" Covers state
   mutation visibility at the call site. Insufficient for
   anything outside the call site.

2. **First v2.0.1 cut:** "of all the data the deferred function
   mutates, does any observer in the rest of the deferred-from
   frame (or any concurrent thread within the deferral window)
   read it?" Covers cross-frame and cross-thread mutation
   visibility. Still insufficient: it addresses *mutation*
   visibility but ignores wrap signature.

3. **Second v2.0.1 cut:** "does the wrap declare and propagate
   the wrapped function's return type?" Covers return-value
   plumbing. Still insufficient: ignores stack args.

The fully corrected audit question for any future wrap or hook
in this plugin is:

> **Read the full prologue AND epilogue AND every `[rbp+offset]`
> access in the body before declaring the wrap.**
>
> - Prologue: count register-arg homing and identify which args
>   are byte / dword / qword (`mov [rsp+0x18], r8b` says r8 is
>   a byte; `mov [rsp+0x18], r8` says r8 is a qword).
> - Body: any `[rbp+0x77]`, `[rbp+0x7f]`, `[rbp+0x87]` etc.
>   reads (after a `lea rbp, [rax-N]` prologue) are stack args
>   5, 6, 7. Find the highest such offset and that's the arg
>   count. Note the access width (byte / dword / qword) for
>   each.
> - Epilogue: `mov ?, eax` immediately before `ret` says the
>   function returns a value; declare the wrap with the right
>   width.
>
> Then declare the wrap as `RetType __fastcall Hook(arg1, ...,
> argN)` matching every observed argument and return.

Step 1 is mechanical. Step 2 is a single grep over the dump.
Step 3 is mechanical. None of these were done before the v2.0.0
release; all three are now part of the v2.x audit checklist.

## 7. Open work

| Item | Status | Notes |
|---|---|---|
| Synthetic harness for `Phase4Defer` | Pending | Should also exercise: (a) callee returns 1 / 0; wrap forwards correctly. (b) caller passes non-zero stack args; wrap forwards them verbatim to the trampoline. |
| In-game smoke test on freeze-prone scenarios | **Validated for skyshards.** Pending for combat / archery / casting / Papyrus-heavy mods, lockpicking, word walls, bookshelves, sleep transitions. | Stats line shows 9/9 queued/drained in one minute of normal play; 0 cycles formed. The structural fix is operating correctly. |
| Audit other wraps for the same class of bug | Pending | `Phase4Defer.cpp` is the only place we wrap a non-void engine function, and the only place the wrap's calling convention isn't trivially `void Func(rcx)`. The v1 `AcquireHook` on `id 12210` (`BSSpinLock::Lock`) wraps a void single-arg function; not affected. Re-confirm before any future wrap is added. |

## 8. Lesson

Three lessons compounded into the same regression:

1. **Read the full prologue AND epilogue AND every stack-arg
   access.** A wrap is the calling-convention adapter for the
   wrapped function. Getting any of the three wrong corrupts
   the wrapped function's behaviour silently.

2. **A correct deferral can still corrupt visible state if the
   wrap's signature is wrong.** The cycle-breaking logic (the
   depth counter, the queue, the drain) was correct from
   v2.0.0 onwards. The bug was orthogonal: a calling-convention
   mismatch on the host wrap. These are independent failure
   modes; both need to be audited independently.

3. **Add diagnostic logging early, not after multiple wrong
   hypotheses.** The 6-arg root cause was visible in a single
   `LockA-WRAP-ENTER` line that included `arg5=` and `arg6=`.
   Two earlier hypotheses could have been falsified in
   minutes if logging had been wired in from the start.
   Keeping `diagnostic_logging` in the toml as a one-line
   toggle for any future regression class is the durable
   takeaway.
