#!/usr/bin/env python3
"""
Phase 1, Task 1: locate the singleton base address that hosts each of the
two known deadlock spinlocks.

    LockA  SkyrimSE+0x2eff8e0
    LockB  SkyrimSE+0x2f3b8e8

Strategy
--------
A BSSpinLock is 8 bytes wide and lives as a field inside some singleton
struct. The struct is referenced from many sites in .text via
RIP-relative `lea`s. Most of those sites compute `&singleton + offset`
for various fields (vtable, queue head, count, the lock, etc.).

For each lock, we scan a window `[lock - 0x800, lock + 8]` and collect
EVERY RIP-relative reference whose target lands in that window. We then
group the targets into clusters and rank candidate bases by:

  1. Number of references at exactly that base RVA (vtable / top-of-struct
     accesses are usually the most frequent).
  2. Number of distinct functions referencing addresses in
     [base, base + 0x800].
  3. Whether the lock RVA falls at a plausible struct offset
     (typical: 0x08, 0x10, 0x18, 0x20, 0x40).

The most-referenced "tightly-bunched" base in the window is the
singleton's start address.

Output is grouped per lock and tagged with confidence reasoning.
"""

from __future__ import annotations

import bisect
import struct
import sys
from collections import defaultdict
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

LOCK_A = 0x2eff8e0
LOCK_B = 0x2f3b8e8

LOCKS = {
    "LockA": LOCK_A,
    "LockB": LOCK_B,
}

WINDOW_BEFORE = 0x800
WINDOW_AFTER = 8

PLAUSIBLE_LOCK_OFFSETS = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60}

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse


def parse_pe(path: Path):
    data = path.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    coff_off = pe_off + 4
    num_sections = struct.unpack_from("<H", data, coff_off + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff_off + 16)[0]
    opt_off = coff_off + 20
    image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]
    sections_off = opt_off + opt_size
    sections = []
    for i in range(num_sections):
        s = sections_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", errors="replace")
        vaddr = struct.unpack_from("<I", data, s + 12)[0]
        vsize = struct.unpack_from("<I", data, s + 8)[0]
        roff = struct.unpack_from("<I", data, s + 20)[0]
        rsize = struct.unpack_from("<I", data, s + 16)[0]
        sections.append((name, vaddr, vsize, roff, rsize))
    return data, image_base, sections


