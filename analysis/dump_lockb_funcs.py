#!/usr/bin/env python3
"""Dump full disasm of the four LockB-touching functions to identify
which is acquire vs release."""
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB
from xref_calls import parse_pe

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
LOCK_B_RVA = 0x2f3b8e8

IDS = [40285, 40333, 40334, 40335]


def main() -> int:
    name, ptr_size, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    sorted_entries = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in sorted_entries]

    data, image_base, sections = parse_pe(EXE)

    def get_bytes(rva, length):
        for (n, va, vs, ro, rs) in sections:
            if va <= rva < va + vs:
                return data[ro + (rva - va):ro + (rva - va) + length]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    for fid in IDS:
        rva = by_id[fid]
        idx = offsets.index(rva)
        next_rva = offsets[idx + 1] if idx + 1 < len(offsets) else rva + 0x500
        size = next_rva - rva
        if size > 0x600:
            size = 0x600
        blob = get_bytes(rva, size)
        print(f"\n========================================================")
        print(f"id {fid}  RVA 0x{rva:x}  size 0x{size:x}")
        print(f"========================================================")
        for ins in md.disasm(blob, rva):
            rel = ins.address - rva
            tag = ""
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM:
                    mem = op.value.mem
                    if mem.base == capstone.x86.X86_REG_RIP:
                        target = ins.address + ins.size + mem.disp
                        if target == LOCK_B_RVA:
                            tag = "  <-- LockB"
                        elif target == LOCK_B_RVA + 4:
                            tag = "  <-- LockB.state"
            print(f"  +0x{rel:<4x}  {ins.mnemonic:7s} {ins.op_str}{tag}")
            if rel > size - 0x10 and ins.mnemonic == "ret":
                break

    return 0


if __name__ == "__main__":
    sys.exit(main())
