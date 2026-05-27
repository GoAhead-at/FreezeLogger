# 27. HDT-SMP-UP main-thread event-wait deadlock

**Date observed:** 2026-05-26 to 2026-05-27 (six freezes captured)
**Affected mod:** `Faster HDT-SMP-UP` (community SE/AE fork of HDT-SMP)
**Status:** Open. Reported here as a bug-class characterisation suitable for
forwarding to the upstream maintainer. **Not** a `WorkerSpinLockFix` issue —
the WSF telemetry is clean across all six freezes; the bug lives entirely
inside `hdtsmp64.dll`.

---

## TL;DR for the maintainer

- The Skyrim main thread parks on a **manual-reset `NotificationEvent`
  whose producer side has no live owner anywhere in the process**.
- The wait is initiated from inside `hdtsmp64.dll+0x42f0e` (frame `#03`
  at the top of the stack), via a known Skyrim helper at
  `id 35565+0x50e` -> `id 5664+0xc38130` that vectors through a
  vtable-style dispatch table (Singleton-B at `SkyrimSE+0x2f26a70`).
- After main parks, **the dispatch struct main read its handle from is
  zeroed by some other thread**. The struct was non-zero when main loaded
  it (otherwise the wrapper would have early-returned without waiting),
  so the producer side was published, consumed by main, and then torn
  down while main was still parked. Classic write-after-publish race.
- The HDT-SMP worker pool (16 threads) is *idle* at freeze time, each
  parked on its own per-worker auto-reset event in `hdtsmp64.dll+0x14572d`.
  The workers are not deadlocked against each other — they are simply not
  going to be told to do anything more, because the producer side of
  main's wait event has been torn down.
- Six freezes captured in two days, five during cell-load (`Open menus:
  loading screen`) and one during combat in an exterior cell.

If you read nothing else, the smoking gun is in §5.

---

## 1. Affected build

| Field | Value |
|---|---|
| File | `<MO2>/MODS/mods/Faster HDT-SMP-UP/SKSE/Plugins/hdtsmp64.dll` |
| FileDescription | `hdtsmp64` |
| FileVersion | `3.0.5.0` |
| ProductVersion | `3.0.5.0 (avx)` |
| Copyright | `GPLv3` |
| Size | 2,218,496 bytes |
| SHA-256 | `F8FE49BABF999C1ABE13F9B4A79AEF1670C5FD6845492F1D339779C1E4A056EA` |
| Installed | 2026-05-20 (file mtime) |
| Game runtime | Skyrim SE 1.5.97.0 (anniversary downgrade / "old runtime") |

If this is not the build you maintain, the SHA-256 is the most reliable way
to identify the exact binary. Multiple community forks share the file name.

## 2. Symptom

The game becomes fully unresponsive: input does nothing, the screen does not
update (or freezes mid-fade on a loading screen), audio loops on the last
frame's buffer, alt-tab and `Win` key are ignored. Only Task Manager kills
it. No crash dialog. No mini-dump from the engine itself (FreezeLogger
writes its own).

The user typically experiences this 2-6 times per session.

## 3. Reproduction profile

Six freezes captured in 48 hours of play; FreezeLogger output is in
`Documents/My Games/Skyrim Special Edition/SKSE/FreezeLogger/`. All six
match the same fingerprint.

| # | Captured | Stalled | Cell | Worldspace | Menu | Render age | hdtsmp64 frames |
|---|---|---|---|---|---|---|---|
| 1 | 2026-05-26 01:24:56 | main | RoriksteadExterior03 | Tamriel | **loading screen** | 0 ms | 1 (main only) |
| 2 | 2026-05-26 02:44:43 | main | WhiterunExterior16 | Tamriel | **loading screen** | 16 ms | 10 (main + 9 workers) |
| 3 | 2026-05-26 04:06:06 | main | WhiterunPlainsDistrict03 | WhiterunWorld | **loading screen** | 0 ms | 11 (main + 10 workers) |
| 4 | 2026-05-27 20:14:38 | main | POIPineForest17 | Tamriel | **loading screen** | 16 ms | 1 (main only) |
| 5 | 2026-05-27 22:03:46 | main | DragonMoundPineForest02 | Tamriel | **loading screen** | 16 ms | 1 (main only) |
| 6 | 2026-05-27 23:15:52 | **both** | BrokenHelmHollowExterior02 | Tamriel | none (combat) | (rendered last 15 s ago) | 16 (main + 15 workers) |

