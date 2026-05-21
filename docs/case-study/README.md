# Skyrim SE 1.5.97 Hard-Freeze Investigation - Case Study

**Project:** FreezeLogger SKSE Plugin (+ WorkerSpinLockFix companion)
**Period:** 2026-05-14 to 2026-05-21
**Modlist:** Nolvus Awakening (Skyrim SE 1.5.97)
**Outcome:** Root cause identified (AB-BA spinlock inversion in Skyrim's
worker dispatcher) and a working runtime fix shipped as
`WorkerSpinLockFix` v1.0.0.

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
- `10-future-approaches.md` - Forward-looking summary of what was actually
  tried in the WorkerSpinLockFix companion plugin (v0.1 - v0.11), why each
  version failed, and what realistic paths remain.
- `11-worker-spinlockfix-retrospective.md` - Current decision record for
  WorkerSpinLockFix v0.1 - v0.15: what was tried, how each attempt behaved in
  real freeze/crash logs, and the rules that prevent repeating the same
  serialization/enumeration mistakes.
- `12-engine-fix-mod-audit.md` - Audit of two public open-source engine-fix
  mods (`EngineFixesSkyrim64` by aers, `po3-Tweaks` by powerof3): what they
  do and do not contain regarding `BSSpinLock` / dispatcher / worker
  internals, and what concrete tooling improvements (notably `safetyhook`)
  the audit produces for our project.
- `13-rethought-solution.md` - Design proposal for `WorkerSpinLockFix v1.0`,
  built on the constraints from documents 11 and 12: sub-millisecond
  ID-independent cycle detection via a `safetyhook` entry-point hook on
  `BSSpinLock::Acquire`, with the stale-owner reaper retained as a safety
  net and a parallel research track on lock-bypass via data-structure
  replacement held in reserve.
- `14-final-design-v1.md` - Final architecture as actually shipped in
  `WorkerSpinLockFix v1.0.0`. Records the two new lock-ordering
  regressions discovered during bring-up (heap allocation inside the
  detour, `safetyhook::call<>`'s internal `std::recursive_mutex`), the
  time-based confirmation flow that replaced observation-counting (and
  fixed a real production gap for clean 2-thread AB-BA cycles), and the
  synthetic AB-BA harness that proves the breaker end-to-end without
  waiting for a real engine cycle.
- `appendix-A-evidence.md` - Raw freeze logs, disassembly excerpts, register
  dumps used as evidence in the main narrative.

If you read top-to-bottom these files reconstruct the entire investigation
without needing the agent transcripts.
