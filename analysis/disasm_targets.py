#!/usr/bin/env python3
"""
Disassemble the function entry points we identified via Address Library.

Reads bytes directly from the *unpacked* SkyrimSE.exe (Steam-stub stripped),
maps RVA -> file offset via the PE section table, and disassembles N bytes
at each RVA using capstone. Highlights interesting calls (kernel32 imports,
self-calls, spinwait patterns, cmpxchg/xchg etc.).
"""

import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")

TARGETS = [
    (0x132bd0, "BSSpinLock::Acquire — find which register holds lock ptr"),
]

CALL_SITES = []


def parse_pe(path: Path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise SystemExit("not a PE file")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit("PE sig missing")
    coff_off = pe_off + 4
    num_sections = struct.unpack_from("<H", data, coff_off + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff_off + 16)[0]
    opt_off = coff_off + 20
    image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]  # PE32+ ImageBase
    sections_off = opt_off + opt_size
    sections = []
    for i in range(num_sections):
        s = sections_off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("ascii", errors="replace")
        vsize = struct.unpack_from("<I", data, s + 8)[0]
        vaddr = struct.unpack_from("<I", data, s + 12)[0]
        rsize = struct.unpack_from("<I", data, s + 16)[0]
        roff = struct.unpack_from("<I", data, s + 20)[0]
        sections.append((name, vaddr, vsize, roff, rsize))
    return data, image_base, sections


def rva_to_offset(rva, sections):
    for (_n, va, vs, ro, rs) in sections:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    print(f"[+] image base: 0x{image_base:x}", file=sys.stderr)
    for (n, va, vs, ro, rs) in sections:
        print(f"  section {n:8s} va=0x{va:08x} vs=0x{vs:08x} ro=0x{ro:08x} rs=0x{rs:08x}", file=sys.stderr)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    print("\n========== TARGET FUNCTIONS ==========\n")
    for rva, note in TARGETS:
        off = rva_to_offset(rva, sections)
        if off is None:
            print(f"-- 0x{rva:x}  ??? RVA not in any section ({note})\n")
            continue
        chunk = data[off:off + 0x800]   # 2048 bytes per fn
        addr = image_base + rva
        print(f"-- 0x{rva:x}  ({note}); file off=0x{off:x}, vaddr=0x{addr:x}")
        for i, ins in enumerate(md.disasm(chunk, addr)):
            print(f"   0x{ins.address - image_base:08x}  {ins.bytes.hex():<24s}  {ins.mnemonic:<8s} {ins.op_str}")
            if i >= 320:
                break
        print()

    print("\n========== CALL SITES ==========\n")
    for site_rva, fn_rva, note in CALL_SITES:
        off = rva_to_offset(fn_rva, sections)
        if off is None:
            print(f"-- {note}: function at 0x{fn_rva:x} not found")
            continue
        # Dump 32 bytes before and after the site
        site_off = off + (site_rva - fn_rva)
        before = max(0, site_off - 16)
        chunk = data[before:site_off + 64]
        start_va = image_base + (fn_rva + (before - off))
        print(f"-- site 0x{site_rva:x}  ({note}); file off=0x{site_off:x}")
        for ins in md.disasm(chunk, start_va):
            marker = "  <==" if (ins.address - image_base) == site_rva else ""
            print(f"   0x{ins.address - image_base:08x}  {ins.bytes.hex():<24s}  {ins.mnemonic:<8s} {ins.op_str}{marker}")
            if ins.address - image_base > site_rva + 32:
                break
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