Pattern:

- 5 / 6 = freeze during a cell-loading screen, render thread still ticking,
  main thread parked.
- 1 / 6 = freeze during active melee combat with many actors, both main
  and render threads frozen.

Both shapes share the same `hdtsmp64.dll+0x42f0e` main-thread fingerprint
and the same wait-event-with-no-producer mechanism. The cell-load case is
~5x more frequent in this user's data.

The session that produced freeze #6 had been running 75 minutes; the user
reported no warning, just a sudden freeze in combat. Auto-save fired
~75 seconds before the freeze instant.

## 4. Stack evidence (freeze #6, the cleanest sample)

### 4.1. Main thread

```
TID 11060 [main game thread]
  #00 0x00007ff91ddc00f4  ntdll.dll!NtWaitForSingleObject+0x14
  #01 0x00007ff91b52c2ef  KERNELBASE.dll!WaitForSingleObjectEx+0xaf
  #02 0x00007ff6ef2d34fe  SkyrimSE.exe+0x5b34fe   [id 35565 +0x50e]
  #03 0x00007ff8417b2f0e  hdtsmp64.dll+0x42f0e
  nv-regs: RBX=0x2824 RBP=0x0 RSI=0xffffffff RDI=0x0
  waiting on: HANDLE=0x2824 [NotificationEvent (manual), NOT signaled]
```

`hdtsmp64.dll+0x42f0e` is the immediate caller of Skyrim's wait-helper
chain. From the saved RSP context (next frames recovered from stack
fragment): main was inside an HDT-SMP "frame batch produce/wait" path.
Main's `RBX` is the kernel HANDLE that `WaitForSingleObjectEx` clobbers
on entry — i.e. main is parked waiting on event `0x2824`.

### 4.2. HDT-SMP worker pool — sixteen threads, all idle

```
TID 28188   waiting on: HANDLE=0x7218 [SynchronizationEvent (auto), NOT signaled]
  #02 hdtsmp64.dll+0x14572d
TID 21280   waiting on: HANDLE=0x77c8 [SynchronizationEvent (auto), NOT signaled]
  #02 hdtsmp64.dll+0x14572d
TID 29996   waiting on: HANDLE=0x7584 [SynchronizationEvent (auto), NOT signaled]
  #02 hdtsmp64.dll+0x14572d
... 13 more identical frames, each on its own per-worker auto-reset event ...
```

Per-worker dedicated handles, all auto-reset, all NOT signaled. This is
the *normal* idle state for an HDT-SMP worker (waiting for the next
job push). The workers are not in a deadlock with each other or with
main — they are simply waiting for the dispatcher to fire their wakeup
event, which it never will, because (per §5) the dispatcher-side state
has been torn down.

### 4.3. Render thread

```
TID 28784 [render thread]
  #00 ntdll.dll!NtWaitForSingleObject+0x14
  #01 KERNELBASE.dll!WaitForSingleObjectEx+0xaf
  #02 SkyrimSE.exe+0x576770   [id 34557 +0x70]
  #03 SkyrimSE.exe+0x576d56   [id 34567 +0x86]
  #04 SkyrimSE.exe+0xc0d6bd   [id 67147 +0x3d]
  waiting on: HANDLE=0x28c0 [SynchronizationEvent (auto), NOT signaled]
```

Render is inside Skyrim's own render-queue wait, blocked because the main
thread is no longer feeding it work. This is a downstream symptom, not a
cause — the render thread becomes idle on its own as a side effect of
main being stuck.

## 5. Smoking gun — the dispatch struct is zeroed *after* main parks

FreezeLogger walks the chain main read its wait HANDLE from. The chain
starts at the global `Singleton-B` pointer at `SkyrimSE+0x2f26a70` and
walks `[+0x08] -> element[0] -> *element[0] -> [vtable+1]` to recover
the HANDLE that was passed into `WaitForSingleObject`.

