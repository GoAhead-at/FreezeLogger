# 29. `SiteABreaker` — releasing the Site-A worker-ack deadlock (PLAN)

**Date:** 2026-06-16
**Status:** IMPLEMENTED in `WorkerSpinLockFix` v2.4.0 (ships **detect-only**
by default; the `SetEvent` release path is gated behind
`[site_a_breaker] detect_only = false` pending field validation). Built as
a third WSLF layer alongside `Phase4Defer` (AB-BA spinlock prevention) and
`JobWaitBreaker` (Site-B `WaitForJobTask` lost-wakeup recovery). See
`../../skyrim-freeze-fix/src/SiteABreaker.cpp`, the Site-A scan in
`../../skyrim-freeze-fix/src/SkyrimAnchors.cpp`, and
`../../skyrim-freeze-fix/docs/design.md` §3.8.
**Targets:** A *third* hard-freeze class — the `Main::Update` **Site-A
worker-acknowledgement** wait (`id 34554` / Singleton-A), distinct from
both the AB-BA `BSSpinLock` inversion (case-study 06/Phase4Defer) and the
Site-B `WaitForJobTask` hang (case-study 27/28/JobWaitBreaker).
**Triggering captures (FreezeLogger v0.7.0, SE 1.5.97):**
- `freeze_2026-06-16_133223_both.log` — both main+render stalled, HDT-SMP **3.2.0.0**
- `freeze_2026-06-16_152328_main.log`  — main only stalled, HDT-SMP **3.1.0.0**

---

## 0. One-paragraph summary

`Main::Update` has **two** infinite-wait sites. Site-B is `WaitForJobTask`
(handled by `JobWaitBreaker`). **Site-A** (`id 34554`, reached via
`id 35565`) is the engine's synchronous parallel-work dispatch: the caller
sets a work-pending flag and wakes a worker via an auto-reset event
(`Singleton-A[+0x58]`), then blocks `INFINITE` on a manual-reset
worker-**ack** event (`Singleton-A[+0x60]`) until the worker signals
completion. When a worker consumes the wake but never signals the ack,
main sleeps forever. Both field captures show this exact state
(`pending=1`, worker-wake auto-event **not** signaled = already consumed,
worker-ack manual-event **not** signaled, no BSSpinLock cycle). In both,
the caller one frame up is **`hdtsmp64.dll`** (Faster HDT-SMP) driving the
engine's parallel cloth dispatch. The proposed fix mirrors
`JobWaitBreaker`: detect the stuck signature and deliver the missing
signal with a single `SetEvent` on the ack handle.

---

## 1. Evidence — the two captures are the same signature

Main's stack is identical in both (handles differ per session, as expected):

```
#00 ntdll!NtWaitForSingleObject
#01 KERNELBASE!WaitForSingleObjectEx
#02 SkyrimSE+0x5765ff  [id 34554 +0x2f]      <- Site-A wait helper
#03 SkyrimSE+0x5b35dd  [id 35565 +0x5ed]      <- caller
#04 hdtsmp64.dll+0x4xxxx                       <- Faster HDT-SMP
waiting on: HANDLE=0x26xx [NotificationEvent (manual), NOT signaled]
```

Site-A probe (Singleton-A @ `SkyrimSE+0x2f26680`), both captures:

| field | 133223 | 152328 |
|---|---|---|
| `[+0x58]` worker-wake handle (auto-event) | `0x260c` | `0x25d0` |
| `[+0x60]` worker-ack handle (manual-event) | `0x2610` | `0x2630` |
| `[+0x68]` work-id | `0` | `0` |
| `[+0x6c]` pending flag | `1` | `1` |
| worker-wake event state | NOT signaled | NOT signaled |
| worker-ack event state | NOT signaled | NOT signaled |
| BSSpinLock spinning? | none | none |
| Wait-site detection | A=HIT, B=miss | A=HIT, B=miss |

FreezeLogger's own verdict in both: **`DEADLOCK SIGNATURE MATCH` — pending=1,
worker never signaled completion; main parked in `id 34554`'s INFINITE wait.**

### Key findings
1. **It is genuinely stuck, not slow.** FreezeLogger trips only after 15 s of
   zero main progress and neither freeze self-resolved (player force-killed).
   A still-progressing worker would have signaled the ack and the frame loop
   would have continued. The capture itself proves the worker is stuck/gone.
   This is the same "stuck is established by the non-self-resolving dwell"
   reasoning that justifies `JobWaitBreaker`.
