# Skyrim SE 1.5.97 Hard-Freeze Investigation - Case Study

**Project:** FreezeLogger SKSE Plugin
**Period:** 2026-05-14 to 2026-05-19
**Modlist:** Nolvus Awakening (Skyrim SE 1.5.97)
**Outcome:** Root cause identified - AB-BA spinlock inversion in Skyrim's worker dispatcher.

This folder contains the long-form case study describing how a custom SKSE
diagnostics plugin was designed, iterated on across multiple real freezes,
and ultimately used to localize a deadlock to two specific functions inside
`SkyrimSE.exe`.

## Files

- `01-overview.md` - Executive summary and problem statement.
- `02-tools.md` - Cast of tools used.
- `03-timeline.md` - Chronological investigation timeline.
- `04-plugin-evolution.md` - How the plugin's diagnostic capabilities grew.
- `05-static-analysis.md` - Ghidra / capstone / Address Library work.
- `06-root-cause.md` - The AB-BA deadlock with full evidence.
- `07-discarded-hypotheses.md` - What we were wrong about and why.
- `08-mitigation.md` - Options for fixing or working around the bug.
- `09-lessons-learned.md` - Takeaways for future engine debugging.
- `appendix-A-evidence.md` - Raw freeze logs, disassembly excerpts, register
  dumps used as evidence in the main narrative.

If you read top-to-bottom these files reconstruct the entire investigation
without needing the agent transcripts.
