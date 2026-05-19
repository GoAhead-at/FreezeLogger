# 01 - Overview

## Executive Summary

Over a five-day period (May 14-19, 2026) we designed, built, and iteratively
extended a custom SKSE diagnostics plugin (`FreezeLogger`) to investigate
random hard freezes that occur intermittently in a heavily modded Skyrim SE
1.5.97 install (Nolvus Awakening). The freezes were unreproducible from any
specific in-game action; they happened anywhere from a few minutes to an hour
into a session, the game would lock up with audio decay but no crash, and
recovery required force-killing the process.

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
- **LockB** at `SkyrimSE+0x2f3b8e8`, taken by `id 40706` via `[arg+0x150]`
  and by `id 40333` via direct `lea`.

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

If you want the full story:

- Read `02-tools.md` then the chronological narrative in `03-timeline.md`,
  cross-referencing `04-plugin-evolution.md` to understand which diagnostic
  features were added when.
- Then `05-static-analysis.md` for how we located the locks once
  `FreezeLogger` told us where to look, and `06-root-cause.md` for the final
  picture.
- `07-discarded-hypotheses.md` is optional but documents the wrong turns -
  it is the easiest section to learn from for "what not to chase next time".

## What this case study is not

- It is not a Skyrim engine reference. We only reverse-engineered the
  functions and structures relevant to the freeze; everything else is
  unexplored.
- It is not a finished mitigation. As of 2026-05-19 the bug is identified
  but the fix plugin is still a proposal.
- It is not a generic SKSE tutorial. It assumes familiarity with
  CommonLibSSE-NG, the Address Library, MO2 deployment, and basic Win32
  threading.
