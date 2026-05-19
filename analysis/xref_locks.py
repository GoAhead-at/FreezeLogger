#!/usr/bin/env python3
"""
Locate every RIP-relative reference to the two BSSpinLock globals
implicated in the 2026-05-19_120444 freeze:

    LockA  SkyrimSE+0x2eff8e0  (held by TID 5096,  awaited by TID 18456)
    LockB  SkyrimSE+0x2f3b8e8  (held by TID 18456, awaited by TID 5096)

Both addresses live in SkyrimSE.exe .data; their referents are 8-byte
BSSpinLock {threadID, lockState} structs. Goal of this scan is to find
the FUNCTIONS that take each lock so we can confirm the AB-BA inversion
and identify the mitigation hook site.
"""

import bisect
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

LOCK_A = 0x2eff8e0
LOCK_B = 0x2f3b8e8

# A single +/- 8-byte window per lock catches RIP-rel references to either
# the threadID slot (offset +0) or the lockState slot (offset +4); BSSpinLock
# is exactly 8 bytes wide.
TARGETS = {
    LOCK_A: "LockA  +0x2eff8e0",
    LOCK_B: "LockB  +0x2f3b8e8",
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


def label_for(rva):
    for base, note in TARGETS.items():
        if base <= rva < base + 8:
            d = rva - base
            return f"{note}{('' if d == 0 else f' +{d}')}"
    return f"<unlabeled 0x{rva:x}>"


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x} ({len(text_bytes):,} bytes)",
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
    md.skipdata = True

    matches = []
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 2_000_000 == 0:
            print(f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                  file=sys.stderr)
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
            for base in TARGETS:
                if base <= target_rva < base + 8:
                    matches.append((ins.address - image_base, target_rva,
                                    ins.mnemonic, ins.op_str))
                    break
            break

    print(f"[+] {len(matches)} matching instructions\n", file=sys.stderr)

    print("=" * 100)
    print("XREFS into LockA / LockB")
    print("=" * 100)
    print(f"{'INSTR_RVA':<12s} {'TARGET':<12s}  {'NEAR_FN':<12s} +DELTA  TARGET")
    print("-" * 100)

    matches.sort(key=lambda m: m[0])
    for rva, tgt_rva, mnem, op in matches:
        sym, fn_rva, delta = nearest_sym(rva)
        note = label_for(tgt_rva)
        print(f"0x{rva:<10x} 0x{tgt_rva:<10x}  {sym:<12s} +{delta:<5d}  {note}")
        print(f"             {mnem} {op}")

    print()
    print("=" * 100)
    print("DISTINCT FUNCTIONS that reference each lock:")
    print("=" * 100)
    for base, note in TARGETS.items():
        print(f"\n  {note} (RVA 0x{base:x}):")
        fns = {}
        for rva, tgt_rva, mnem, op in matches:
            if not (base <= tgt_rva < base + 8):
                continue
            sym, fn_rva, _ = nearest_sym(rva)
            fns.setdefault(sym, []).append((rva, tgt_rva, mnem, op))
        for sym in sorted(fns.keys(), key=lambda s: int(s[2:]) if s.startswith("id") else 0):
            sites = fns[sym]
            print(f"    {sym:<14s}  {len(sites)} site(s)")
            for rva, tgt_rva, mnem, op in sites:
                print(f"      @ 0x{rva:x}  +0x{tgt_rva-base}  {mnem} {op}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
