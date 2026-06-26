# 30. `LeakedSpinLockBreaker` — force-releasing a leaked `BSSpinLock`

**Date:** 2026-06-25 (diagnosis complete), field validation 2026-06-26
**Status:** IMPLEMENTED in `WorkerSpinLockFix` v2.6.0. Ships **detect-only**
by default; the force-release path is gated behind
`[leaked_spinlock_breaker] detect_only = false`. First field session with
active mode confirms the layer breaks the **hard-freeze** signature but does
**not** remove the underlying leak producer — the game can enter a
**stuck → released → stuck** loop while the trigger condition remains.
**Targets:** A *fifth* hard-freeze class — a heap `BSSpinLock` whose unlock
was **leaked** by an idle engine worker, distinct from the AB-BA inversion
(case-study 06 / `Phase4Defer`), the `WaitForJobTask` hang (27/28 /
`JobWaitBreaker`), and the main- or render-side Site-A worker-ack deadlocks
(29 / `SiteABreaker` / `SiteARenderBreaker`).
**Primary diagnostic capture:** `freeze_2026-06-25_222116_both.log`
(FreezeLogger v0.11.1, SE 1.5.97).

---

## 0. One-paragraph summary

When Faster HDT-SMP drives the engine's parallel cloth dispatch (`id 35565`),
the main thread can reach `BSSpinLock::Acquire` (`id 12210`) and spin forever
on a heap lock whose `[+0]` owner is a **different** thread: an idle-pool
worker parked in a kernel wait (`id 68058`, on the pool **Semaphore**). That
worker acquired the lock on an earlier task, returned to the top of its loop,
and went idle **without** releasing it — the unlock leaked. FreezeLogger
v0.11.1's steady-state probe proves the lock is frozen (owner/state unchanged
over 150 ms); the holder will never release it. `SetEvent` on any worker-ack
(Layers 2–4) cannot help. `LeakedSpinLockBreaker` (Layer 5) force-releases
the leaked lock after the same dwell + steady-state gates used elsewhere in
the plugin. In active mode this **unblocks** the contending thread but does
**not** fix whatever keeps leaking the lock; if the producer condition
persists, the session degrades to a **stutter loop** (freeze → release →
freeze) rather than a permanent hang — which can still be enough to move the
camera, leave the area, or identify a content trigger.

---

## 1. How this freeze class was isolated

### 1.1 Co-occurring symptoms in early captures

Several captures from 2026-06-24 onward showed **two** simultaneous stalls:

| Symptom | Site | Typical frames |
|---|---|---|
| Render thread parked in a Site-A join | `id 34557` / `id 34567` | Worker-ack `[+0x60]` never signalled |
| Main thread spinning on a heap `BSSpinLock` | `id 12210` via `id 35565` | Spin-retry return on stack; lock owner ≠ main |

FreezeLogger v0.9.0 added render-side Site-A classification (`RenderSiteAWorkerAck`).
`SiteARenderBreaker` (WSLF Layer 4, v2.5.0) targets the render join. Field
logs with Layer 4 in **active** mode showed the render path could be released
while **main still froze** spinning on the heap lock — proving the render-side
ack stall was a **co-victim**, not the sole root of main's hang.

### 1.2 Active recovery ruled out a simple lost-wakeup

When Layers 3–4 delivered `SetEvent` on worker-ack handles, sessions **stuttered**
then **re-froze** instead of recovering cleanly. That pattern is inconsistent
with a one-shot lost wakeup (where a single missing signal is the entire
failure) and consistent with a **lock-ownership** problem: unblocking one wait
site does not restore a leaked `BSSpinLock`.

### 1.3 FreezeLogger v0.10.0–v0.11.1 closed the diagnosis

| Version | Addition | Load-bearing result |
|---|---|---|
| v0.10.0 | BSSpinLock wait-for chain | Main contends lock `L`; owner `T₂` is parked in kernel wait while **still holding** `L` — "lock held across a thread park" |
| v0.10.1 | Module-based owner run-state | Parked workers no longer mislabelled "RUNNING"; owner engine frames show `id 68058` (pool idle) |
| v0.11.0 | Parked-holder detail | Lock object dump; register/stack handle scan |
| v0.11.1 | Type-aware wait scan + steady-state | Holder waits on pool **Semaphore** (not Event); lock owner/state **UNCHANGED** over 150 ms → **LEAKED** |

Representative verdict from `freeze_2026-06-25_222116`:

