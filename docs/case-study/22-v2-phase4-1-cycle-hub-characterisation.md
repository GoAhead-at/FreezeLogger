# 22. v2.0 Phase 4.1 - Cycle Hub Characterisation and Final C5 Design

**Date:** 2026-05-22
**Status:** Phase 4.1 complete. The two LockB acquirers
(id 40333, id 40334) are short, well-shaped functions whose
mutations are not read by their direct callers. The cycle hub
id 36016 is a 96-way event-dispatch switch with two LockB-
firing call sites. The post-id-40333 / post-id-40334 followup
in id 36016 reads only fields that those functions don't write.
**Conclusion: a `defer-when-LockA-held` wrapper at the entry
of each LockB acquirer is correctness-preserving.** The C5
design is reduced to **three inline hooks** (id 19369 wrap,
id 40333 entry-gate, id 40334 entry-gate) plus a thread-local
queue that drains after id 19369 returns. id 40285 is not
hooked at all - the LB->LA direction can be left alone once
the LA->LB direction is broken.
**Inputs:**
`analysis/dump_one_func.py` (re-run on id 36016 / id 19371 /
id 35974 / id 19372 / id 40333 / id 40334 / id 38413).
**Produces:**
`analysis/out_id36016_full.txt`, `analysis/out_id19371_full.txt`,
`analysis/out_id35974_full.txt`, `analysis/out_id19372_full.txt`,
`analysis/out_id40333_full.txt`, `analysis/out_id40334_full.txt`,
`analysis/out_id38413_full.txt`.

## 1. The function shapes, decoded

### id 19369 (LockA acquirer)
Already documented in docs 17-20. A traverser. Holds LockA for
its body; calls id 19371 directly at +0x37b on the LA->LB path.

### id 19371 (animation event dispatcher) - 0x340 bytes
Takes `(rcx = formA, rdx = formB)`. Filters both formA and formB
by `[+0x1a] == 0x3e` (FormType::ActorCharacter). Looks up an
animation graph handle on the result of `vtable[+0x380]`. Builds a
local matrix-transform (xmm regs, dot products, vec3 + 4th
component) on the stack as an event packet. Decides between two
emit paths:

- `+0x234`: `call id 35974` (this is the LA->LB path)
- `+0x249`: `call qword ptr [rax + 0x548]` (a different
  vtable dispatch on the actor; not on our cycle path)

After either dispatch, follows up with BSStringT building and
returns.

### id 35974 (event-emit helper / first BSAutoLock<LockC>) - 0xf0 bytes

```c
void EmitAnimEvent(EventSink* rbx, RefHandle rdx, vec3* r8, float xmm3, byte arg5) {
    Build packet at [rsp+0x30] {type=0x20, refHandle, vec3, xmm3, byte};
    if (rdx) lock_inc([rdx+0x28]);                  // refcount the handle
    if (g_some_flag != 0) {                          // [0x182a959]
        BSAutoLock lock(LockC);                      // <-- THIRD spinlock at RVA 0x2973a78
        bool consumed = (*rbx->[+0x10])->vtable[+0x8](&packet);
        // ... lock release inline ...
        if (consumed) call id 36016 with the packet; // <-- THE CYCLE-FIRING DIRECT CALL
    }
}
```

Three observations:

- id 35974 introduces a **third spinlock (LockC)** at RVA
  `0x2973a78`. Phase 2's BFS missed it because LockC isn't on
  the AB-BA cycle - it's a separate lock around event-sink
  dispatch. Documenting it here for completeness.
- The vtable dispatch on `[rbx+0x10]->[vt+0x8]` is the
  event-sink callback. This is a separate dispatch tree that
  doesn't enter the AB-BA cycle (the dispatch returns a bool
  that gates the id 36016 call).
- The id 36016 call is **direct**, not indirect.

### id 36016 (cycle hub - 96-way event dispatch) - 0x1000 bytes

Layout:

```c
void HandleAnimEvent(EventPacket* rdi) {
    int category = rdi->[+0x0] - 2;
    if ((unsigned)category > 0x60) return;
    void* jt = .rdata + 0x5c8e00;
    goto jt[category];                              // 96-way dispatch table

case ...: ...                                       // most cases just call vtable methods
case +0xd4a:                                        // *** CYCLE-FIRING CASE #1 ***
    Form* form1 = HashLookup(rdi->[+0x8]);          // global hash lookup
    Form* form2 = HashLookup(rdi->[+0x10]);
    if (!form1 || !form2) goto end_of_case;
    if (form2 == g_PlayerCharacter) {               // *** PlayerCharacter gate ***
        if (PCSingleton.SomePredicate()) {
            id_40334(g_ProcessLists, form1);        // <-- DIRECT call to LockB acquirer
            // followup: read [form1+0xe0] bit 30 (NOT bit 9 -- safe);
            // read [form1+0xf0] (NOT written by id 40334);
            // call 0x644300, more state-mutating helpers...
        }
    } else {
        // alternative non-player path -- doesn't fire LockB
    }
case +0xf8b:                                        // *** CYCLE-FIRING CASE #2 ***
    Actor* form = rdi->[+0x8];
    id_19372(form, rdi->[+0x10], rdi->[+0x18]);     // direct call; id 19372 then calls id 40333
    // followup: jmp to common epilogue (no immediate state read)
...
}
```

The "PlayerCharacter gate" (`cmp rdi, [0x2eff7d8]; jne ...` at
+0xd9c) is what restricts the cycle from firing on every event:
the LA->LB edge only fires for animation events whose `[+0x10]`
hash key resolves to the player.

### id 19372 (AddToTempChangeList wrapper) - 0x9c0 bytes

A complex preparation routine that does:

- gates on a stack flag `[rsp + 0x24]`,
- calls `id 40333` directly at +0x606 (rcx=g_ProcessLists, rdx=actor),
- followup: reads `[rsp + 0x24]`, calls `0x63f810`, etc.

The followup code does **not** read fields that id 40333 mutated
(see post-id-40333 analysis below).

### id 40333 (AddToTempChangeList) - 0x170 bytes - **DECODED**

```c
void AddToTempChangeList(ProcessLists* pl, Actor* actor) {
    BSAutoLock lock(LockB);                                           // +0x1c-+0x2a
    RefHandle h = ResolveHandle(actor, /*out*/&local_h);              // +0x39
    auto* bucket = (BucketArray*)((char*)pl + 0x158);
    int count = bucket->count;                                        // [pl+0x168]
    int found = -1;
    for (int i = 0; i < count; ++i) {                                 // linear search
        if (bucket->data[i] == h) { found = i; break; }
    }
    if (found != -1) goto unlock;                                     // already in list
    if (actor->aiProcess == nullptr) goto unlock;                     // [actor+0xf0]
    auto* tls = gs:[0x58][TlsIndex] + 0x768; int saved = tls[0];      // TLS bookkeeping
    tls[0] = 0x73;
    Animation_OnAddToList(actor, /*notify=*/true);                    // call 0x1949a0
    int new_idx = ResizeOrInsert(&bucket->data, h, sizeof(h), 4, &count);
    bucket->data[new_idx] = h;
    actor->boolBits |= 0x200;                                         // <-- SET kInTempChangeList bit
    tls[0] = saved;
    // BSAutoLock dtor releases LockB
}
```

Mutations under LockB:

- `bucket->data[]` (append slot)
- `bucket->count` (increment, via the resize helper)
- `actor->boolBits |= kInTempChangeList`
- TLS slot (saved/restored, no side effect across the call)

### id 40334 (RemoveFromTempChangeList) - 0x140 bytes - **DECODED**

```c
void RemoveFromTempChangeList(ProcessLists* pl, Actor* actor) {
    BSAutoLock lock(LockB);                                           // +0x1c-+0x2a
    if (g_2f44db0 == actor) g_2f44db0 = nullptr;                      // +0x30-+0x40 (the private global from doc 20)
    RefHandle h = ResolveHandle(actor, /*out*/&local_h);              // +0x4c-+0x4f
    auto* bucket = (BucketArray*)((char*)pl + 0x158);                 // (combined as offsets)
    int count = bucket->count;                                        // [pl+0x168]
    int found = -1;
    for (int i = 0; i < count; ++i) {
        if (bucket->data[i] == h) { found = i; break; }
    }
    if (found == -1) goto unlock;
    if (count == 1) {
        if (bucket->data == nullptr) goto unlock;
        bucket->count = 0;
    } else {
        memmove(&bucket->data[found], &bucket->data[found+1], (count - found - 1) * 4);
        bucket->count--;
    }
    if (actor) actor->boolBits &= ~0x200;                             // <-- CLEAR kInTempChangeList bit
    // BSAutoLock dtor releases LockB
}
```

