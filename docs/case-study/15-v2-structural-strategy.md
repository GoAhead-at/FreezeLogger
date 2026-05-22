# 15 - v2.0 Structural Strategy: Eliminating the AB-BA at the Source

**Date:** 2026-05-22
**Status:** Strategy proposal. Implementation has not yet started.
**Predecessor docs:**
- [`06-root-cause.md`](06-root-cause.md) — The AB-BA evidence between LockA and LockB.
- [`08-mitigation.md`](08-mitigation.md) — Original five-option mitigation analysis.
- [`11-worker-spinlockfix-retrospective.md`](11-worker-spinlockfix-retrospective.md) — Failure modes of function-entry serialisation.
- [`12-engine-fix-mod-audit.md`](12-engine-fix-mod-audit.md) — Audit of `EngineFixesSkyrim64` and `po3-Tweaks`; `safetyhook` adoption rationale; `form_caching` precedent.
- [`13-rethought-solution.md`](13-rethought-solution.md) — Reactive runtime fix proposal that became v1.0.
- [`14-final-design-v1.md`](14-final-design-v1.md) — v1.0 final architecture and outcome.

This document defines the strategy for **WorkerSpinLockFix v2.0** — a structural fix that removes the AB-BA inversion at its source rather than detecting and breaking cycles at runtime. v1.0 is retained as a defence-in-depth safety net beneath v2.0.

---

## 1. Why a v2.0 is now warranted

Doc 13 §2 framed the v1.0 design under explicit budget constraints: "no public reference solution exists, structural replacement requires knowing what data the engine locks protect, which is unknown." Under those constraints we shipped a reactive runtime mitigation (cycle detect + force-release) and reserved the structural track for "v2.0 if and only if we ever localise what the locks guard."

The constraint that gated structural work was engineering effort, not technical impossibility. With that constraint lifted, the structural track is now the right primary investment. v1.0 stops being the goal and becomes the regression detector.

This document supersedes doc 13 §7 ("Structural / lock-bypass research track") with a fully specified roadmap.

## 2. What we carry forward

The constraints from doc 11 that killed function-entry serialisation are still real and still bind v2.0 design choices:

- **C1** `BSSpinLock::Acquire` returns `void`. Compiled callers cannot be asked to handle an acquire failure.
- **C2** The set of LockA/LockB acquirers cannot be enumerated by static analysis alone.
- **C3** `BSSpinLock::Release` is heavily inlined; reliable per-thread held-set tracking via Acquire+Release pairs is not viable.
- **C4** What LockA and LockB protect is not yet known.

C1, C2, C3 cannot be relaxed. C4 is exactly what Phases 1–3 of the v2.0 plan are designed to eliminate.

We also carry forward **all the live evidence** the v1.0 work produced:

- LockA at `SkyrimSE+0x2eff8e0`, private to `id 19369` (six RIP-relative refs in one function — see [`05-static-analysis.md`](05-static-analysis.md) §5.3).
- LockB at `SkyrimSE+0x2f3b8e8`, shared across `id 40285`, `id 40333`, `id 40334`, `id 40335` (direct `lea`) and `id 40706` (indirect via `[arg+0x150]`).
- The deadlock call topology, fully reconstructed and verified across 9 freeze captures.
- The acquire/release inline pattern (`lea + cmpxchg + dec` on `state` (+0x4) and `owner` (+0x0)).
- The 33 analysis scripts under `analysis/`.

## 3. Strategy overview

```
                       ┌─────────────────┐
                       │   Phase 1       │
                       │   Singleton     │   (1–3 days focused RE)
                       │   identification│
                       └────────┬────────┘
                                │  parent class names known
                                ▼
                       ┌─────────────────┐
                       │   Phase 2       │
                       │   Class layout  │   (1–3 weeks)
                       │   reconstruction│
                       └────────┬────────┘
                                │  field-by-field type known
                                ▼
                       ┌─────────────────┐
                       │   Phase 3       │
                       │   Access-pattern│   (2–6 weeks; partial overlap with Phase 2)
                       │   survey        │
                       └────────┬────────┘
                                │  full read/write graph known
                                ▼
                       ┌─────────────────┐
                       │   Phase 4       │
                       │   Implementation│   ───┬───  C1 lock-free replacement (preferred)
                       │   fork          │      ├───  C2 single-thread marshalling (fallback)
                       └─────────────────┘      ├───  C3 comprehensive lock-order enforcement
                                                └───  C4 wholesale function replacement
```