2. **HDT-SMP version-independent.** Reproduced on both 3.2.0.0 and 3.1.0.0.
   => downgrading HDT-SMP is **not** a fix; the common factor is HDT-SMP (any
   version) driving the engine's Site-A parallel dispatch, not a 3.2.0.0
   regression. (This retires the "just downgrade FSMP" hypothesis.)
3. **Separable from render.** 152328 had only main stalled (render healthy);
   render uses a different Singleton-A wait pair (`id 34557/34567`). The fix
   targets main's `id 34554` ack only.
4. **Not Phase4Defer's domain** (no spinlock cycle) and **not
   JobWaitBreaker's** (Site-B wrapper = miss). Site-A is an uncovered blind
   spot today — which is why WSLF logged `job_wait: stuck=0 released=0`.

---

## 2. Mechanism

```
caller (id 35565, invoked by hdtsmp64.dll)
  -> Singleton-A[+0x68] = work-id
  -> Singleton-A[+0x6c] = 1            (pending)
  -> SetEvent(Singleton-A[+0x58])      (wake a worker; auto-reset)
  -> WaitForSingleObjectEx(Singleton-A[+0x60], INFINITE)   (id 34554; ack)
worker
  -> wakes (auto-event consumed -> back to NOT signaled)
  -> runs the job ...
  -> [SHOULD] SetEvent(Singleton-A[+0x60])  (ack)  <-- NEVER HAPPENS
```

Auto-wake reset + ack never set + worker not parked in WFSO = a worker
woke, took the job, and never came back to ack (died, lost, or stuck
elsewhere). Main waits forever.

---

## 3. Proposed fix — `SiteABreaker` module

Mirrors `JobWaitBreaker` and reuses its watchdog pattern.

1. **Hook** the Site-A wait helper (`id 34554`, `SkyrimSE+0x5765d0`) the
   same way we wrap `WaitForJobTask`. Track main-thread enter/exit + an
   episode sequence + `thread_local` depth (pin main on first call).
2. **Derive the ack handle at wait entry** from Singleton-A:
   `ack = *(singletonA + 0x60)` (and read `pending` `[+0x6c]`, `work-id`
   `[+0x68]`, worker-wake `[+0x58]`). SEH-guarded; 0 on fault. Capturing at
   entry keeps the recovery robust even if fields change later.
3. **Stuck gate (watchdog).** After `dwell_threshold_ms` with main still in
   the same Site-A episode, sample the signature; wait `recheck_window_ms`;
   re-sample. Act only if **zero progress** across the window:
   - main still parked in `id 34554`, same episode/seq;
   - `pending` still 1; `work-id` unchanged;
   - worker-ack event still NOT signaled.
4. **Recover (active mode):** `SetEvent(ack)`.
5. **Config / safety:** new `[site_a_breaker]` TOML block mirroring
   `[job_wait_breaker]` (`enabled`, `detect_only`, `dwell_threshold_ms`,
   `poll_interval_ms`, `recheck_window_ms`, `diagnostic_logging`). Ships
   **`detect_only = true`** for the first session.
6. **Anchoring:** `id 34554` / `id 35565` / Singleton-A slot
   (`SkyrimSE+0x2f26668`, instance `+0x2f26680`) resolved via Address
   Library RelocationID where stable, or a `.text` body signature like
   `SkyrimAnchors` does for Site-B. (TBD which is version-robust for AE —
   see open questions.)

### Reused infrastructure
The watchdog thread, episode-tracking atomics, `NowMs`, stats counters,
and the `unsafe_call` inline-hook discipline are all copy-adaptable from
`JobWaitBreaker.cpp`. No process-wide hook needed (handle is chain/struct
derived, same as the v2.3.0 JWB rework).

---

## 4. Risk analysis

- **"Slow worker" risk: negated** by finding #1 — the dwell + zero-progress
  gate only fires on a worker that is provably not progressing.