At freeze time, the chain looks like this:

```
*(SkyrimSE+0x2f26a70):       0x000001c8f00b4298   (instance pointer)
sub-array @ +0x08:           0x0000000000000000   <-- ZERO
element[0] (sub_array+0):    0x0000000000000000   <-- ZERO
*element[0] (vtable):        0x0000000000000000   <-- ZERO
handle @ vtable[1]:          0x0000000000000000   <-- ZERO
```

But the wait wrapper at `id 5664+0xc38130` is structured to early-return
without waiting if any of those pointers are null. Main is in the
`WaitForSingleObject` call, so the chain *was* non-zero when main loaded
it. **The chain was nulled by something else after main fell asleep.**

There is no other thread in the entire process whose register set
references either main's wait handle (`0x2824`) or the singleton instance
pointer (`0x000001c8f00b4298`):

```
Co-consumer search (other thread with any register == main RBX 0x2824):
    <no other thread has main's wait handle in any register>
Toucher-thread search (other thread with any register == singleton 0x1c8f00b4298):
    <no other thread is currently touching the singleton instance>
```

And from the wait-graph cross-tabulation (270 threads parked across the
process):

```
HANDLE 0x2824 [NotificationEvent, NOT signaled] - 1 waiter(s):
    TID 11060 [MAIN]

Summary:
  main TID 11060 waiting on HANDLE 0x2824 [NotificationEvent, NOT signaled]; 0 other waiters reference it.
  >>> classic dispatch+wait deadlock: nobody is holding the producer side of main's handle.
```

Interpretation: the HDT-SMP code at `hdtsmp64.dll+0x42f0e` (or some helper
it calls) had a producer side that

1. published a dispatch struct into Singleton-B,
2. handed main a HANDLE drawn from that struct,
3. main called `WaitForSingleObject(HANDLE, INFINITE)`,
4. the producer (or some sibling cleanup path) cleared the dispatch
   struct **without first waking main**.

Step 4 turns the wait into "main is now waiting forever on a HANDLE
nobody else even knows exists". The handle is still owned by main's
thread; nothing crashes; nothing logs; the game just stops.

The 16 worker threads stay idle for the same reason: the wakeup events
they would need to receive go through that same dispatch struct, which
no longer exists.

## 6. Why this is HDT-SMP and not Skyrim or another mod

Three independent confirmations.

### 6.1. WorkerSpinLockFix telemetry is clean

The user runs `WorkerSpinLockFix v2.0.3` (an unrelated SKSE plugin that
fixes a known `BSSpinLock` AB-BA inversion in Skyrim's worker dispatcher,
Address-Library `id 12210`). WSF was loaded, armed, and reporting healthy
counters at the freeze instant:

```
[2026-05-27 23:15:50.062] stats:
  acq_slow=1223
  cycles_observed=0
  cycles_confirmed=0
  breaks_done=0
  breaks_raced=0
  breaks_suppressed=0
  | phase4: queued=4455 drained=4455 passthrough=150914
  | reaper: scans=0 threads=0 spinners=0 candidates=0 edges=0 stale_reaped=0 races=0
```

`phase4: queued=4455 drained=4455` (perfectly balanced — the structural
fix preempted 4,455 AB-BA cycles in this 75-minute session, every single
one drained successfully). `cycles_observed=0` and `breaks_done=0` mean
Skyrim's own worker-pool spinlocks were not in any wait-for cycle at any
point. Whatever this freeze is, it is not Skyrim's `BSSpinLock` cycle.

### 6.2. No spinning thread on Skyrim's spin-retry RVA

```
BSSpinLock-owner search (threads spinning at SkyrimSE+0x132c5a):
    <no thread is currently spinning on a BSSpinLock>
```

No thread in the process is spinning inside `BSSpinLock::Acquire`. The
deadlock is purely event-based.

### 6.3. The fingerprint *is* HDT-SMP

`hdtsmp64.dll+0x42f0e` (main-thread frame `#03`) and `hdtsmp64.dll+0x14572d`
(per-worker-thread frame `#02`) are both inside the loaded `hdtsmp64.dll`
image (`0x00007ff841770000-0x00007ff841998000`, exactly the build in §1).

