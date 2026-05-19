#!/usr/bin/env python3
"""Disassemble a single function in full to inspect lock semantics."""
import bisect
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB
from xref_calls import parse_pe

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
LOCK_A_RVA = 0x2eff8e0
LOCK_B_RVA = 0x2f3b8e8


def main() -> int:
    target_id = int(sys.argv[1])

    name, ptr_size, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    sorted_entries = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in sorted_entries]
    ids_for_off = [e[0] for e in sorted_entries]

    rva = by_id[target_id]
    idx = offsets.index(rva)
    next_rva = offsets[idx + 1] if idx + 1 < len(offsets) else rva + 0x800
    size = min(next_rva - rva, 0x1000)

    data, image_base, sections = parse_pe(EXE)

    def get_bytes(rva, length):
        for (n, va, vs, ro, rs) in sections:
            if va <= rva < va + vs:
                return data[ro + (rva - va):ro + (rva - va) + length]

    blob = get_bytes(rva, size)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    print(f"=== id {target_id}  RVA 0x{rva:x}  size 0x{size:x} ===\n")
    last_ret_rel = None
    for ins in md.disasm(blob, rva):
        rel = ins.address - rva
        tag = ""
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM:
                mem = op.value.mem
                if mem.base == capstone.x86.X86_REG_RIP:
                    target = ins.address + ins.size + mem.disp
                    if target == LOCK_B_RVA:
                        tag = "  <-- LockB.tid"
                    elif target == LOCK_B_RVA + 4:
                        tag = "  <-- LockB.state"
                    elif target == LOCK_A_RVA:
                        tag = "  <-- LockA.tid"
                    elif target == LOCK_A_RVA + 4:
                        tag = "  <-- LockA.state"
        # Detect specific patterns
        if ins.mnemonic == "lock":
            tag += "   ((((LOCK PREFIX))))"
        if "cmpxchg" in ins.mnemonic:
            tag += "   ((((CMPXCHG))))"
        print(f"  +0x{rel:<5x}  {ins.mnemonic:8s} {ins.op_str}{tag}")
        if ins.mnemonic == "ret":
            last_ret_rel = rel

    return 0


if __name__ == "__main__":
    sys.exit(main())
