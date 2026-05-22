# 01 - Overview

## Executive Summary

Over a nine-day period (May 14-22, 2026) we designed, built, and iteratively
extended a custom SKSE diagnostics plugin (`FreezeLogger`) to investigate
random hard freezes that occur intermittently in a heavily modded Skyrim SE
1.5.97 install (Nolvus Awakening), and then designed, built, and shipped a
companion fix plugin (`WorkerSpinLockFix`) in two versions: a v1.0 reactive
runtime breaker (2026-05-21) and a v2.0 structural fix (2026-05-22) that
preempts the cycle before it can form. The freezes were unreproducible from
any specific in-game action; they happened anywhere from a few minutes to an
hour into a session, the game would lock up with audio decay but no crash,
and recovery required force-killing the process.

We progressed through the following diagnostic rounds:

| Round | Date  | What changed in the plugin                                          | What we learned about the freeze |
|-------|-------|---------------------------------------------------------------------|----------------------------------|
| 0     | 05-14 | Spec, scaffold, dual heartbeat hooks, watchdog, snapshot writer     | Pipeline works on synthetic stalls. |
| 1     | 05-17 | First real freeze captured. Manual WinDbg analysis of stack frames. | Main thread parked in `WaitForSingleObjectEx` inside `RE::Main::Update`. |
| 2     | 05-17 | Added `MainWaitProbe` to read the wait-target singleton's fields.   | Identified Site A: `id 34554` lock primitive, "worker-ack" event never signaled. |
| 3     | 05-18 | Added BSSpinLock-owner search.                                      | Worker pool was spin-locked while main waited. |
| 4     | 05-18 | Added Site B detection (different wait wrapper at `SkyrimSE+0xc38130`). | A second, unrelated wait site exists in the same function. |
| 5     | 05-19 | Heuristic `BSSpinLock` candidate detector (no longer relies on RDI). | Found a four-thread cluster of spin holders/contenders. |
| 6     | 05-19 | Big batch: Address-Library symbolication, per-thread `NtQueryEvent`, full register dump, automated wait-graph, always-on minidump. | First freeze report that *alone* contains enough data to identify a specific lock cycle. |

The final report (`freeze_2026-05-19_120444_both.log`) showed unambiguous
evidence of an **AB-BA spinlock inversion** between two `BSSpinLock` globals
inside `SkyrimSE.exe`:

- **LockA** at `SkyrimSE+0x2eff8e0`, taken by `id 19369` via direct `lea`.
- **LockB** at `SkyrimSE+0x2f3b8e8`, taken by three non-virtual
  `RE::ProcessLists` methods identified during the v2.0 RE work:
  `id 40285` (`TransferBetweenTempChangeLists`-style traverser),
  `id 40333` (`AddToTempChangeList`), `id 40334`
  (`RemoveFromTempChangeList`).

(The original v1.0-era write-up in this section listed `id 40706` as a
LockB acquirer; Phase 1.5 of the v2.0 RE work disproved that - see
[`17-v2-phase1-5-findings.md`](17-v2-phase1-5-findings.md). `id 40706`
takes a per-instance lock at `[obj+0x150]` of a different class.
[`21-v2-phase4-prep-dispatch-decode.md`](21-v2-phase4-prep-dispatch-decode.md)
also reclassified `id 40335` as `BSSpinLock::Unlock` for LockB rather
than an acquirer.)

Two worker threads were each holding one lock and spinning for the other; two
more workers were tail-blocked behind them; main thread was blocked on the
worker-ack event because the worker that was supposed to signal it was one of
the spinners.

## Why this case study exists

This document captures the reasoning, tool choices, false starts, and the
final evidence chain so that:

1. **Future regressions are easier to handle** - the next time
   `FreezeLogger` reports a freeze, the reader knows the templates.
2. **The plugin's design decisions are justified.** Several of the more
   exotic features (heuristic lock-candidate scan, per-thread `NtQueryEvent`,
   wait-graph cross-cut) only made sense after a specific failed iteration;
   the rationale is recorded here.
3. **Anyone building a similar in-process post-mortem tool** can reuse the
   patterns: SEH-safe field readers, mutex-isolated DbgHelp walks, the
   "snapshot all suspended threads then resume" structure, etc.
4. **The mitigation discussion is grounded.** Without the static-analysis
   confirmation we could only have guessed at a fix; with the call-graph and
   the lock-pair localized to four `BSSpinLock` callers, the patch has a
   small surface area.

## How to read this case study

If you only have ten minutes:

- Skim `06-root-cause.md` (the AB-BA topology and disassembly).
- Glance at `08-mitigation.md`.
- Read `23-v2-release.md` for what shipped.

If you want the full story:

- Read `02-tools.md` then the chronological narrative in `03-timeline.md`,
  cross-referencing `04-plugin-evolution.md` to understand which diagnostic
  features were added when.
- Then `05-static-analysis.md` for how we located the locks once
  `FreezeLogger` told us where to look, and `06-root-cause.md` for the final
  picture.
- `07-discarded-hypotheses.md` is optional but documents the wrong turns -
  it is the easiest section to learn from for "what not to chase next time".

For the fix-plugin track:

- `11-worker-spinlockfix-retrospective.md` and
  `12-engine-fix-mod-audit.md` cover the v0.1 - v0.15 retracted iterations
  and the `safetyhook` adoption rationale.
- `13-rethought-solution.md` and `14-final-design-v1.md` are the v1.0
  design and final architecture (still installed in v2.0.0 as
  defence-in-depth).
- `15-v2-structural-strategy.md` -> `22-v2-phase4-1-cycle-hub-characterisation.md`
  document the four-phase RE work that produced the v2.0 structural fix.
- `23-v2-release.md` is the v2.0.0 release note - what shipped, what
  changed from v1.0, how to verify it works, what work remains open.

## What this case study is not

- It is not a Skyrim engine reference. We only reverse-engineered the
  functions and structures relevant to the freeze; everything else is
  unexplored.
- It is not a generic SKSE tutorial. It assumes familiarity with
  CommonLibSSE-NG, the Address Library, MO2 deployment, and basic Win32
  threading.
