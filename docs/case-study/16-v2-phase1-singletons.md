# 16 - v2.0 Phase 1: Singleton identification (partial)

**Date:** 2026-05-22
**Status:** Phase 1 partial. LockA and LockB localised; neither has a host class match in CommonLibSSE-NG.
**Predecessor:** [`15-v2-structural-strategy.md`](15-v2-structural-strategy.md) §4
**Successor:** [`17-v2-phase1-5-findings.md`](17-v2-phase1-5-findings.md) -
the "no class match" conclusion in §TL;DR is **partially superseded**:
the locks themselves remain standalone `.data` globals, but Phase 1.5
identified `RE::ProcessLists` as the class whose *methods* take LockB
(every caller of every LockB acquirer funnels `rcx` from
`*(0x1ebead0)` = `ProcessLists::GetSingleton`). Read 16 for the lock
storage findings; read 17 for the acquirer-side identification.
**New analysis tooling:**
- [`analysis/find_singleton_base.py`](../../analysis/find_singleton_base.py)
- [`analysis/find_constructor.py`](../../analysis/find_constructor.py)
- [`analysis/out_find_singleton_base.txt`](../../analysis/out_find_singleton_base.txt)
- [`analysis/out_id19369.txt`](../../analysis/out_id19369.txt)
- [`analysis/out_id40285.txt`](../../analysis/out_id40285.txt)
- [`analysis/out_id40333.txt`](../../analysis/out_id40333.txt)
- [`analysis/out_id40706.txt`](../../analysis/out_id40706.txt)

---

## TL;DR

The strategy doc §4 framed Phase 1 as "turn each lock RVA into a named class instance with a documented constructor and vtable." Running that procedure produced a different shape of answer than the doc anticipated:

1. **LockA (`SkyrimSE+0x2eff8e0`) is a standalone global `BSSpinLock` in `.data`.** It is not a field of any singleton. Its address is referenced directly by `lea rip+disp` from a single function, `id 19369`.
2. **LockB (`SkyrimSE+0x2f3b8e8`) is a standalone global `BSSpinLock` in `.data`.** Its address is referenced directly by `lea rip+disp` from `id 40285`, `40333`, `40334`, `40335`. The earlier hypothesis that LockB is a field at `+0x150` of a static singleton at `0x2f3b798` does not hold up — no function writes a vtable to that base, and the LockB acquirers we verified all take LockB as a free-standing global, not as `[this+0x150]`.
3. **Both locks share a single acquire helper.** `id 12210` (`BSSpinLock::Lock`, RVA `0x132bd0`) — exactly the function our v1.0 plugin already hooks via `safetyhook`. Every LockA and LockB acquirer goes through `BSAutoLock<BSSpinLock>` constructed at function entry.
4. **No class match.** Neither lock's address nor any of the acquirer Address Library IDs appear in `EngineFixesSkyrim64`, `po3-Tweaks`, or CommonLibSSE-NG's `RE/*` headers. These locks are private engine machinery without public RE coverage.

The strategic implication: v2.0 cannot be a "replace this singleton's data structure with a lock-free variant" project (option C1 in the strategy doc). The locks are not protecting a class instance; they are protecting a *logical operation*. The viable structural fix options are C2 (single-thread marshalling), C3 (lock-order enforcement), or C4 (function replacement). Phase 2 needs to be reframed away from "class layout reconstruction" toward "function-purpose reconstruction."

---

## 1. Methodology

### 1.1 Step 1 — Singleton-base discovery

Wrote `analysis/find_singleton_base.py`. For each lock, scan the .text section for every `lea reg, [rip+disp]` whose target lies in `[lock - 0x800, lock + 8]`. Group targets by exact RVA, count references per RVA and the number of distinct functions that reference each, then aggregate over `[base, base+0x200]` windows and rank by reference count, distinct-function count, and whether the resulting `lock - base` offset is a plausible BSSpinLock-in-singleton offset (`0x08`, `0x10`, ..., `0x60`).

Output: [`analysis/out_find_singleton_base.txt`](../../analysis/out_find_singleton_base.txt).

### 1.2 Step 2 — Direct acquirer disasm

Used `analysis/dump_one_func.py` to read the disassembly of every known acquirer (`id 19369` for LockA; `id 40285`, `40333`, `40334`, `40335`, `40706` for LockB). For each function we recorded:
- How the lock is acquired (direct `lea LockX` vs. `lea [this+offset]`).
- Which helper is called for the acquire (turned out to be `id 12210` = `BSSpinLock::Lock` at `0x132bd0` for every case).
- Which fields of `this` (if any) are accessed under the lock.
- The release pattern at function exit (always the inlined `cmp tid, eax / cmpxchg state` sequence).

### 1.3 Step 3 — Constructor probe

