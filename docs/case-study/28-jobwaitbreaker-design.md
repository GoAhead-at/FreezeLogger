# 28. `JobWaitBreaker` — releasing the `WaitForJobTask` lost-wakeup hang

**Date:** 2026-06-15
**Status:** Implemented in `WorkerSpinLockFix` v2.3.0 (ships
**detect-only** by default; the `SetEvent` release path is gated behind
`[job_wait_breaker] detect_only = false` pending field validation). The
v1.0 runtime breaker, the stale-owner reaper, and the synthetic test
harness were removed in the same release. See
`../../skyrim-freeze-fix/src/JobWaitBreaker.cpp` and
`../../skyrim-freeze-fix/docs/design.md` §3.7.
**Targets:** The Skyrim main-thread `WaitForJobTask` hang documented in
case-study 27 — the *second* hard-freeze class, distinct from the AB-BA
`BSSpinLock` inversion that `WorkerSpinLockFix` already prevents.
**Trigger for this doc:** freeze capture
`freeze_2026-06-15_160413_THIS.log` (FreezeLogger v0.6.0, SE 1.5.97),
the first capture where the Site-B probe walked the whole chain and
proved the lost-wakeup mechanism end-to-end.

---

## 0. One-paragraph summary

Skyrim's `Main::Update` periodically blocks in `WaitForJobTask`
(`SkyrimSE+0xc38130`) on a per-job completion event until the in-flight
jobs drain. Under heavy parallelism a **publish-then-tear-down race**
can lose the wakeup: main reads the job chain as non-null, commits to
`WaitForSingleObject(handle, INFINITE)` on a **manual-reset
`NotificationEvent`**, and *after* main is asleep another thread clears
the job sub-array **without** signalling main's event. The producer
side is then gone — no thread in the process holds the handle — so main
sleeps forever and the watchdog trips at 15 s. There is no clean
script/INI fix; the proposed fix is a tightly-gated **runtime breaker**
(sibling to `WorkerSpinLockFix`) that detects this exact signature and
delivers the missing signal with a single `SetEvent`.

---

## 1. Evidence (freeze_2026-06-15_160413)

FreezeLogger's MainWaitProbe walked the Site-B chain live and produced
a verdict with no ambiguity. The load-bearing rows:

```
Wait-site detection:
  B) +0xc38130 wrapper:  HIT     (main IS inside WaitForJobTask)
Probe via RBX-as-HANDLE:
  Event type:   0 (NotificationEvent (manual-reset))
  Event state:  0 (NOT signaled)
Site-B probe (Singleton-B):
  *(Singleton-B):        0x000001ade3934298   (instance valid)
  sub-array @ +0x08:     0x0000000000000000   <-- NULL
  element[0]:            0x0000000000000000   <-- NULL
  Note: the struct was valid when main loaded it ... so SOMEONE
        cleared it after main went to sleep.
Co-consumer search (other thread holding handle 0x2478): <none>
...
Summary: main TID 16080 waiting on HANDLE 0x2478 [NotificationEvent,
         NOT signaled]; 0 other waiters reference it.
  >>> classic dispatch+wait deadlock: nobody is holding the
      producer side of main's handle.
```

Corroborating facts from the same capture:

- **Not a `WorkerSpinLockFix` case.** `BSSpinLock-owner search
  (threads spinning at SkyrimSE+0x132c5a): <no thread is currently
  spinning on a BSSpinLock>`. WSLF's breaker has nothing to act on —
  this is an event wait, not a lock spin.
- **HDT-SMP is innocent.** The `hdtsmp64.dll+0x434fe` frame above the
  wait is FSMP's `Main::Update` hook trampoline (maintainer-confirmed,
  case-study 27 §0). FSMP's 15 physics workers are all idle-parked at
  `hdtsmp64.dll+0x145d0d` on their per-worker auto-reset events — the
  correct steady state when no batch is queued; they are *downstream*
  of main being stuck, not the cause.
- **Job content looks like background / Story-Manager event jobs** —
  the Singleton-B hex dump carries strings such as `"Cast Magic
  Event"`, `"Crime Gold Event"`. Consistent with a heavy quest/script
  modlist generating many background jobs (this same machine's logs
  show very high script activity).