- **Residual risk = data coherence on release.** Site-A is a worker-*completion*
  ack (vs. Site-B's already-torn-down job). If main is released while the
  worker's output is half-written, main could read inconsistent data.
  **Mitigation / why it's acceptable here:** the stuck work is **HDT-SMP
  cloth/physics** — per-frame, visual, self-correcting; a stale/skipped
  cloth frame is cosmetically trivial and fixes next tick. And the
  alternative is a guaranteed permanent freeze + force-kill (losing all
  progress since last save). Same tradeoff already accepted for Site-B.
- **Wrong-target risk:** `id 34554` may be a generic wait helper used for
  more than this dispatch. The detour must gate on main-thread + the
  Singleton-A `pending` pattern before counting an episode, so unrelated
  callers are ignored. Detect-only validates this before any `SetEvent`.

---

## 5. Validation plan

1. Build with `[site_a_breaker] detect_only = true`. Confirm in
   `WorkerSpinLockFix.log`: the module arms, resolves `id 34554` /
   Singleton-A, and on the next freeze logs the stuck signature with a
   derived ack handle that matches FreezeLogger's `0x26xx`.
2. One clean detect-only confirmation -> flip `detect_only = false`.
3. Confirm a `STUCK Site-A RELEASED ... SetEvent(0x26xx)` line and that the
   game resumes (FreezeLogger should log a resolve / no capture).
4. Keep FreezeLogger v0.7.0 installed throughout for independent capture.

---

## 6. Render-side Site-A (`id 34557/34567`) — implemented in v2.5.0

`freeze_2026-06-24_053440` (FreezeLogger v0.7.0) closed this follow-up: it
captured the **render-side** counterpart of the main-side deadlock, which the
classifier had been reporting as "Unrecognised". The render thread was parked
inside `id 34557` (frames `id 34557+0x7d` / `id 34567+0x86`, RBP = the
Singleton-A instance `module+0x2f26680`) while main spun on a heap `BSSpinLock`
held by the same stalled HDT-SMP worker pool ("0 idle").

Disassembly of the render path (against the unpacked SE 1.5.97 binary):

- `id 34567` (`0x576cd0`) is the render worker loop. It waits `INFINITE` on
  the worker-wake `[+0x58]`, then per work item calls `id 34557`, then signals
  the worker-ack `[+0x60]` — i.e. it reads the **same** Singleton-A field
  layout as main's `id 34554` (`[+0x58]`/`[+0x60]`/`[+0x6c]`).
- `id 34557` (`0x576700`) is the per-task cloth-join. It iterates the
  dispatched sub-tasks and waits `INFINITE` on each one's completion (the
  innermost wait, reached via a virtual call at `+0x7b`/`+0x7d`). When a
  sub-task worker is stuck, the render thread parks here forever and the loop
  never reaches the `[+0x60]` ack `SetEvent`.

**FreezeLogger v0.9.0** now resolves `id 34557` by a byte-unique entry
signature and reports the render-side Site-A class (`RenderSiteAWorkerAck`)
plus a render-side characterization block in `MainWaitProbe` (render context +
Singleton-A field/ack readback).

**WorkerSpinLockFix v2.5.0** adds Layer 4 (`SiteARenderBreaker`): an inline
wrap on `id 34557` capturing the Singleton-A from `rcx` and the worker-ack
`[+0x60]` at entry, with the same dwell + zero-progress watchdog as Layer 3.
In active mode it `SetEvent`s the worker-ack to publish the completion the
stuck join would have raised. It ships `detect_only=true` until the release
path is validated against a FreezeLogger v0.9.0 render-side capture, because
the ack publishes completion to the waiter rather than unblocking the deepest
per-sub-task wait inside `id 34557`.

**Follow-up (case-study 30):** when main simultaneously spins on a leaked heap
`BSSpinLock` held by the same idle worker pool, Layers 3–4 cannot unblock main.
`LeakedSpinLockBreaker` (Layer 5, v2.6.0) force-releases that lock.

## 7. Open questions
- AE anchoring for `id 34554` / `id 35565` / Singleton-A (RelocationID vs
  body signature). SE 1.5.97 is confirmed by all captures. (`id 34557` resolves
  by the same kind of `.text` signature and is expected to port the same way.)
- Is there an upstream root cause (worker thread death / HDT-SMP dispatch
  bug) worth reporting to the FSMP maintainer, independent of the breaker?
  The "0 idle" worker pool behind both the main-side and render-side variants
  points at a single stuck/exited HDT-SMP worker as the common producer.