Mutations under LockB:

- `g_2f44db0` (private to id 40285 + id 40334 -- doc 20)
- `bucket->data[]` (memmove)
- `bucket->count` (decrement)
- `actor->boolBits &= ~kInTempChangeList`

## 2. Post-call followup is decoupled from the LockB mutations

> **POST-RELEASE CORRECTION (added 2026-05-23, revised through
> three diagnostic cuts the same day):** three distinct issues
> with v2.0.0's wrap on `id 19369` surfaced under this audit. The
> first two hypotheses turned out to be wrong; the third is
> supported by direct log evidence.
>
> **Hypothesis 1 (wrong):** the audit question in this section
> was too narrow because it checked only the direct caller's
> *next instruction* for stale reads of the deferred mutation.
> Cut 1 of v2.0.1 split the deferred `kInTempChangeList` bit
> toggle out so it ran synchronously. The user reported skyshards
> still did not work.
>
> **Hypothesis 2 (wrong):** `HookedLockAAcquirer` was declared
> `void`, but `id 19369`'s epilogue is `movzx eax, bl; ret`. Cut
> 2 of v2.0.1 made the wrap return `bool` and captured the
> trampoline's return via `unsafe_call<bool>`. The user reported
> skyshards still did not work.
>
> **Actual root cause (correct):** `id 19369` takes **six** args,
> not four. The body reads stack arg 5 (dword) at `[rbp+0x77]`
> and stack arg 6 (byte) at `[rbp+0x7f]` six times across the
> function (line locations: +0x47, +0x87, +0x2f5, +0x41f, +0x583,
> +0x5c1). The wrap was declared with only four register args, so
> when MSVC called the trampoline through `unsafe_call`, the
> outgoing stack-arg slots at `[rsp+0x28]` / `[rsp+0x30]` were
> uninitialised. The trampoline read garbage for arg 5 and arg 6
> on every invocation, and the function ran with corrupted
> arguments. Cut 3 of v2.0.1 declares the wrap with all six args
> and forwards them verbatim. Skyshards work; the structural fix
> engages on real cycles in normal play (`phase4: queued=9
> drained=9` over one minute, all preempted before the AB-BA
> cycle could form). The bool-return capture and the synchronous
> bit toggle are kept as defensive scaffolding.
>
> See [`24-v2-0-1-skyshard-regression-fix.md`](24-v2-0-1-skyshard-regression-fix.md)
> for the full retrospective with log evidence and the corrected
> audit methodology ("read the full prologue AND epilogue AND
> every `[rbp+offset]` access in the body before declaring the
> wrap").

The critical correctness check for any "defer the LockB acquirer
when LockA is held" design is: **does the caller of id 40333 / id
40334 immediately read fields that those functions just mutated?**
If yes, deferral is incorrect (the caller will see stale state
and behave wrong). If no, deferral is safe (the mutation is
just slightly delayed; eventual consistency holds).

The two call sites under audit are:

### Followup at id 36016 +0xdcb (after id 40334)

```
+0xdcb    call 0x6d9890                           ; id 40334 - clears bit 9 of [rbx+0xe0]
+0xdd0    xor sil, sil
+0xdd3    mov eax, dword ptr [rbx + 0xe0]         ; READS [rbx+0xe0]
+0xdd9    shr eax, 0x1e                           ; shifts to expose bit 30
+0xddc    test al, 1                              ; tests BIT 30 (NOT bit 9)
+0xdde    jne <skip>
+0xde0    mov sil, 1
+0xde3    mov rcx, qword ptr [rbx + 0xf0]         ; reads aiProcess (NOT written by id 40334)
+0xdea    test rcx, rcx
+0xded    je <skip>
+0xdef    movzx edx, sil
+0xdf3    call 0x644300                           ; aiProcess->something
```

**id 40334 writes bit 9. id 36016 reads bit 30.** Different bits.
Deferral is safe: the read at +0xdd3 will get the correct bit-30
value either way. id 40334's bit-9 mutation is invisible to the
followup.

### Followup at id 19372 +0x606 (after id 40333)

```
+0x606    call 0x6d9720                           ; id 40333
+0x60b    cmp byte ptr [rsp + 0x24], 0            ; reads STACK FLAG (NOT id 40333's mutations)
+0x610    je <branch>
+0x612    call 0x63f810                           ; unrelated helper
```

The followup reads only `[rsp + 0x24]` -- a flag id 19372 set
itself before calling id 40333. None of id 40333's writes
(bucket-array append, count++, bit-9 set) are read by this
immediate followup. Deferral is safe.

### Followup at id 36016 +0xfa3 (after id 19372 -> id 40333)

```
+0xfa3    call 0x2977d0                           ; id 19372 (which contains the id 40333 call)
+0xfa8    jmp 0x5c6fbc                            ; immediate jump to common epilogue
```

**Empty followup**. The case branch ends with an unconditional
jmp to the dispatch epilogue. No state is read between the call
and the function return. Deferral is trivially safe.

## 3. The LB->LA direction does not need to be touched

The LB->LA path is `id 40285 -> [vt+0x790] -> id 36614 -> id 38413
-> id 19369`. id 38413 (decoded: 0xe0 bytes) does:

```c
char id_38413(SomeActor* this_, Form* otherForm) {
    Actor* via = ResolveHandle(this_->[+0x10]+0xd8);
    if (!via) return 0;
    via->[+0xd8] = g_someValue;                                // [0x1ebda40]
    if (otherForm == g_someGlobal) return 0;                   // [0x28ae258] gate
    id_19369(via, otherForm, /*flags=*/0);                     // *** LockA acquire ***
    this_->[+0x10]->[+0x470] = 0;
    // BSHandleRefObject release on `via`
    return 1;
}
```

The function unconditionally calls id 19369 once its early
returns clear. **It can't be made not-acquire-LockA** without
breaking semantics.

But we don't need to. The AB-BA deadlock requires *both*
back-edges to be unbroken. Once we break the LA->LB edge by
deferring id 40333 / id 40334 calls, the LB->LA edge is harmless
(the LockB-holding thread can call id 19369 to acquire LockA
freely, because no LockA-holding thread is blocked on LockB).

This means we can leave id 40285, id 36614, id 38413 entirely
alone. **Only the LA->LB direction needs intervention.**

## 4. The C5 design, finalised

```c++
// Hook 1: wrap id 19369 with safetyhook::create_inline.
//   Track LockA-held depth so nested calls aren't double-counted.
thread_local int g_lockA_depth = 0;
thread_local std::vector<DeferredCall> g_deferred;  // small, usually 0-3 entries

void hooked_id_19369(Args... args) {
    g_lockA_depth++;
    original_id_19369(args...);                                  // does its work, releases LockA naturally
    g_lockA_depth--;
    if (g_lockA_depth == 0) DrainDeferred();                     // <-- NOW safe to acquire LockB
}

// Hook 2: wrap id 40333 entry.
void hooked_id_40333(ProcessLists* pl, Actor* actor) {
    if (g_lockA_depth > 0) {
        g_deferred.push_back({DeferredCall::kAdd, pl, actor});   // queue and skip
        return;
    }
    original_id_40333(pl, actor);
}

// Hook 3: wrap id 40334 entry.
void hooked_id_40334(ProcessLists* pl, Actor* actor) {
    if (g_lockA_depth > 0) {
        g_deferred.push_back({DeferredCall::kRemove, pl, actor});
        return;
    }
    original_id_40334(pl, actor);
}

void DrainDeferred() {
    auto local = std::move(g_deferred);                          // takes ownership; g_deferred is empty now
    for (auto& call : local) {
        if (call.kind == DeferredCall::kAdd)    original_id_40333(call.pl, call.actor);
        else                                    original_id_40334(call.pl, call.actor);
    }
}
```

That's the entire fix. **Three inline hooks**, one thread-local
counter, one thread-local vector.

### Why this is correct

1. **The cycle is broken.** When T1 is in id 19369 and reaches
   id 36016 -> id 40334, the call to id 40334 is intercepted; T1
   does *not* try to acquire LockB. T1 finishes id 19369 and
   releases LockA. T1 then drains the queue and calls id 40334;
   LockB is acquired without LockA being held. The AB->BA cycle
   simply cannot form on this thread.
2. **The mutations still happen.** Every queued id 40333 / id
   40334 call is executed, just slightly later (within
   microseconds of LockA being released).
3. **The call ordering is preserved per-thread.** T1's queue is
   drained in FIFO order on T1's own thread. Other threads' calls
   are unaffected.
4. **The followup correctness audit (section 2) shows the
   immediate followup at each call site does not read fields
   that the deferred function would have written.** State
   visible to the *current* function up to the point of return
   is unaffected by the deferral.

### Why this is small

| component                      | LOC estimate |
| ------------------------------ | ------------ |
| three safetyhook InlineHook installs | ~30        |
| three trampoline functions     | ~30          |
| DrainDeferred + queue type     | ~25          |
| logging hooks (optional)       | ~20          |
| **total**                      | **~100-120 LOC** |

Layered as a new `Phase4Defer` module alongside the existing
`AcquireHook` / `Breaker` / `Reaper` modules in v1.0.0.
The runtime breaker remains installed as defence-in-depth.

### What this does NOT do

- It does not eliminate LockB. LockB still exists and is still
  acquired by id 40333 / id 40334 / id 40285. Other threads can
  still contend on it.
- It does not eliminate LockA. id 19369 still acquires LockA.
- It does not modify ProcessLists state mutation semantics. The
  LockB-protected mutations happen as before, just deferred to
  outside the LockA-holding window.
- It does not address LockC (the third spinlock found in id 35974
  at RVA 0x2973a78). LockC is not on the AB-BA cycle.

## 5. Comparison with C1 / C2b (the previously-leading designs)

| design         | scope                | engine functions modified | semantic risk                              |
| -------------- | -------------------- | ------------------------- | ------------------------------------------ |
| **C5 (this doc)** | defer LockB acquires when LockA is held | 3 inline hooks (id 19369, id 40333, id 40334) | minimal (mutation slightly delayed) |
| C1             | rewrite the LockB acquirers as lock-free atomics | 3 detours (id 40333, id 40334, id 40285) | medium (need to prove bucket-array race-freedom) |
| C2b            | shrink id 19369's LockA scope around 6 vtable dispatches | 1 in-place patch (id 19369) | low-medium (changes lock granularity, breaks LA->LB only) |

C5 wins on every dimension: smallest scope, smallest patch,
smallest semantic risk, no engine-internal data structure
rewrites. C1 + C2b remain queued as fallbacks if C5 turns out
to have an unforeseen behavioural issue in playtesting.

## 6. Phase 4 implementation checklist

- [ ] Add a new `Phase4Defer` module skeleton in
      `skyrim-freeze-fix/src/`.
- [ ] Resolve id 19369, id 40333, id 40334 RVAs at runtime via
      Address Library lookups (use existing `AddressLibrary`
      helper from v1.0.0's `AcquireHook`).
- [ ] Install three inline hooks via safetyhook.
- [ ] Implement the per-thread queue (small `std::vector` is
      fine; calls are rare per LockA window).
- [ ] Add a config knob in `WorkerSpinLockFix.toml`:
      `[Phase4Defer] enable = true | false`.
- [ ] Add diagnostic counters (queued / drained per thread)
      exposed via the existing logging path.
- [ ] Synthetic harness: extend the v0.19.0 AB-BA test to
      exercise the LA->LB direction by faking a call to id 19369
      that subsequently dispatches into id 36016 -> id 40334.
      Verify the queue receives the deferred call and drains
      after id 19369 returns.
- [ ] In-game smoke test on the original freeze-prone scenarios
      (combat, archery, Papyrus-heavy mods).

## 7. Optional Phase 4.2 (only if needed)

If C5 proves correct but the runtime breaker (v1.0.0) still
fires occasionally, that signals the cycle has another entry
point we missed. Investigation candidates:

- Does id 19371 reach id 36016 by a path *other than* via id
  35974? (The +0x249 vtable dispatch at id 19371 +0x249 might
  also reach id 36016 transitively. Worth tracing.)
- Are there other LockA acquirers besides id 19369 that we
  haven't surfaced? The Phase 1 acquirer scan was thorough but
  didn't run on indirect-call sources.
- LockC (id 35974's third spinlock) -- is it on a separate
  cycle with LockA or LockB that we haven't explored?

These are speculative; only investigate if C5 telemetry shows
residual cycles.
