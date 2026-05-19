#!/usr/bin/env python3
"""Disassemble N bytes starting at a given RVA in SkyrimSE.exe.

Usage:
  python analysis/dump_at_rva.py 0xC44AD0 0x40
"""
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from xref_calls import parse_pe

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    rva  = int(sys.argv[1], 0)
    size = int(sys.argv[2], 0)

    data, image_base, sections = parse_pe(EXE)
    text = next(s for s in sections if s[0] == ".text" and s[2] > 0x100000)
    _, va, _, raw_off, raw_size = text
    text_blob = data[raw_off:raw_off + raw_size]

    blob_off = rva - va
    if blob_off < 0 or blob_off + size > len(text_blob):
        print(f"RVA 0x{rva:x} is outside .text", file=sys.stderr)
        return 1

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    chunk = text_blob[blob_off:blob_off + size]
    print(f"=== RVA 0x{rva:x}  size 0x{size:x} ===\n")
    for ins in md.disasm(chunk, image_base + rva):
        rva_local = ins.address - image_base
        bytes_hex = " ".join(f"{b:02x}" for b in ins.bytes)
        print(f"  0x{rva_local:08x}  {bytes_hex:30s}  {ins.mnemonic:8s} {ins.op_str}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