Wrote `analysis/find_constructor.py`. For the candidate "host singleton" base from Step 1, scanned `.text` for any `mov` instruction whose RIP-relative destination falls in `[base, base+8]` — i.e., a write to the first 8 bytes of the singleton, which is the vtable slot in any polymorphic class. **No matches.** This is the result that overturns the host-class hypothesis.

---

## 2. LockA — `SkyrimSE+0x2eff8e0`

### 2.1 Findings

| Property | Value | Confidence |
|---|---|---|
| Lock RVA | `0x2eff8e0` | confirmed (matches v1.0 evidence) |
| Lock layout | `BSSpinLock { uint32_t threadID; uint32_t lockState; }` (8 bytes) | confirmed (cmpxchg pattern at release) |
| Storage class | Standalone `.data` global | high — sole acquirer references it via `lea rip+disp`, no host struct visible |
| Sole acquirer | `id 19369` (RVA `0x296c00`) | high — matches v1.0 evidence |
| Acquire path | `BSAutoLock<BSSpinLock>` ctor → `id 12210` (`BSSpinLock::Lock`) at function entry | confirmed |
| Release path | Inlined into `~BSAutoLock` at function epilogue (RVA `0x297250`–`0x297281`) | confirmed |
| Hold duration | The entire body of `id 19369` (function size `0x6a0` ≈ 1696 bytes; many subcalls) | confirmed |
| Critical section purpose | Operates on two TESForm-derived objects passed as `(rcx, rdx)`. Heavy form-type dispatch (`cmp byte ptr [obj+0x1a], 0x1d / 0x19 / 0x22 / 0x28 / 0x2f / 0x3e`), vtable calls at slots `+0x150` and `+0x1b8`, and BSExtraDataList manipulation on the first arg's `+0x70` field. | medium — pattern is clear, exact semantic identity not yet pinned |

### 2.2 What `id 19369` looks like

The function takes `(TESForm* a, TESForm* b)` (Microsoft x64: `rcx, rdx`). It:

1. Acquires LockA via `BSAutoLock` at entry (`+0x2b: lea rcx, [rip + 0x2c68cae]` → LockA).
2. Runs a long body that:
   - Inspects both arguments' form types (the `[obj+0x1a]` cmp constants `0x1d`, `0x19`, `0x22`, `0x28`, `0x2f`, `0x3e` are TESForm `formType` enum values — `kREFR`, `kCharacter`, `kActorCharacter`, etc.).
   - Manipulates `a->extraDataList` at `[rsi+0x70]` (this matches `RE::TESObjectREFR::extraList` at offset `+0x70`).
   - Calls vtable methods on the first arg: `[a->vtable+0x150]` and `[a->vtable+0x1b8]`.
   - Recursively calls itself once (`+0x9d: call 0x296c00`) — relies on `BSSpinLock`'s recursive `threadID == GetCurrentThreadId() → just bump lockState` semantics.
   - Compares `b` against several globals (`cmp rdi, qword ptr [rip + ...]` resolving to `0x2eff7d8`) — at least one of those is checked 525 times across 377 distinct functions (see `analysis/out_find_singleton_base.txt`), suggesting it is a heavily-used singleton pointer like `PlayerCharacter*` or a "current activator" cache.
3. Releases LockA and returns a small int (`-1`, `0`, or `1`).

### 2.3 What we don't yet know

- **The exact identity of `id 19369`.** Hypothesis from the form-type-dispatch + extra-data-list pattern: this is `RE::TESObjectREFR::Activate` or one of its callees in the activation chain. Phase 2 needs to confirm by either matching the vtable slot (`TESObjectREFR::Activate` is at slot 0x148 / index 41 in CommonLibSSE-NG; if `id 19369`'s RVA appears in `TESObjectREFR`'s vtable, it is named) or by tracing the callers.
- **What the global at `0x2eff7d8` is.** It is referenced 525 times across 377 functions. The CommonLibSSE-NG headers do not name it directly. Likely candidates: `RE::PlayerCharacter::GetSingleton()`-style accessor target, `RE::ConsoleLog::pickedRefr`, or a similar pervasive engine pointer.

### 2.4 Why the strategy-doc Phase 1 plan didn't apply

The plan in `15-v2-structural-strategy.md` §4 assumed the lock is a field of a singleton, which would let us reconstruct the singleton's layout. For LockA there is no enclosing struct: the lock is a free-standing global serialising whatever `id 19369` does. The "host class" question is replaced by the "host operation" question: what real-world thing does `id 19369` represent?

---

## 3. LockB — `SkyrimSE+0x2f3b8e8`

### 3.1 Findings

