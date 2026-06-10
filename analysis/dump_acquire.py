#!/usr/bin/env python3
"""Disassemble BSSpinLock::Acquire (id 12210) to locate the spin-retry site.

Standalone variant of dump_one_func.py that points at analysis/se and
annotates branch targets so we can see the spin loop and which register
holds the lock pointer (`self`) at the +0x8a retry site.
"""
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB
from xref_calls import parse_pe

EXE = Path(__file__).parent / "se" / "SkyrimSE.exe.unpacked.exe"


def main() -> int:
    target_id = int(sys.argv[1]) if len(sys.argv) > 1 else 12210

    name, ptr_size, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    sorted_entries = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in sorted_entries]

    rva = by_id[target_id]
    idx = offsets.index(rva)
    next_rva = offsets[idx + 1] if idx + 1 < len(offsets) else rva + 0x800
    size = min(next_rva - rva, 0x400)

    data, image_base, sections = parse_pe(EXE)

    def get_bytes(rva, length):
        for (n, va, vs, ro, rs) in sections:
            if va <= rva < va + vs:
                return data[ro + (rva - va):ro + (rva - va) + length]

    blob = get_bytes(rva, size)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    print(f"=== id {target_id}  RVA 0x{rva:x}  size 0x{size:x}  (entry+0x8a = 0x{rva + 0x8a:x}) ===\n")
    # First pass: collect branch targets (relative jmps/jcc) within the func.
    branch_targets = set()
    insns = list(md.disasm(blob, rva))
    for ins in insns:
        if ins.group(capstone.x86.X86_GRP_JUMP):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_IMM:
                    branch_targets.add(op.value.imm)

    for ins in insns:
        rel = ins.address - rva
        marks = []
        if ins.address in branch_targets:
            marks.append("<== branch target")
        if rel == 0x8a:
            marks.append("################ +0x8a SPIN-RETRY HOOK SITE")
        if ins.mnemonic == "lock":
            marks.append("(LOCK PREFIX)")
        if "cmpxchg" in ins.mnemonic:
            marks.append("(CMPXCHG)")
        if ins.mnemonic in ("call",):
            marks.append("(CALL)")
        if ins.mnemonic == "pause":
            marks.append("(PAUSE)")
        tag = ("   " + "  ".join(marks)) if marks else ""
        print(f"  +0x{rel:<5x} 0x{ins.address:<8x}  {ins.mnemonic:10s} {ins.op_str}{tag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