def find_text(sections):
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            return (va, vs, ro, rs)
    raise SystemExit("no main .text section found")


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(
        f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x} ({len(text_bytes):,} bytes)",
        file=sys.stderr,
    )

    _, _, entries = parse(ADDRLIB)
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    def nearest_sym(rva: int):
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            return ("?", 0, 0)
        return (f"id{ids_for_off[i]}", offsets[i], rva - offsets[i])

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True

    # Collect RIP-rel references whose target falls inside ANY lock's window.
    ranges = [
        (lock - WINDOW_BEFORE, lock + WINDOW_AFTER, name, lock)
        for name, lock in LOCKS.items()
    ]

    matches: list[tuple[str, int, int, int, str, str]] = []
    progress = 0
    print("[+] linear disassembly...", file=sys.stderr)
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 4_000_000 == 0:
            print(
                f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                file=sys.stderr,
            )
        try:
            ops = ins.operands
        except capstone.CsError:
            continue
        for op in ops:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base != capstone.x86.X86_REG_RIP:
                continue
            if op.mem.index != capstone.x86.X86_REG_INVALID:
                continue
            target_rva = ins.address + ins.size + op.mem.disp - image_base
            for win_lo, win_hi, win_name, win_lock in ranges:
                if win_lo <= target_rva <= win_hi:
                    matches.append(
                        (
                            win_name,
                            ins.address - image_base,
                            target_rva,
                            win_lock,
                            ins.mnemonic,
                            ins.op_str,
                        )
                    )
                    break
            break

    print(f"[+] {len(matches):,} matching instructions across all windows\n", file=sys.stderr)

    # Per-lock candidate ranking.
    for win_name, win_lock in LOCKS.items():
        win_lo = win_lock - WINDOW_BEFORE
        win_hi = win_lock + WINDOW_AFTER
        sub = [m for m in matches if m[0] == win_name]

        print("=" * 100)
        print(f"{win_name}  RVA 0x{win_lock:x}    window [0x{win_lo:x}, 0x{win_hi:x}]    refs: {len(sub):,}")
        print("=" * 100)

        # Histogram of distinct target RVAs.
        target_hist: dict[int, list[tuple[int, str, str]]] = defaultdict(list)
        for _, ins_rva, tgt_rva, _, mnem, op in sub:
            target_hist[tgt_rva].append((ins_rva, mnem, op))

        # Distinct functions referencing each target.
        target_fns: dict[int, set[str]] = defaultdict(set)
        for _, ins_rva, tgt_rva, _, _, _ in sub:
            sym, _, _ = nearest_sym(ins_rva)
            target_fns[tgt_rva].add(sym)

        sorted_targets = sorted(
            target_hist.keys(),
            key=lambda t: (-len(target_hist[t]), -len(target_fns[t]), t),
        )

        print(f"\n  TARGET HISTOGRAM (sorted by ref count, then distinct fn count):\n")
        print(f"  {'TARGET_RVA':<14s} {'#REFS':>7s} {'#FNS':>6s}  {'LOCK_OFF':>10s}  EXAMPLES")
        print(f"  {'-' * 90}")
        for t in sorted_targets[:30]:
            lock_off = win_lock - t
            lock_off_marker = ""
            if lock_off in PLAUSIBLE_LOCK_OFFSETS:
                lock_off_marker = " *plausible-singleton*"
            elif lock_off == 0:
                lock_off_marker = " <-- the lock itself"
            elif lock_off < 0:
                lock_off_marker = " (above the lock)"
            example = target_hist[t][0]
            ex_str = f"@0x{example[0]:x}: {example[1]} {example[2][:50]}"
            print(
                f"  0x{t:<12x} {len(target_hist[t]):>7d} {len(target_fns[t]):>6d}  +0x{lock_off:<8x}{lock_off_marker}\n      {ex_str}"
            )

        # Candidate-base ranking. A base candidate is a target RVA at a
        # plausible offset from the lock, with the highest aggregate refs
        # in [base, base + 0x800].
        print(f"\n  CANDIDATE SINGLETON BASES (ref counts aggregated over [base, base+0x200]):\n")

        candidates = []
        for t in sorted_targets:
            if t == win_lock:
                # The lock itself isn't a base.
                continue
            if t > win_lock:
                # Above-the-lock targets are unlikely to be the base.
                continue
            agg_refs = 0
            agg_fns: set[str] = set()
            agg_targets: list[int] = []
            for tt in sorted_targets:
                if t <= tt < t + 0x200:
                    agg_refs += len(target_hist[tt])
                    agg_fns.update(target_fns[tt])
                    agg_targets.append(tt)
            lock_off = win_lock - t
            plausible = lock_off in PLAUSIBLE_LOCK_OFFSETS
            score = agg_refs * (3 if plausible else 1) * (1 + len(agg_fns))
            candidates.append(
                (score, t, lock_off, plausible, agg_refs, len(agg_fns), agg_targets)
            )

        candidates.sort(key=lambda c: -c[0])

        print(
            f"  {'SCORE':>8s}  {'BASE':<14s} {'LOCK_OFF':>10s} {'PLAUS':>6s} {'AGG_REFS':>9s} {'AGG_FNS':>9s}  IN-STRUCT TARGETS"
        )
        print(f"  {'-' * 100}")
        for score, t, lock_off, plausible, agg_refs, agg_fns, agg_tgts in candidates[:10]:
            tgt_str = ", ".join(f"+0x{tt - t:x}" for tt in agg_tgts[:10])
            print(
                f"  {score:>8d}  0x{t:<12x} +0x{lock_off:<8x} {('YES' if plausible else 'no'):>6s} "
                f"{agg_refs:>9d} {agg_fns:>9d}  [{tgt_str}]"
            )

        # Functions touching the top candidate.
        if candidates:
            top_score, top_base, top_lock_off, top_plaus, _, _, top_tgts = candidates[0]
            print(f"\n  TOP CANDIDATE: 0x{top_base:x}  (lock at +0x{top_lock_off:x}, plausible={top_plaus})")
            top_fns: dict[str, list[tuple[int, int, str, str]]] = defaultdict(list)
            for _, ins_rva, tgt_rva, _, mnem, op in sub:
                if top_base <= tgt_rva < top_base + 0x200:
                    sym, _, _ = nearest_sym(ins_rva)
                    top_fns[sym].append((ins_rva, tgt_rva, mnem, op))
            print(f"  Functions referencing the top candidate:")
            for sym in sorted(
                top_fns.keys(),
                key=lambda s: int(s[2:]) if s.startswith("id") else -1,
            ):
                offsets_touched = sorted({tg - top_base for _, tg, _, _ in top_fns[sym]})
                off_str = ", ".join(f"+0x{o:x}" for o in offsets_touched)
                print(f"    {sym:<14s}  {len(top_fns[sym]):>3d} refs  fields: [{off_str}]")

        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