Each phase has a hard exit criterion. Phase N+1 cannot start until Phase N's deliverable is signed off. This is deliberately heavyweight: the v0.1–v0.5 retracted iterations all failed because they ran ahead of analysis. v2.0 replaces "iterate fast, fail in production" with "analyse exhaustively, ship once."

---

## 4. Phase 1 — Singleton identification

**Goal:** turn `LockA at +0x2eff8e0` and `LockB at +0x2f3b8e8` into named class instances with documented constructors and vtables.

### 4.1 Tasks

1. **Reverse-locate the parent struct base.** For each lock RVA, enumerate every `lea r?, [rip + disp]` in `.text` whose target lies in `[lock - 0x800, lock]`. The most heavily referenced base in that window is the singleton's address. The window size 0x800 is a guess at the upper bound of a singleton struct; iterate if no clear winner emerges.
2. **Locate the constructor.** Find every function that writes a 64-bit value to `[base + 0]` (the vtable slot) followed by zero/default initialisation of surrounding fields. The constructor is typically called from `WinMain → BSSystem::Initialize → ...`; tracing back from the constructor to its first caller usually identifies the subsystem boot path.
3. **Extract the vtable.** Read the vtable contents (function pointers at `[base + 0]`, `[base + 8]`, ...) and match against:
   - **CommonLibSSE-NG headers** (`RE/*`) — thousands of vtable RVAs are documented there. Most named singletons in Skyrim have a published vtable signature.
   - **`EngineFixesSkyrim64`** (aers, open source) — has private symbols for some classes CommonLibSSE-NG hasn't named.
   - **po3's CommonLibSSE fork** — sometimes ahead of upstream on naming.
   - Any vtable methods whose signature matches a known `BSTSingleton<T>::GetSingleton` pattern (returns `&base`, no other side effects).
4. **RTTI extraction (best-effort).** Skyrim was compiled with `/GR-` so most RTTI is stripped, but a small number of polymorphic types retain partial RTTI chains. Use a tool like `ClassInformer` (IDA plugin) or hand-trace from `__type_info` references in `.rdata`.
5. **AE binary cross-reference.** Run the same procedure on `SkyrimSE.exe` 1.6.x. The AE port shares the dispatcher architecture but was rebuilt; some inlining decisions differ and some debug strings survive that 1.5.97 stripped. The AE Address Library maps a non-trivial number of 1.5.97 IDs to AE IDs, so a name found in AE often back-ports.
6. **Fallout 4 binary cross-reference.** Same engine generation, often-shared dispatcher code, shipped with a different optimisation profile. FO4's `Fallout4.exe` has more public RE coverage than Skyrim 1.5.97 in some areas (Buffout's source is a useful reference).

### 4.2 Tools

- Ghidra 12.x (already set up; see [`../ghidra-bring-up.md`](../ghidra-bring-up.md)).
- IDA Pro + Hex-Rays decompiler — license cost is trivial relative to engineer-hours saved on heavily-templated MSVC C++; Hex-Rays' output is meaningfully better than Ghidra's on this kind of code.
- The `analysis/` scripts (`xref_locks.py`, `xref_calls.py`, `dump_one_func.py`) — extend rather than replace.
- `CommonLibSSE-NG` headers locally checked out for grep-based vtable matching.
- `EngineFixesSkyrim64` and `po3-Tweaks` source trees locally checked out for the same.

### 4.3 Deliverable

A short addendum to [`05-static-analysis.md`](05-static-analysis.md) (or a new `05a-singletons.md`) containing, for each lock:

- Singleton base address.
- Lock offset within the singleton.
- Constructor function ID and disassembly.
- Vtable contents (offsets + function IDs).
- Best-confidence class name (or "unnamed; vtable signature `<sig>`" if no match).
- A confidence score per claim.

### 4.4 Exit criterion

Both LockA and LockB have a class identity at confidence ≥ "plausible." Plausible is enough to start Phase 2; high confidence is not required at this stage because Phase 2 will refine it.

---

