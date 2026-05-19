#!/usr/bin/env python3
"""
Scan unpacked SkyrimSE.exe for every RIP-relative reference to the
deadlocked singleton at SkyrimSE+0x2f26680 and its sibling tables.

Coordinates pinned by freeze_2026-05-17_211200 + capstone-confirmed
disassembly (NOT my earlier hand-math, which was off by 0x60):

  +0x2f26668  : qword pointer to singleton (lock primitive loads from here)
  +0x2f26680  : singleton struct
                  +0x60  HANDLE event       (= 0x2ac8 at freeze time)
                  +0x68  reserved/work-id   (cleared before each wait)
                  +0x6c  pending flag       (1 == wait scheduled)

Output:
  1. xrefs.txt  - every instruction whose RIP-rel disp lands in the region
  2. The producer-side functions are the ones that:
     (a) load the singleton ptr from +0x2f26668, and
     (b) write 1 into [singleton+0x6c] (set pending), or
     (c) call SetEvent(handle from [singleton+0x60]).
"""

import bisect
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

# Region of interest. ListenerTable_A/_B observed near 0x2f265e0 / 0x2f26648
# from id 34555 / 34556 disassembly. Singleton-A struct at +0x2f26680 was
# the deadlock site in 5/17–5/18 freezes; Singleton-B at +0x2f26a70 surfaced
# in freeze_2026-05-18_131625 (Main::Update +0x4f9 -> +0xc38130). Both live
# in SkyrimSE.exe .data; widen the scan to cover both clusters.
REGION_START = 0x2f26000
REGION_END   = 0x2f27500

HIGHLIGHTS = {
    0x2f265e0: "ListenerTable_A (13 ptrs, scanned by id 34555)",
    0x2f26648: "ListenerTable_B (13 ptrs, scanned by id 34556)",
    0x2f26668: "Singleton-A pointer slot (loaded by id 34554 lock primitive)",
    0x2f26680: "*** Singleton-A struct (5/17 deadlock) ***",
    0x2f26a70: "*** Singleton-B pointer slot (5/18 13:16 deadlock via +0xc38130) ***",
}

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
        roff  = struct.unpack_from("<I", data, s + 20)[0]
        rsize = struct.unpack_from("<I", data, s + 16)[0]
        sections.append((name, vaddr, vsize, roff, rsize))
    return data, image_base, sections


def find_text(sections):
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            return (va, vs, ro, rs)
    raise SystemExit("no main .text section found")


def label_for(target_rva):
    # Closest highlight wins, with offset annotation for in-struct hits.
    best = None
    for hl_rva, hl_note in HIGHLIGHTS.items():
        if abs(target_rva - hl_rva) <= 0x80:
            d = target_rva - hl_rva
            tag = f"{hl_note} +0x{d:x}" if d > 0 else hl_note
            if best is None or abs(d) < abs(best[0]):
                best = (d, tag)
    return best[1] if best else f"<unlabeled +0x{target_rva - REGION_START:x}>"


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x} size=0x{text_vs:x} ({len(text_bytes):,} bytes)",
          file=sys.stderr)
    print(f"[+] scanning region SkyrimSE+0x{REGION_START:x}..0x{REGION_END:x}",
          file=sys.stderr)

    _, _, entries = parse(ADDRLIB)
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    def nearest_sym(rva):
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            return ("?", 0, 0)
        return (f"id{ids_for_off[i]}", offsets[i], rva - offsets[i])

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail   = True
    md.skipdata = True   # don't bail on jump tables / data padding

    matches = []
    print("[+] linear disassembly starting...", file=sys.stderr)
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 1_000_000 == 0:
            print(f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                  file=sys.stderr)
        try:
            ops = ins.operands
        except capstone.CsError:
            continue   # skipdata pseudo-instruction, no operands
        for op in ops:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base != capstone.x86.X86_REG_RIP:
                continue
            if op.mem.index != capstone.x86.X86_REG_INVALID:
                continue
            target = ins.address + ins.size + op.mem.disp
            target_rva = target - image_base
            if REGION_START <= target_rva < REGION_END:
                rva = ins.address - image_base
                matches.append((rva, target_rva, ins.mnemonic, ins.op_str))
                break

    print(f"[+] {len(matches)} matching instructions\n", file=sys.stderr)

    # Group by target sub-region for at-a-glance summary.
    print("=" * 100)
    print("XREFS into SkyrimSE+0x2f26500..0x2f26780  (sorted by instruction RVA)")
    print("=" * 100)
    print(f"{'INSTR_RVA':<12s} {'TARGET':<12s}  {'NEAR_FN':<12s} +DELTA  TARGET_NOTE")
    print("-" * 100)
    matches.sort(key=lambda m: m[0])
    for rva, tgt_rva, mnem, op in matches:
        sym, fn_rva, delta = nearest_sym(rva)
        note = label_for(tgt_rva)
        print(f"0x{rva:<10x} 0x{tgt_rva:<10x}  {sym:<12s} +{delta:<5d}  {note}")
        print(f"             {mnem} {op}")

    # Functional summary: distinct nearest-symbol IDs that touch the region.
    print()
    print("=" * 100)
    print("DISTINCT FUNCTIONS that reference the region:")
    print("=" * 100)
    by_fn = {}
    for rva, tgt_rva, mnem, op in matches:
        sym, fn_rva, _ = nearest_sym(rva)
        by_fn.setdefault(sym, set()).add(tgt_rva)
    for sym in sorted(by_fn.keys(), key=lambda s: int(s[2:]) if s.startswith("id") else 0):
        tgts = sorted(by_fn[sym])
        print(f"  {sym:<14s}  ->  {', '.join(f'0x{t:x}' for t in tgts)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