```
TID [MAIN] acquiring BSSpinLock 0x…3268, held by TID 136184 (state=1).
owner engine frames: … -> id 68058 -> …
[RCX] handle 0x68c: Semaphore
Steady-state: lock owner/state UNCHANGED over 150 ms — effectively LEAKED.
```

Waking the pool Semaphore makes a worker **look for work**; it does **not**
execute the missing unlock on a lock the worker already leaked.

---

## 2. Mechanism

`BSSpinLock` layout (SE 1.5.97):

```c
struct BSSpinLock { uint32_t threadID; uint32_t lockState; };  // [+0], [+4]
```

Normal acquire/release pairs update both fields. A **leak** occurs when a thread:

1. Acquires the lock (`owner = self`, `state = 1`).
2. Returns to the worker loop without reaching `BSSpinLock::Unlock`.
3. Parks in the idle pool (`id 68058`, waiting on the shared pool Semaphore).

The lock remains `state = 1` with a stale `owner` while the thread will never
touch that lock object again. Any other thread that needs the same lock spins
in `id 12210` indefinitely.

This is **not** the AB-BA inversion (`Phase4Defer`'s domain): there is no
two-lock cycle, only a single lock with a dead owner field and a live victim.

---

## 3. Why Layers 2–4 do not fix this

| Layer | Mechanism | Why it fails here |
|---|---|---|
| 2 `JobWaitBreaker` | `SetEvent` on job-pool wait handle | Main is not in `WaitForJobTask` |
| 3 `SiteABreaker` | `SetEvent` on Singleton-A worker-ack | Main is spinning on `BSSpinLock`, not parked in `id 34554` |
| 4 `SiteARenderBreaker` | `SetEvent` on render worker-ack | Unblocks render join only; main's spin on the heap lock remains |

Field captures with Layers 3–4 armed logged `stuck=0` or released the wrong
wait while FreezeLogger still reported main blocked on the leaked lock.

---

## 4. `LeakedSpinLockBreaker` — design and runtime behaviour

Implemented in `WorkerSpinLockFix` v2.6.0 as **Layer 5**. See
`../../skyrim-freeze-fix/src/LeakedSpinLockBreaker.cpp` and
`../../skyrim-freeze-fix/docs/design.md` §3.10.

### 4.1 Detection (no inline hook)

There is no single engine function to wrap: the failure is a **lock object
state** observed across threads. A watchdog thread:

1. Resolves one or more **spin-retry return addresses** inside
   `BSSpinLock::Acquire` via a version-independent `.text` signature (wildcarded
   `call [rip+d32]` displacement; same technique as FreezeLogger).
2. Each poll (~1 s default), suspends every process thread once, snapshots
   integer+control context, resumes.
3. Finds a **victim** whose stack contains a spin-retry return (contending a
   lock) and a **foreign lock** in registers/stack: `owner ≠ victim`,
   `state ≠ 0`.
4. Confirms the **owner** is parked in a system wait module (`ntdll`,
   `KERNELBASE`, …), is **not** itself spinning on another lock, and has been
   unchanged across **`dwell_threshold_ms`** (default 5000 ms).
5. Pauses **`recheck_window_ms`** (default 1500 ms) and **re-proves** the
   leak: owner RIP unchanged, lock word byte-identical.

### 4.2 Recovery (active mode)

When `detect_only = false`, the watchdog clears the lock with
`InterlockedCompareExchange64(lock, 0, expected_word)` — only if the 64-bit
`(owner, state)` pair is still exactly the proven-leaked value. If anything
changed in the last instant, the CAS is a no-op.

The victim thread (usually main) can then acquire the lock and proceed. The
leaker thread does not run again to release it; clearing the word is the only
recovery path.

### 4.3 Detect-only mode

When `detect_only = true` (default), the watchdog logs:

```
[LeakedSpinLockBreaker] LEAKED BSSpinLock detected (detect_only): …
WOULD force-release the lock here; set [leaked_spinlock_breaker] detect_only=false …
```

Engine state is not modified. The game remains in a **hard-freeze** until
force-killed.

### 4.4 Active mode — observed field behaviour

First field validation (2026-06-26, SE 1.5.97, Faster HDT-SMP, active mode):

**Hard-freeze is replaced by a stutter loop.** The game repeatedly:

1. Hangs while main (or another victim) spins on a leaked lock.
2. After dwell + recheck (~6.5 s with defaults), force-releases the lock and
   advances a few frames.
3. Re-enters the same leak condition if the underlying producer is still active.

From the player's perspective this is severe stuttering — not a clean recovery
— but it differs from detect-only / no Layer 5:

| Mode | Typical player experience |
|---|---|
| No Layer 5 / detect-only | Permanent hard-freeze; task manager required |
| Active Layer 5, leak producer still active | Stuck → brief run → stuck → …; camera / movement partially possible between releases |
| Active Layer 5, leak producer removed | Stutter stops; session stable |

The stutter loop is **expected** when the engine keeps leaking locks on every
cloth dispatch cycle. Layer 5 is a **symptom breaker**, not a root-cause fix
for the leak site inside the worker / HDT-SMP interaction.

### 4.5 What active mode does and does not do

**Does:**

- Break the **unrecoverable** hard-freeze when the steady-state leaked-lock
  signature is present.
- Allow enough forward progress to leave a trigger area, save, disable a mod,
  or narrow down a content-specific actor / cell.
- Confirm in logs (`leaked_lock: released=N` in periodic stats) that
  force-release fired.

**Does not:**

- Repair the worker path that failed to unlock.
- Prevent the next leak if the same dispatch pattern runs again.
- Guarantee visual or simulation correctness for the frames that run between
  releases (cloth/physics may be briefly inconsistent).
- Replace identifying and removing the **content or mod trigger** when one
  exists.

---

## 5. Content-trigger correlation (field finding)

During active-mode stutter, partial camera control made it possible to compare
frozen vs non-frozen sightlines. Disabling one actor reference eliminated all
recurrence:

| Field | Value |
|---|---|
| Actor name | Dervenin (Daedric quest NPC) |
| Base ID | `0001327C` |
| Ref ID | `000198DC` |

With that reference disabled, freezes stopped entirely **without** further
Layer 5 releases. This points to a **content-specific** trigger (that actor's
cloth/physics or script-driven dispatch) interacting with the HDT-SMP parallel
path — not a universal engine bug that fires on every load.

The mechanism behind *why* that reference leaks a lock is **not** established.
Layer 5 does not need that answer to unblock; removing or fixing the trigger
is the durable fix for that installation.

---

## 6. Configuration

```toml
[leaked_spinlock_breaker]
enabled = true

# false = active force-release; true = log only (hard-freeze persists)
detect_only = true

dwell_threshold_ms = 5000   # lock unchanged this long before candidate
poll_interval_ms = 1000     # thread-scan cadence
recheck_window_ms = 1500    # second proof before acting
diagnostic_logging = false
```

**Recommended rollout:**

1. Ship with `detect_only = true`; confirm
   `[LeakedSpinLockBreaker] LEAKED BSSpinLock detected` aligns with
   FreezeLogger's wait-for chain + steady-state LEAKED line.
2. Set `detect_only = false` only when repeated hard-freezes are unacceptable
   and stutter is an acceptable trade-off.
3. If stutter persists, treat Layer 5 as diagnostic aid: use the breathing room
   to find a mod/content trigger (disable Faster HDT-SMP for that actor class,
   remove the offending ref, etc.).

---

## 7. Relationship to other layers

```
Faster HDT-SMP cloth dispatch (id 35565)
  ├─ Main: BSSpinLock::Acquire (id 12210)  ──► Layer 5 (leaked lock)
  ├─ Main: Site-A ack wait (id 34554)      ──► Layer 3
  └─ Render: Site-A join (id 34557)          ──► Layer 4

Main: WaitForJobTask                       ──► Layer 2
Worker dispatcher AB-BA                    ──► Layer 1
```

A single session can hit multiple layers' detect-only warnings. Only Layer 5
addresses the leaked-lock signature on main.

---

## 8. Open questions

- Exact engine/HDT-SMP instruction path that skips `Unlock` before `id 68058`.
- Whether specific actor setups (skeleton, outfit, SMP config) widen the leak
  window — the Dervenin correlation is one data point.
- Optimal dwell/poll tuning to reduce stutter severity without increasing
  false-release risk (defaults are conservative).
- CTD reported in one session after switching **back** to `detect_only = true`
  following active mode — cause unconfirmed; may be unrelated to Layer 5.

---

## 9. References

- FreezeLogger CHANGELOG v0.10.0–v0.11.1 — wait-for chain, parked-holder,
  steady-state leak proof.
- Case-study 29 §6 — render-side Site-A co-freeze context.
- `../../skyrim-freeze-fix/src/LeakedSpinLockBreaker.{h,cpp}`
- `../../skyrim-freeze-fix/docs/design.md` §3.10