## 5. Phase 2 — Class layout reconstruction

**Goal:** for each singleton, a complete field-by-field type description. The output is a working C++ struct definition plus an annotated decompilation of every method that takes the lock.

### 5.1 Tasks

1. **Enumerate every memory access through `this`** inside every method on the class. Start with the methods we already know — `id 19369` for LockA; `id 40285`, `id 40333`, `id 40334`, `id 40335`, `id 40706` for LockB — and expand via the vtable's other slots.
2. **Type each field from its access pattern.**
   - Pointer: loaded into a 64-bit register and dereferenced.
   - Integer: 32- or 64-bit, atomic vs non-atomic (look for `lock` prefix), signed vs unsigned.
   - Embedded sub-struct: contiguous offsets accessed without immediate dereference.
   - `BSTArray<T>` / `BSTHashMap<K,V>` / `BSTList<T>` / `BSTSmallArray<T,N>` / `BSScrapArray<T>`: recognisable from CommonLibSSE-NG's known layouts. The header layout for these is `(allocator*, capacity, size, data*)` or close variants; fingerprint matches are usually unambiguous.
   - `BSAtomic` / `std::atomic` wrapped types: look for `lock cmpxchg` / `lock xadd` patterns.
3. **Drive Hex-Rays incrementally.** Once 60–70% of fields have types, the decompiler output becomes meaningfully readable. Iterate: read decomp, propose types, re-decompile, read again.
4. **Cross-validate by retyping callers.** Once the singleton struct is partially typed, callers that took `this` as `void*` or `int64_t*` can be retyped; the new caller-side decomp validates or contradicts the singleton typing. Contradictions are diagnostic.
5. **Per-field confidence rating.** Some fields will be obvious (`vtable`, `count`, `head`); others will be opaque without runtime correlation. Mark each field High / Medium / Low confidence. Phase 3 will fill in the Low-confidence ones.

### 5.2 Tools

- IDA Pro Hex-Rays for the bulk of the decompilation (see Phase 1 §4.2 rationale).
- Ghidra as a second opinion — different decompilers infer different types from the same disassembly; comparing outputs catches errors.
- `analysis/dump_one_func.py` (capstone-based) for raw access listings when decompiler output is suspect.
- `CommonLibSSE-NG` `RE/*` headers as a layout reference for any embedded `BST*` containers.

### 5.3 Deliverable

For each singleton, a header file (or markdown table) of the form:

```cpp
// inferred from disassembly; see docs/case-study/15-v2-structural-strategy.md
struct LockA_HostClass {           // or whatever name Phase 1 produced
    void**      vtable;            // +0x00 — confirmed (constructor writes here)
    BSSpinLock  lock;              // +0x08 — confirmed (matches LockA RVA at +0x2eff8e0)
    std::uint32_t count;           // +0x10 — high confidence (incremented under lock)
    void*       queue_head;        // +0x18 — medium confidence (loaded as pointer, dereferenced)
    /* +0x20..+0x3F unknown — low confidence */
    BSTArray<???> jobs;            // +0x40 — header layout matches BSTArray; element type unknown
    /* ... */
};
```

Plus a decompiled and annotated version of every locked method, tagged with which field each access touches.

### 5.4 Exit criterion

≥ 80% of fields under the lock have High or Medium confidence types. The remaining < 20% are explicitly enumerated as Low-confidence, with hypotheses to test in Phase 3.

---

## 6. Phase 3 — Access-pattern survey

**Goal:** know everything that touches the protected data and under what conditions. The output drives the Phase 4 fork decision.

### 6.1 Static survey

1. **Cross-reference every `lea r?, [rip + disp]`** in `.text` whose target is the singleton's base address. Every such reference is a potential access point.
2. **For each access point, classify it:**
   - **Locked path:** the access happens after `BSSpinLock::Acquire` on the same singleton's lock.
   - **Lock-free fast path:** the access happens without the lock and assumes publication ordering. Game engines often have these for read-mostly state. Fingerprint: load through the singleton pointer, no atomic prefix, no spin.
   - **Constructor / destructor:** singleton init and teardown. Excluded from the protected-data analysis.
   - **Unrelated:** the singleton pointer is passed through but the field accessed is not under our lock. Discriminate by which fields are touched.