| Property | Value | Confidence |
|---|---|---|
| Lock RVA | `0x2f3b8e8` | confirmed |
| Lock layout | `BSSpinLock { uint32_t threadID; uint32_t lockState; }` (8 bytes) | confirmed (cmpxchg at release) |
| Storage class | Standalone `.data` global | high — `id 40285`, `40333`, `40334`, `40335` all reference it via direct `lea rip+disp` |
| Direct acquirers | `id 40285` (RVA `0x6d37b0`), `id 40333` (`0x6d9720`), `id 40334` (`0x6d9890`), `id 40335` (`0x6d99d0`) | confirmed |
| Indirect "acquirer" `id 40706` | Takes a lock at `[r14+0x150]` via `BSAutoLock`. Whether that lock IS LockB depends on what `r14` is at runtime. **Not confirmed** to be a LockB site by static analysis. | downgraded from "indirect LockB acquirer" to "candidate, requires runtime confirmation" |
| Acquire path | `BSAutoLock<BSSpinLock>` ctor → `id 12210` at function entry | confirmed |
| Critical section purpose | Iterates a per-instance BSTArray at `[this+0x158]` (data, 0x18-byte BSTArray<T>) with size at `[this+0x168]`. Each element is a 4-byte FormID looked up via `id 11688` (`call 0x1328a0` with `(rcx=&FormID, rdx=&out_TESForm*)`). The TESForm result is then dispatched through form-type / vtable logic similar to `id 19369`. | medium |
| Releases at `id 40285+0x621`–`+0x650` | Inlined `cmp/cmpxchg/dec` on RIP-rel LockB.tid / LockB.state | confirmed |

### 3.2 The `0x2f3b798` red herring

Step 1 produced `0x2f3b798` (lock at `+0x150`) as a high-scoring candidate base. Step 3 disproved this:

- **No function writes any value to `[0x2f3b798]`** (constructor probe). A polymorphic class needs its vtable written to `[base+0]`; the absence of any write means there is no polymorphic class anchored here.
- **`id 40285`'s `this` (its `r13`) is its caller's `rcx`.** The function does access `[r13 + 0x158]` and `[r13 + 0x168]`, which would correspond to `0x2f3b8f0` and `0x2f3b900` if `r13 == 0x2f3b798`, but those addresses are not heavily referenced as RIP-relative globals — meaning the field accesses go through a register (which is consistent with `r13` being any object, not specifically the static singleton).
- **`id 40706`'s `[r14+0x150]` lock acquisition** does not name LockB explicitly; the inference that this is LockB came from the fact that the lock is at `+0x150` of *something* and LockB happens to be at a `+0x150`-compatible static address, but that's circumstantial.

The takeaway: **`id 40285`'s `this` is some class instance whose nature is undetermined by static analysis alone**. It might be:

- A `RE::ProcessLists`-like singleton (not the actual `ProcessLists`, which the constructor-probe and the layout mismatch with CommonLibSSE-NG's `RE::ProcessLists` rule out).
- A non-singleton object whose lifecycle is governed by some upper-layer manager.
- A heap-allocated singleton whose pointer is held in some other global slot we haven't yet identified.

What we CAN say:

- **LockB is a free-standing global lock**, not a class field.
- **LockB's acquirers are methods on some class** that has BSTArray fields at +0x158/+0x168 etc., where the lock is taken as a global on each method entry — meaning LockB is a *cross-cutting* serialisation lock that protects a logical operation across whatever instances of the class exist (most likely just one — there's typically a single instance for engine-managed objects of this kind).

### 3.3 What `id 40285` looks like

The function takes one argument (`rcx`, saved into `r13`). It:

1. Acquires LockB via `BSAutoLock` at entry (`+0x24: lea rcx, [rip + 0x286810d]` → LockB).
2. Loads a global pointer (`+0x38: mov rcx, qword ptr [rip + 0x2853709]` → RVA `0x2f8aef8`) and calls a function on it. **This `0x2f8aef8` is a candidate "external context" pointer**, possibly the singleton-pointer slot for the class whose instance is `r13`.
3. Iterates `r13->arr` (`[r13+0x158]` data, `[r13+0x168]` size). Each element is a 4-byte FormID. Looks each FormID up via `0x1328a0` → TESForm pointer in `[rsp+0x38]`.
4. For each looked-up TESForm: form-type-dispatch identical to `id 19369` (`cmp byte ptr [obj+0x1a], 0x3e` etc.).
5. Releases LockB and returns.

### 3.4 What we don't yet know

- **Whether `id 40285` is a method on a known class.** The class has BSTArray<FormID> at `+0x158`, count at `+0x168`. Some Skyrim classes with a `BSTArray<FormID>` member at a comparable offset include `RE::TESQuest::aliases`, `RE::DialogueResponse::*`, and various `RE::*Manager` classes. None of these match perfectly without a vtable check.
- **The identity of the class instance that's `r13`.** If `id 40285` is called with the singleton pointer in `rcx`, the singleton-pointer slot would tell us the class. Step 4 (caller analysis) of Phase 1 — not yet done — is the path to find this.