- **High parallelism widens the window:** 427 threads on 32 logical
  CPUs, several plugin worker pools (FSMP ×15, CommunityShaders,
  SkyParkourNG, CONCRT). The more threads draining the queue, the wider
  the read-handle-then-sleep race.

### 1.1 Attributor caveat

The automated *stuck-job attribution* section in this capture
**misfired**: the job's discriminator was the literal value
`0x0000000000000001`, so the producer search matched `entry[2]==0x1` on
~60 unrelated idle thread-pool stacks (CommunityShaders, SkyParkourNG)
and listed them all as "candidate producers." That output is noise.
The authoritative conclusion is the Site-B verdict above, not the
candidate list. (A follow-up FreezeLogger change should suppress
producer matches when the discriminator is a small integer; tracked
separately, not part of this fix.)

---

## 2. Mechanism

```
        main thread (Main::Update)                worker / drain thread
        --------------------------                ---------------------
  t0    chain = *(Singleton-B + 0x08)
        chain != null  -> jobs in flight
  t1    handle = job completion event
        (manual-reset NotificationEvent)
  t2    WaitForSingleObject(handle,
                            INFINITE)  --asleep-->
  t3                                              drains/clears the job
                                                  sub-array (chain = null)
  t4                                              WITHOUT SetEvent(handle)
                                                  (signal path missed /
                                                   producer already past it
                                                   / producer exits)
  ...   main never wakes; handle stays
        manual-reset + unsignaled; no
        thread references it -> permanent
        freeze; watchdog @ 15 s
```

The key properties that make a safe fix possible:

1. **The job is already gone** at freeze time (`chain == null`). The
   work main was waiting on is effectively complete/abandoned — only
   the *signal* was lost.
2. **The event is manual-reset.** `SetEvent` on it is idempotent;
   signalling it twice (us + a hypothetical late producer) is harmless.
3. **No producer is alive** (`0 other waiters reference it`). There is
   nothing left in the process that will ever signal `handle`.

These three are exactly what FreezeLogger already computes for the
MainWaitProbe, so the detection logic is proven and reusable.

---

## 3. Why there is no "normal" fix

- It is an **engine-level race** inside Skyrim's task pool, not a
  setting or a script. No INI, load-order, or Papyrus change closes the
  window.
- It is **not** the AB-BA `BSSpinLock` inversion, so `WorkerSpinLockFix`
  (both its runtime breaker and its `Phase4Defer` structural layer) does
  not apply — there is no lock cycle here, just a lost wakeup.
- The only actionable levers are (a) reduce the probability by lowering
  concurrent task-pool pressure (mitigation, §6), or (b) recover from it
  deterministically with a breaker (the fix, §4).

---

## 4. Proposed fix — `JobWaitBreaker`

A minimal runtime breaker, shipped either as a **new module inside
`skyrim-freeze-fix`** (preferred — it reuses the existing watchdog,
config, and logging infrastructure) or as a standalone SKSE plugin.

### 4.1 Capture main's wait handle

Wrap `WaitForJobTask` (`SkyrimSE+0xc38130`, resolved via SkyrimAnchors
/ `REL::RelocationID` so it is multi-runtime ready):

- **On entry:** record into a single main-thread slot:
  `{ inJobWait = true, handle = <event main is about to wait on>,
     enteredAtMs = now }`.
- **On return:** clear the slot (`inJobWait = false`).

This is main-thread-only and runs once per job-wait, so it adds no
measurable cost. It gives us the handle deterministically without
guessing it from registers at watchdog time.

> Note: extracting the handle on entry requires reading the same
> Singleton-B chain the engine is about to read. If hooking the entry
> proves fiddly, the fallback is the FreezeLogger approach: at watchdog
> time, read main's `RBX` (the KERNELBASE working register holds the
> handle at the `WaitForSingleObjectEx` frame) and validate it with
> `GetHandleInformation` + `NtQueryEvent`. The hook is cleaner; the RBX
> path is the proven backstop.

### 4.2 Watchdog poll

A background thread (the WSLF watchdog, or a dedicated one) ticks every
~250 ms and, when `inJobWait` is true, checks how long main has been
parked.

### 4.3 Break gate (ALL must hold)