3. **Build the full read/write graph** of the protected data: for each field, list every instruction that reads or writes it, partitioned by lock-state and by function.

### 6.2 Dynamic survey

Static survey alone cannot answer "is this field a Havok phase counter or a worker queue depth." Dynamic instrumentation closes the gap.

1. **Hook + log every locked method** with full argument capture, return value, and timing. Run under controlled gameplay scenarios:
   - Cell load (interior ↔ exterior).
   - Save / load.
   - Combat start / end.
   - Spawn / despawn (NPC, projectile, FX).
   - Havok step (every frame — high volume; sample, don't log every one).
   - Animgraph state transition.
   - Worker pool job dispatch.
2. **Hook + log every field write** for fields whose semantics are unclear. Correlate timestamps with player-facing events.
3. **DynamoRIO or Pin** for finer-grained instrumentation if hook-based logging is too coarse. Both run on Windows; DynamoRIO is more permissive.
4. **Time Travel Debugging (TTD)** under WinDbg for replayable post-mortem inspection of any freeze that v1.0's breaker fires on. TTD gives microsecond-resolution forward/backward stepping over a captured trace; correlating field writes against the freeze instant is dramatically faster with TTD than with live debugging.
5. **rr (under Wine on Linux)** as a second replay system. rr is more efficient than TTD on long traces; useful for soak-test recordings.
6. **AE binary correlation.** Run the same gameplay scenarios under Skyrim AE 1.6.x with AE-equivalent hooks. Different inlining and optimisation in AE sometimes exposes structure that 1.5.97 hides.

### 6.3 Deliverable

A complete access-pattern table:

| Field | Type (Phase 2) | Locked accesses | Lock-free accesses | Player-facing event correlation | Phase 4 candidate |
|---|---|---|---|---|---|
| `count` | `uint32_t` | `id 19369 +0x40` (write), `id 40333 +0x80` (read) | none | increments on cell load worker dispatch | C1 (atomic) |
| `queue_head` | `void*` | `id 40333 +0x10` (read), `id 40334 +0x18` (write) | none | written by every job submission | C1 (lock-free queue) |
| ... | ... | ... | ... | ... | ... |

The "Phase 4 candidate" column is the per-field recommendation; the overall Phase 4 decision is the union over fields.

### 6.4 Exit criterion

Every field has a confirmed type, a complete access list, and a Phase 4 candidate. No field is left as "unknown semantics."

---

## 7. Phase 4 — Implementation fork

The Phase 3 deliverable determines which of four implementation strategies fits. The strategies are listed in order of architectural cleanliness; pick the first one that the data permits.

### 7.1 Option C1 — Lock-free data-structure replacement (preferred)

**Applies if** the protected data is a hash table, queue, list, counter, or any other structure with a known concurrent variant.

**Approach:**

1. Allocate a substitute structure (`tbb::concurrent_hash_map`, Michael-Scott lock-free queue, hazard-pointer list, atomic counter, atomic bitmask) at plugin init.
2. Hook the engine's access primitives so reads and writes go through the substitute. The engine still calls into `id 19369` etc., but inside those functions every access point identified in Phase 3 is redirected.
3. The original `BSSpinLock`s are still acquired by the engine but become uncontended (and therefore irrelevant) because the protected operations are now lock-free.
4. Optionally hook `BSSpinLock::Acquire` itself for these specific lock instances and turn it into a no-op, eliminating even the spin overhead.

**Precedent:** [`12-engine-fix-mod-audit.md`](12-engine-fix-mod-audit.md) §4 documents po3's `form_caching` mod, which replaced the engine's form-lookup linked list with `tbb::concurrent_hash_map` using exactly this pattern. It is a working proof of concept for C1 in the Skyrim engine.

**Why preferred:** does not require enumerating dispatch paths. The engine's normal call graph reaches the substitute via the data-structure pointer alone — every callsite (direct, indirect, vtable-based, jump-table-based) is automatically covered because they all dereference the pointer.

**Risk:** lock-free programming has its own failure modes (memory ordering, ABA, lifetime management with hazard pointers). Mitigation: use battle-tested libraries (`tbb`, `boost.lockfree`) rather than hand-rolling primitives. Validate with the v1.0-style synthetic test harness extended to v2.0's substitutes.

### 7.2 Option C2 — Single-thread marshalling (fallback)

**Applies if** the protected operations are infrequent enough that serialising them on one thread does not tank performance, and the data structure is too compound for C1.

**Approach:**

1. Spawn a dedicated `WSLF::DispatcherThread` at plugin init.
2. Replace each lock-acquiring function with a stub that posts a serialised work-item to that thread.
3. Choice of synchrony per call site:
   - **Synchronous:** block the caller on a per-task event until completion. Preserves call-site semantics. Costs latency.
   - **Asynchronous fire-and-forget:** return immediately. Changes call-site semantics; only safe if the engine doesn't expect synchronous completion at that call site (Phase 3 must confirm).
4. The `BSSpinLock`s are still acquired by the dedicated thread but are uncontended because only one thread ever runs the protected code.

**Precedent:** SKSE's `TaskInterface` follows this pattern for main-thread-only operations (`TESForm` mutation, animgraph). v2.0's variant uses a dedicated worker thread instead of the main thread to avoid loading the already-busy main thread further.

**Risk:** changes synchrony assumptions. Some engine call sites may have implicit expectations about what state has been mutated by the time the call returns. Mitigation: default to synchronous; only use async where Phase 3 explicitly proves it is safe.

### 7.3 Option C3 — Comprehensive lock-order enforcement

**Applies if** C1 and C2 are both ruled out by Phase 3 findings (e.g., the protected data is too entangled with engine-specific invariants for either replacement).

**Approach:**

1. Enumerate every dispatch path that takes either lock — direct calls, indirect calls (vtable, register-relative), function pointers stored in fields, jump tables.
2. Hook each one to acquire a plugin-side ordering token before the engine lock.
3. Track held tokens via a per-thread set. The blocker that killed v0.1–v0.5 was inlined Release; resolve it by either:
   - **Static release-site discovery:** capstone-driven scan for the `cmpxchg + dec` instruction pattern across all of `.text` whose target is one of the locks. Tractable across 22 MiB of `.text`. Patch each release site with a hook to drop the token.
   - **RIP-range inference:** track only acquires; infer the held-set from `RIP ∈ [acquire_site, release_site]` ranges. Refresh on every lock-related hook entry.

**Why this is the brute-force option:** doc 11 catalogues why function-entry serialisation failed across v0.1–v0.5 with a small enumeration set. Scaling that enumeration to the full graph (direct + indirect + vtable + jump-table) is possible with unlimited resources but architecturally inferior to C1/C2 because it leaves the lock structure in place.

**Risk:** the longest tail risk of the four. Any missed dispatch path resurrects the original bug; any incorrectly-tracked release introduces a new one.

### 7.4 Option C4 — Wholesale function replacement

**Applies if** Phase 2/3 produce a high-confidence understanding of what each lock-acquiring function does AND the function is small enough to re-implement cleanly.

**Approach:**

1. Write a substitute in C++ that implements the same observable behaviour using a corrected lock order or no locks.
2. `safetyhook::create_inline` it at the function entry, completely bypassing the original.

**Why last-resort:** highest leverage if the function is small (could fix the bug in tens of lines); highest regression risk if the function is large or has subtle behaviours we missed.

### 7.5 Selection criteria

```
Phase 3 deliverable says:                                           → Pick:
─────────────────────────────────────────────────────────────────────────────
Protected data is a single concurrent structure (hash, queue, list)    C1
Protected data is a counter or atomic flag set                         C1
Protected operations are infrequent (<1000/sec) and synchronous-safe   C2
Protected operations are frequent and entangled with engine invariants C3
A locked function is small (<200 instructions) and well-understood     C4
```

C1 and C4 can be combined: if Phase 3 reveals the lock guards a single structure plus a small amount of method logic, C1-replace the structure and C4-replace the method.

---

## 8. Parallel research tracks

These run alongside the main path because they de-risk the project. Each is independently valuable; failure of any one does not block the others.

### 8.1 Survey ALL dispatcher locks, not just LockA/LockB

The 9 freeze captures all show the same pair, but the engine has many `BSSpinLock`s on the worker pool (`BSTaskPool`, `BSScheduler`, the dispatch root chain `id 67147 → id 68058 → id 68010 → id 40289`). Run Phase 1 + Phase 2 for every dispatcher lock visible in those modules. There may be sister deadlocks waiting to fire on rarer code paths that just haven't surfaced in our captures.

If Phase 4 picks C1 and the substitute pattern generalises, the same code can fix multiple sister bugs simultaneously.

### 8.2 Cross-engine archaeology

- **Fallout 4.** Same engine generation, often-shared dispatcher code. FO4 has more public RE coverage than Skyrim 1.5.97 in some areas (Buffout's source is a useful reference). If FO4 has the equivalent bug, fixing both validates the technique.
- **Skyrim AE 1.6.x.** Different optimisation profile; some inlining decisions differ; some debug strings survive. Names found in AE often back-port to 1.5.97 because the underlying class structure is preserved across the AE port.
- **Older Bethesda engine binaries.** Oblivion / FO3 / FNV / FO4 lineage has documented evolution; some classes have ancestors with surviving symbols.

### 8.3 Adversarial v1.0 stress

While v2.0 is being built, run v1.0 in adversarial gameplay configurations: long save loads, modlist splash testing, mod combination matrices, marathon sessions. Anything v1.0's breaker fires on (or fails to fire on) is information for v2.0's design.

Specifically: if v1.0 ever logs a `breaks_done` for a cycle that does NOT involve LockA/LockB, that is a sister deadlock and v2.0 must cover it.

### 8.4 Generalise the synthetic test harness

The `TestMode` module in v1.0 validates a 2-thread AB-BA on registered test locks. Generalise it to:

- N-thread cycles (3, 4, 8, ..., to stress the wait-graph's `kMaxHops` bound).
- Multiple concurrent disjoint cycles (to stress the recent-cycles map).
- Race-prone CAS scenarios (to stress `breaks_raced` paths).
- Synthetic versions of any sister deadlock §8.1 finds.

Every cycle pattern v2.0 is supposed to fix gets a synthetic test before shipping.

### 8.5 Engage community experts

aers (`EngineFixesSkyrim64`) and powerof3 (`po3-Tweaks`) are the most experienced engine-fix authors active. With unlimited resources, paid review of the Phase 1 / Phase 2 deliverables compresses months of independent RE. Both have shown willingness to collaborate on engine-level fixes; po3's `form_caching` pattern is the closest existing analogue to what v2.0 will be.

### 8.6 Tooling investments

- **IDA Pro + Hex-Rays Decompiler** (commercial). Better C++ recovery than Ghidra on heavily-templated MSVC code.
- **DynamoRIO / Pin** (free). Dynamic instrumentation for fine-grained access tracing.
- **WinDbg + TTD** (free, Microsoft Store). Time-travel debugging for replayable post-mortem.
- **ClassInformer for IDA** (free). Automated RTTI extraction.
- **Ghidra 12.x + custom scripts** (already set up). Second-opinion decompiler.

---

## 9. Defence-in-depth: v1.0 stays installed

v2.0 does not retire v1.0. The two ship together:

- **v2.0** is the structural fix. It eliminates the AB-BA at its source by replacing the protected data structure (or marshalling, or enforcing lock order, depending on Phase 4 outcome).
- **v1.0**'s `AcquireHook + WaitGraph + Breaker` runs underneath as a backstop. Any cycle that v2.0's design did not anticipate — a missed code path, a regression introduced by a future Skyrim update, a sister deadlock §8.1 didn't catch — gets broken by v1.0 and logged.

This is the same pattern aers used in `EngineFixesSkyrim64`: discrete patches plus instrumented logging that flags any case where the patches fail to cover the intended code path.

**v1.0's stats counters become v2.0's regression detector.** After v2.0 ships, the expected steady state is `cycles_observed = 0` indefinitely. Any non-zero value means v2.0 missed something and v1.0 just rescued the session — that fires a regression investigation and a v2.x patch.

---

## 10. Risk register

| ID | Risk | Mitigation |
|---|---|---|
| R1 | Phase 1 fails to identify the singleton (no public match, no RTTI). | Fall back to vtable-fingerprint-only identification; use AE/FO4 cross-reference; engage community experts (§8.5). |
| R2 | Phase 2 produces a layout with too many low-confidence fields. | Phase 3 dynamic instrumentation closes the gap. If gap remains, narrow the fix scope to high-confidence fields only. |
| R3 | Phase 3 reveals the protected data is fundamentally non-replaceable. | Pick C3 (lock-order enforcement) instead of C1/C2. v2.0 ships as a heavyweight fix rather than an elegant one. |
| R4 | Phase 4 implementation introduces new bugs (lock-free memory ordering, ABA, lifetime). | Use battle-tested libraries (TBB, Boost.Lockfree); extensive synthetic testing (§8.4); v1.0 backstop catches anything that slips through. |
| R5 | A future Skyrim update changes the dispatcher and breaks v2.0. | v1.0 backstop preserves cycle-breaking; v2.0 has a config kill-switch that falls back to v1.0-only mode. |
| R6 | Sister deadlocks exist that we haven't observed. | §8.1 systematically surveys the dispatcher; v1.0 stays installed indefinitely. |
| R7 | The lock-protected data is updated from non-lock-acquiring paths the static analysis missed. | Phase 3 dynamic instrumentation should catch this; if it doesn't, v1.0 backstop catches the resulting cycle. |
| R8 | po3 / aers / community move on the same fix concurrently and ship something incompatible. | Coordinate via §8.5. Aim for v2.0 to be merge-able into `EngineFixesSkyrim64`. |

---

## 11. Success criteria

v2.0 is complete when all of the following hold:

1. **Phase 1–3 deliverables** are committed to the repository at the standards described in §4.3, §5.3, §6.3.
2. **A Phase 4 implementation** is shipping. The choice of C1/C2/C3/C4 is documented with the rationale in a v2.0-specific design doc analogous to [`14-final-design-v1.md`](14-final-design-v1.md).
3. **Synthetic test coverage**: the generalised harness from §8.4 includes at least one test per dispatch path Phase 3 identified, and all tests pass.
4. **Soak-test coverage**: ≥ 100 hours of cumulative gameplay across multiple modlists with v2.0 installed and v1.0 backstop active. `cycles_observed = 0` throughout.
5. **Documentation**: the case-study chapters are updated to reflect the v2.0 outcome (analogous to doc 14 for v1.0).
6. **Upstream-readiness**: the v2.0 patch is structured such that aers could merge it into `EngineFixesSkyrim64`, even if we don't ultimately submit it.

---

## 12. Open questions

These are not blockers but should be answered as analysis proceeds:

- Is LockA's host class and LockB's host class actually distinct? §5 of [`05-static-analysis.md`](05-static-analysis.md) inferred from sequential IDs that LockB's hosts are methods on the same class, but did not prove LockA's host is a different class.
- Are LockA and LockB ever taken from non-worker-pool threads (main, render, audio, Havok-internal)? If yes, the marshalling option C2 has to consider thread-affinity.
- Does the AE binary's equivalent code take the same locks in the same orders? If the AE port refactored the dispatcher, AE's behaviour validates or invalidates 1.5.97 hypotheses.
- Are there other public mods that touch any subset of these functions? §8.5 should answer this.

---

## 13. Immediate next actions

1. Set up an IDA Pro + Hex-Rays workstation with the unpacked `SkyrimSE.exe.unpacked.exe` loaded, all known IDs annotated, and CommonLibSSE-NG headers indexed for cross-reference. Estimated: 1 day.
2. Begin Phase 1 §4.1 task 1: enumerate every `lea r?, [rip + disp]` whose target is in `[+0x2eff800, +0x2eff8e0]` and `[+0x2f3b800, +0x2f3b8e8]`. Identify the most-referenced base in each window. Estimated: 1–2 days.
3. From the bases found in step 2, locate the constructors. Estimated: 1 day per singleton.
4. Match vtables against CommonLibSSE-NG / aers / po3. Estimated: 0.5–2 days per singleton depending on whether a direct match exists.
5. Write up the Phase 1 deliverable as `docs/case-study/16-v2-phase1-singletons.md` (or addendum to `05-static-analysis.md`).

After step 5 is reviewed and signed off, Phase 2 begins.

The work is sequenced so that **at every checkpoint the project can stop cleanly with a documented partial result** rather than producing throwaway artifacts. This is in deliberate contrast to the v0.1–v0.5 retracted iterations, which were code-first and ended up as discarded branches.