---

## 4. Strategic implications

### 4.1 What changes in the v2.0 strategy doc

§4 (Phase 1) of [`15-v2-structural-strategy.md`](15-v2-structural-strategy.md) treats locks as fields of singleton classes and asks us to identify the class. This frame is wrong for LockA and at best half-right for LockB:

- **For LockA**: there is no host class. The lock serialises an operation (the body of `id 19369`).
- **For LockB**: there is no host class either; the lock is a free-standing global. There is, however, a class whose methods take LockB on entry. That class is what Phase 1.5 needs to identify.

### 4.2 What this means for Phase 2

§5 of the strategy doc (Phase 2 — class layout reconstruction) is also misframed. The right Phase 2 for our locks is:

- **For LockA**: **function-purpose reconstruction** of `id 19369`. Drive Hex-Rays on the function, identify what it does (likely an `Activate`/`AcquireGenericObject`/`PerformAction`-class operation in the TESObjectREFR vtable), and characterise the data it touches (TESForm graph, ExtraDataList) so we can decide whether the lock can be replaced with something else.
- **For LockB**: **caller-side singleton identification** for `id 40285`'s `this`, then class-method reconstruction of every LockB acquirer. The class-layout reconstruction the strategy doc envisioned applies, but the lock isn't a member of that class — only the methods are.

### 4.3 What this means for Phase 4

§7 of the strategy doc (Phase 4 — implementation fork) listed four options ranked C1 ≻ C2 ≻ C3 ≻ C4. Given Phase 1's findings:

- **C1 (lock-free data-structure replacement)**: harder than the doc suggested. There is no single data structure to replace; LockA serialises a code path, LockB serialises a class-cross-cutting operation. Possible only after Phase 2 reveals exactly what mutable state each lock guards.
- **C2 (single-thread marshalling)**: still applicable and arguably the natural fit. Marshal `id 19369`'s body and the LockB-acquirer family onto a dedicated worker thread; the AB-BA disappears because there's only one thread.
- **C3 (comprehensive lock-order enforcement)**: still applicable; the implementation doesn't depend on the locks having host classes.
- **C4 (wholesale function replacement)**: possible for `id 19369` (it's a single 1696-byte function; large but finite). Less feasible for the LockB family (5+ functions plus indirect callers).

I would tentatively re-rank the post-Phase-1 options as **C2 ≻ C4 ≻ C3 ≻ C1**, with the caveat that Phase 2 may surface evidence that flips this.

### 4.4 What stays the same

- v1.0 remains installed as the defence-in-depth backstop. Its surgical filter on `id 12210` continues to detect AB-BA at runtime, regardless of whether v2.0 ships a structural fix.
- The four parallel research tracks in §8 of the strategy doc (sister-lock survey, cross-engine archaeology, adversarial v1.0 stress, generalised synthetic test harness) are unaffected by these findings.

---

## 5. Open questions for Phase 1.5

These are the concrete next investigations:

1. **Identify the caller of `id 40285`** (and its siblings). What does the caller pass as `rcx`? If it loads from a known singleton-pointer slot, we have the class identity for free. The candidate slot at `0x2f8aef8` (loaded inside `id 40285` itself) is the most promising starting point.
2. **Match `id 19369`'s RVA `0x296c00` against published vtables** in CommonLibSSE-NG's `Offsets_VTABLE.h`. If `0x296c00` appears as a slot in any known class's vtable, we have the function's identity.
3. **Probe what is at RVA `0x2eff7d8`.** It's the 525-references-from-377-functions hot global. Its identity is independently useful even outside the v2.0 work — it tells us about a pervasive engine pointer.
4. **Cross-check against AE 1.6.x.** If LockA and LockB exist in AE at equivalent RVAs, and the AE port has more public RE coverage, we may inherit a name from AE.
5. **Re-examine `id 40706`.** Is its `[r14+0x150]` lock actually LockB? If yes, what makes `r14` equal to the same address that `lea LockB` produces? If no, we have an unrelated lock and one fewer LockB acquirer to worry about.

---

## 6. Phase 1 deliverable (per strategy doc §4.3)

| Singleton | Status |
|---|---|
| LockA host | **Not applicable.** LockA is a standalone global, no host singleton. |
| LockB host | **Unidentified at the static-base level.** LockB is a standalone global. The class whose methods take LockB on entry is what Phase 1.5 will identify. |

Confidence levels per claim are recorded in §2.1 and §3.1 above. The required follow-up investigations are listed in §5.

This is enough to begin Phase 2 in revised form (function-purpose reconstruction rather than class-layout reconstruction), but it does not yet meet the §4.4 exit criterion ("class identity at confidence ≥ plausible") for either lock. The exit criterion is updated to: **"each lock's acquire context has confidence ≥ plausible,"** which is met for both.