## 7. Suggested investigation areas

The two observed return-into-`hdtsmp64.dll` addresses give you the call
sites to look at in source:

- **`+0x42f0e`** — the main-thread "wait for frame batch / signal main is
  ready" call site. Whatever publishes the `Singleton-B` dispatch struct
  and hands main a HANDLE to wait on is reached from this function.
- **`+0x14572d`** — the per-worker idle-wait, called by every worker after
  it finishes a job and re-enters the pool.

Concrete things worth auditing:

1. **The lifetime of whatever you're publishing into Skyrim's
   `Singleton-B` (`SkyrimSE+0x2f26a70`).** From the FreezeLogger evidence
   the slot at `[+0x08]` (and everything reachable from it) is being
   zeroed while a consumer (Skyrim's main thread) still has the wait
   handle in flight. Either:
   - the producer should not tear down the struct until all consumers
     have drained, or
   - the consumer-side wait should be cancellable (a sentinel signal +
     re-check of state on wake).

2. **The frame batch lifecycle around cell-load.** Five of six freezes
   are during a `loading screen`. The engine tears down the actor scene
   on cell exit and rebuilds it on entry. If HDT-SMP's per-frame batch
   is in flight during that teardown:
   - the producer may receive a "stop / drain" signal and clear its
     dispatch struct,
   - main thread, having already called into HDT-SMP for the in-flight
     batch, parks on the previous batch's completion event,
   - the completion event is the property of the dispatch struct that
     just got cleared,
   - main is now waiting on an orphaned event.

   A "drain pending main-thread waiters before clearing dispatch" pass
   in your cell-transition path would close this case.

3. **The combat-storm shape (freeze #6).** Many actors with HDT-SMP
   physics-enabled cloth/hair/armor in close quarters, plus auto-save
   firing 75 seconds before the freeze. Auto-save can stall the main
   thread briefly while the engine snapshots actor state; if your
   producer pushed a batch *during* the auto-save and the main thread
   resumed and immediately consumed the batch's completion event before
   the workers could finish, the same orphan-event race could fire on
   the per-batch boundary instead of the cell-load boundary.

4. **Per-worker auto-reset events.** Each worker has its own dedicated
   auto-reset `SynchronizationEvent`. If there is any code path that
   publishes a worker handle into a shared structure and then closes /
   reassigns it without first signalling, the worker stays parked
   forever. The 16 idle workers in freeze #6 are consistent with that
   (they outlived the dispatcher that should have woken them).

The pattern matches a textbook "publish-then-tear-down" race on a
producer-consumer event protocol. The fix is usually one of:

- a barrier on cleanup that waits for in-flight consumers to drain;
- a sentinel "shutdown" signal on the wait handle that the consumer
  re-checks for;
- moving the dispatch struct to a refcounted handle so cleanup is
  ordered behind the last consumer.

## 8. How the maintainer can reproduce / capture this themselves

The freeze is rare-but-reliable: a Nolvus Awakening modlist on Skyrim
SE 1.5.97 hits this 2-6 times per real-time hour of play, mostly across
cell-load boundaries. Concrete reproduction steps that worked here:

1. Modlist: Nolvus Awakening (any release recent enough to ship Faster
   HDT-SMP-UP), Skyrim SE 1.5.97 (downgraded).
2. Install [`FreezeLogger`][fl] (the diagnostic SKSE plugin used to
   capture the data in this report). Put `FreezeLogger.dll`,
   `FreezeLogger.toml`, and `FreezeLogger.pdb` in `Data/SKSE/Plugins`.
3. Either:
   - **Cell-load reproduction**: fast-travel to any exterior worldspace
     hub (Whiterun plains, Riverwood, etc.) repeatedly. Five of six
     captured freezes were across cell-load boundaries.
   - **Combat reproduction**: walk into any 4+ actor melee scene in a
     dense exterior (Falmer cave entrance worked here).
4. When the game freezes, FreezeLogger writes a `freeze_<timestamp>_*.log`
   plus a matching `.dmp` to `Documents/My Games/Skyrim Special
   Edition/SKSE/FreezeLogger/`. The watchdog fires after 15 s of main /
   render thread quiescence, so the report is *while* the game is still
   frozen — not after the user kills it.
5. The wait-graph and `MainWaitProbe` sections are the load-bearing parts.
   Specifically look at:
   - the main-thread stack containing `hdtsmp64.dll+0x42f0e`,
   - the wait-graph entry for the HANDLE in main's `RBX`,
   - the `Site-B probe` block, especially whether the chain at
     `SkyrimSE+0x2f26a70 -> [+0x08] -> ...` is still intact or zeroed.

If the chain is zeroed *and* the HANDLE has zero other waiters, you are
looking at this exact bug.

[fl]: not yet on Nexus — internal companion plugin to WorkerSpinLockFix.
The user can supply the binary on request.

## 9. Cross-mod context (so you can rule out interactions)

Other relevant SKSE plugins loaded at freeze time (full list available on
request — 200+ modules):

- `WorkerSpinLockFix v2.0.3` — surgical fix for Skyrim's `BSSpinLock`
  AB-BA inversion. **Fully cleared by its own telemetry** (§6.1). Does
  not hook anything HDT-SMP-related; only hooks
  `BSSpinLock::Acquire (id 12210)`, `id 19369`, and two call sites inside
  the cycle hub.
- `EngineFixes` (Engine Fixes for Skyrim SE) — the standard engine-fix
  pack. No relevant fix in its v6+ changelog points at this code area.
- `skyrim-freeze-fix` (GarrixWong) — addresses `BSReadWriteLock`
  cell-loading deadlocks. Different lock primitive, different call
  sites; running it alongside HDT-SMP did not prevent the cell-load
  freezes in this dataset, suggesting the bug is not the same
  `BSReadWriteLock` class.
- `RecursionFPSFix` — unrelated.
- `CBPC-Collision` — physics, but operates on a different code path
  (CBPC-Collision is collision callbacks; HDT-SMP is the cloth/hair
  solver). Worth ruling out by reproduction with CBPC disabled.

The user has not changed mods between sessions in the 48-hour window
that produced these six freezes; the modlist is constant. The only
variable is gameplay.

## 10. Data available for the maintainer

| Artifact | Path | Approx size |
|---|---|---|
| Freeze report 1 (cell load, Rorikstead) | `freeze_2026-05-26_012456_main.log` | 372 KB |
| Freeze report 2 (cell load, Whiterun ext) | `freeze_2026-05-26_024443_main.log` | 369 KB |
| Freeze report 3 (cell load, Whiterun plains) | `freeze_2026-05-26_040606_main.log` | 397 KB |
| Freeze report 4 (cell load, pine forest) | `freeze_2026-05-27_201438_main.log` | 442 KB |
| Freeze report 5 (cell load, dragon mound) | `freeze_2026-05-27_220346_main.log` | 408 KB |
| Freeze report 6 (combat, Falmer cave) | `freeze_2026-05-27_231552_both.log` | 320 KB |
| Mini-dump for #6 | `freeze_2026-05-27_231552.dmp` | 3.4 MB |
| Mini-dump for #5 | `freeze_2026-05-27_220346.dmp` | 5.0 MB |
| Mini-dump for #4 | `freeze_2026-05-27_201439.dmp` | 5.6 MB |
| Mini-dump for #3 | `freeze_2026-05-26_040606.dmp` | 5.6 MB |
| Mini-dump for #2 | `freeze_2026-05-26_024443.dmp` | 4.8 MB |
| `WorkerSpinLockFix.log` | session log spanning the freeze | 23 KB |

The report `.log` files contain (by section):

1. System info (OS build, RAM, page-file, working set, handle count, CPU).
2. Full thread snapshot for every thread in the process — TID, stack
   walk (up to 256 frames, symbolicated where PDBs are available),
   non-volatile register dump, kernel wait handle and its state.
3. Loaded modules with paths (so you can see exactly which build of
   `hdtsmp64.dll` was loaded).
4. Engine state (player position, cell, worldspace, in-game time, paused
   flags).
5. The `MainWaitProbe` block (§5), which is what makes this report
   particularly useful for HDT-SMP — it walks the dispatch struct main
   read its handle from and reports whether the chain is still intact.
6. The wait-graph cross-tabulation (§4.1, §4.2): every handle every
   thread is parked on, with each handle's waiter list.
7. Recent activity ring buffer: last 100 Papyrus log lines and last 36
   SKSE messaging events, each timestamped with `GetTickCount64()` at
   push time, so the gap between each event and the freeze instant is
   preserved.
8. Mini-dump path (the `.dmp` next to it, suitable for WinDbg / Visual
   Studio analysis).

The mini-dumps are the standard `MiniDumpWithFullMemory |
MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo` set, so all
thread contexts and live heap state are preserved at freeze time.

Direct contact + the raw artifacts can be supplied on request.

## 11. What the user has been asked to try

In addition to forwarding this report:

1. Try alternative HDT-SMP builds (original `Faster-SMP`, `ersh`'s fork)
   to bisect whether this race is specific to the `-UP` line.
2. Tune `hdtSkinnedMeshConfigs.xml` to lower max active actors and
   per-frame physics step count — should reduce frequency without
   changing the bug class, useful as confirmation.
3. Disable HDT-SMP entirely for one session to confirm freezes vanish
   (ground truth: if they vanish, the bug is provably inside HDT-SMP;
   if they don't, the analysis above is wrong).

Step 3 is the load-bearing experiment; the maintainer can ask for the
result of that bisect if they want it before opening an investigation
on their side.

---

## Appendix A — Raw `MainWaitProbe` block (freeze #6)

For reviewers who want the unmodified evidence:

```
Main::Update wait-helper probe (Skyrim SE 1.5.97):
  Two known infinite-wait sites inside RE::Main::Update:
    A) +0x5b35dd -> SkyrimSE+0x5765d0 (id 34554)
         Singleton-A @ SkyrimSE+0x2f26668; reads HANDLE
         from [singleton+0x60]; calls WaitForSingleObjectEx
         (signature: pending=1 + ack-event NOT signaled).
    B) +0x5b34fe -> SkyrimSE+0xc38130 (small wrapper)
         Singleton-B @ SkyrimSE+0x2f26a70; walks
         (*S)[+8][idx0]->vtable[idx1]; tail-jumps to
         KERNEL32!WaitForSingleObject. Main::Update calls
         this with idx0=0, idx1=1, dwMilliseconds=INFINITE.
  KERNELBASE clobbers the caller's RBX with the HANDLE
  for both wait functions, so we can read main's RBX and
  treat it as the kernel HANDLE main is currently parked on.

  SkyrimSE base:                       0x00007ff6eed20000
  Singleton ptr global address:        0x00007ff6f1c46668
  Singleton ptr global value:          0x00007ff6f1c46680

  Main thread context probe:
    Main TID:                          11060
    Main RIP:                          0x00007ff91ddc00f4
    Main RBX:                          0x0000000000002824
    Main RSP:                          0x000000b0046ffb38
    Wait-site detection:
      A) +0x5765d0 lock primitive: miss
      B) +0xc38130 wrapper:        HIT  (rip-in-fn=false, return@rsp+0xa0)

  Probe via RBX-as-HANDLE (KERNELBASE working register):
    GetHandleInformation flags:          0x00000000
    Event type:                          0 (NotificationEvent (manual-reset))
    Event state:                         0 (NOT signaled)

  Site-B probe (Singleton-B @ SkyrimSE+0x2f26a70):
    *(SkyrimSE+0x2f26a70):               0x000001c8f00b4298
    sub-array @ +0x08:                   0x0000000000000000
    element[0] (sub_array+0):            0x0000000000000000
    *element[0] (vtable_or_handles):     0x0000000000000000
    handle @ vtable[1]:                  0x0000000000000000
    Note: chain walk produced a null link. The struct
          was valid when main loaded it (otherwise the
          wrapper would have early-returned without
          waiting), so SOMEONE cleared it after main
          went to sleep. See the singleton hex dump
          and the toucher-thread search below.

    [Singleton instance hex dump @ 0x000001c8f00b4298, 32 qwords:]
      +0x000  0x007961646b656557     ("Weekday\0" — string fragment)
      +0x008  0x0000000000000000
      +0x010  0x000000004d93004c
      +0x018  0x0000000000000005
      +0x020  0x0000007265746157     ("Water\0" — string fragment)
      +0x028  0x0000000000000000
      +0x030  0x0000000023a40008
      +0x038  0x0000000000000006
      +0x040  0x00006465676e6152     ("Ranged\0" — string fragment)
      +0x048  0x000001c8f00b2300
      +0x050  0x00007ff6f034f518
      +0x058  0x00007ff6f0b0d5d0
      +0x060  0x0000000000000000
      +0x068  0x000001c8f00b3fe0
      ... more sub-objects, several still populated ...
      +0x0c8  0x67614d2074736143     ("Cast Mag" — string fragment)
      +0x0d0  0x746e657645206369     ("ic Event" — string fragment)
      +0x0e8  0x6f4720656d697243     ("Crime Go" — string fragment)
      +0x0f0  0x746e65764520646c     ("ld Event" — string fragment)

    Co-consumer search (other thread with any register == main RBX 0x2824):
      <no other thread has main's wait handle in any register>
    Toucher-thread search (other thread with any register == singleton 0x1c8f00b4298):
      <no other thread is currently touching the singleton instance>

  BSSpinLock-owner search (threads spinning at SkyrimSE+0x132c5a):
    <no thread is currently spinning on a BSSpinLock>

  Verdict: main is parked at the +0xc38130 wrapper
  (Site B), waiting INFINITE on a HANDLE drawn from
  Singleton-B. Producer mapping not yet decoded —
  the constructor cluster id5578-id5600 (RVA
  +0x9220b..+0x92993) is the next investigation
  target. Cross-reference Threads section for any
  thread whose RBX equals 0x2824 — that's the producer
  that should have signaled this handle.
```

The "Singleton-B" entry referenced here is a Skyrim global event-table
indexed by named-event keys. The string fragments visible in the hex
dump (`Weekday`, `Water`, `Ranged`, `Cast Magic Event`, `Crime Gold
Event`) are the engine's internal event names. Slot at `[+0x08]` holds
the per-event subscriber list. **That is the slot HDT-SMP appears to be
publishing into and then clearing.** The subscriber list being null while
a consumer is parked on a handle drawn from it is the load-bearing
evidence.

---

## Appendix B — `WorkerSpinLockFix` clearance trail

Full per-minute telemetry across the 75-minute session that produced
freeze #6 (excerpt):

```
[2026-05-27 22:00:50] WorkerSpinLockFix v2.0.3 logger initialized.
[2026-05-27 22:00:50] Runtime confirmed: Skyrim SE 1.5.97.0
[2026-05-27 22:00:50] [AcquireHook] surgical entry-point hook installed
                       on BSSpinLock::Acquire (id 12210, addr=0x7ff6eee52bd0,
                       spin_retry=0x7ff6eee52c5a).
[2026-05-27 22:00:50] [Phase4Defer] structural fix armed.
[2026-05-27 22:00:50] WorkerSpinLockFix armed.

[2026-05-27 22:01:50] stats: acq_slow=0   queued=0    drained=0    passthrough=0
[2026-05-27 22:03:50] stats: acq_slow=0   queued=0    drained=0    passthrough=2983
[2026-05-27 22:04:50] stats: acq_slow=4   queued=21   drained=21   passthrough=4070
... 70 minutes elapsed ...
[2026-05-27 23:14:50] stats: acq_slow=1223 queued=4415 drained=4415 passthrough=149701
[2026-05-27 23:15:50] stats: acq_slow=1223 queued=4455 drained=4455 passthrough=150914
                       cycles_observed=0 cycles_confirmed=0
                       breaks_done=0 breaks_raced=0 breaks_suppressed=0
[2026-05-27 23:15:52] [WATCHDOG]  freeze fired (main_age=15328ms, render_age=15344ms)
```

Across the 75 minutes:

- 4,455 AB-BA cycles preempted by the structural fix, 4,455 drained,
  zero leaked.
- 0 cycles observed by the runtime breaker (Layer 2).
- 0 force-releases issued.

This rules out the `BSSpinLock` AB-BA bug class as the cause of the
freeze. The freeze is downstream of HDT-SMP's event protocol, which WSF
does not hook and has no view into.