Fire only when the **complete** lost-wakeup signature is present:

1. Main is still inside `WaitForJobTask` (slot still set, same handle).
2. It has been parked **longer than the dwell threshold** — default
   **3000 ms** (far above any legitimate job-wait during normal play,
   well below the 15 s freeze threshold).
3. `handle` is a **manual-reset event** and is **NOT signaled**
   (`NtQueryEvent`).
4. The **Singleton-B job sub-array is now NULL** (`*(Singleton-B+0x08)`
   chain walks to null) — i.e. the job is provably gone.
5. **No other thread** references `handle` in any register (the
   co-consumer search comes back empty).
6. **Re-check after a short confirmation window** (e.g. 200 ms): all of
   1–5 still hold. This eliminates any chance of racing a legitimate
   late signal.

### 4.4 Break action

`SetEvent(handle)`.

Because the manual-reset event becomes signalled, main's
`WaitForSingleObject` returns and the frame proceeds as if the lost
completion signal had arrived. Record telemetry: `breaks_done++`,
log the handle, dwell time, and the gate values that matched.

### 4.5 Safety argument

- **No false release of live work.** Gate 4 requires the job chain to
  be null; the job is already complete/abandoned, so waking main is
  semantically equivalent to the signal that was lost. Worst case is a
  single-frame visual glitch versus a permanent freeze.
- **No double-signal hazard.** Manual-reset `SetEvent` is idempotent.
- **No interruption of normal play.** Gate 2's 3 s dwell + gate 6's
  re-check guarantees the breaker never touches a healthy frame — no
  legitimate job-wait lasts seconds.
- **No engine-thread suspension.** Unlike the retired v1 reaper
  (case-study 26), this breaker only *reads* thread context/registers
  and calls `SetEvent`; it never `SuspendThread`s an engine thread on
  its runtime path.

---

## 5. Rollout plan

1. **Phase A — detect-only (ship first).** Implement §4.1–§4.3 plus a
   log line describing what the breaker *would* signal, but **do not**
   call `SetEvent`. Validate against this capture and 1–2 more real
   freezes that the gate fires exactly when (and only when) the
   lost-wakeup signature is genuinely present.
2. **Phase B — enable the break.** Flip on §4.4 behind a config toggle
   (default off until validated), diagnostic logging recommended on for
   the first sessions, mirroring the WSLF `Phase4Defer` rollout.
3. **Phase C — defaults + docs.** Once field-validated, enable by
   default, write the release note, and update case-study 27's status
   from "Open" to "Recoverable via `JobWaitBreaker`".

### Config (proposed)

```toml
[job_wait_breaker]
enabled                 = true     # master switch (Phase B: default off)
dwell_threshold_ms      = 3000     # how long main must be parked first
recheck_window_ms       = 200      # confirmation re-check before SetEvent
diagnostic_logging      = false    # verbose gate/telemetry logging
detect_only             = false    # Phase A: log but never SetEvent
```

---

## 6. Interim mitigation (until the breaker ships)

This race is load/parallelism-sensitive, so reducing concurrent
task-pool pressure lowers its frequency (it does **not** eliminate it):

- Trim Story-Manager / background-event churn from heavy quest-script
  mods (the job content in this capture was background events).
- Where mods expose worker-count settings, avoid stacking many large
  worker pools.
- Expect it most often around heavy task-pool moments — cell-load
  boundaries, scene teardown, asset streaming (case-study 27 §47).

---

## 7. Open questions / follow-ups

- **Handle extraction**: confirm whether the `WaitForJobTask` entry hook
  can cheaply read the to-be-waited handle, or whether to ship the RBX
  backstop (§4.1) for v1.
- **Attributor noise** (§1.1): FreezeLogger should skip producer matches
  when the job discriminator is a small integer (`<= 0xffff`), so the
  stuck-job section stops drowning the real verdict.
- **Cross-runtime**: resolve `WaitForJobTask` and the Singleton-B slot
  via SkyrimAnchors (already version-independent in FreezeLogger) so the
  breaker is SE+AE ready from day one.
- **Telemetry parity**: surface `inJobWait`, longest observed dwell,
  and `breaks_done` in the same stats block as WSLF's `Phase4Defer`
  counters.
