#!/usr/bin/env python3
"""Disassemble id 19000 around +0x763 to find what it calls (and whether
it acquires LockB)."""
import bisect
import struct
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
    name, ptr_size, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    sorted_entries = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in sorted_entries]
    ids_for_off = [e[0] for e in sorted_entries]

    def nearest_id(rva):
        idx = bisect.bisect_right(offsets, rva) - 1
        if idx < 0:
            return None, 0
        return ids_for_off[idx], rva - offsets[idx]

    data, image_base, sections = parse_pe(EXE)

    def get_bytes(rva, length):
        for (n, va, vs, ro, rs) in sections:
            if va <= rva < va + vs:
                file_off = ro + (rva - va)
                return data[file_off:file_off + length]
        raise SystemExit(f"rva 0x{rva:x} not in any section")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    target_id = int(sys.argv[1]) if len(sys.argv) > 1 else 19000
    target_size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x900
    id19000 = by_id[target_id]
    blob = get_bytes(id19000, target_size)

    print(f"=== id {target_id} (RVA 0x{id19000:x}) - first 0x{target_size:x} bytes ===")
    lock_b_hits = []
    lock_a_hits = []
    calls = []
    for ins in md.disasm(blob, id19000):
        rel = ins.address - id19000
        # Direct CALL/JMP with imm operand
        if ins.mnemonic in ("call", "jmp"):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_IMM:
                    calls.append((rel, ins.mnemonic, op.imm, ins.size))
        # RIP-rel mem operands referencing LockA/LockB
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM:
                mem = op.value.mem
                if mem.base == capstone.x86.X86_REG_RIP:
                    target = ins.address + ins.size + mem.disp
                    if target == LOCK_B_RVA:
                        lock_b_hits.append((rel, ins.mnemonic, ins.op_str))
                    if target == LOCK_A_RVA:
                        lock_a_hits.append((rel, ins.mnemonic, ins.op_str))
        if rel > target_size - 0x100 and ins.mnemonic == "ret":
            break

    print(f"\nLockA RIP-rel refs in id 19000: {len(lock_a_hits)}")
    for rel, mnem, ops in lock_a_hits:
        print(f"  +0x{rel:<4x}  {mnem} {ops}")

    print(f"\nLockB RIP-rel refs in id 19000: {len(lock_b_hits)}")
    for rel, mnem, ops in lock_b_hits:
        print(f"  +0x{rel:<4x}  {mnem} {ops}")

    print(f"\nDirect CALLs from id {target_id} (first 0x{target_size:x} bytes):")
    for rel, mnem, tgt, size in calls:
        if rel > target_size:
            break
        sym_id, delta = nearest_id(tgt)
        marker = ""
        ret_addr = rel + size
        if 0x758 <= ret_addr <= 0x770:
            marker = "  <<< this CALL returns to id 19000 +0x763 (TID 25832 frame)"
        print(f"  +0x{rel:<4x}  {mnem} -> 0x{tgt:x} (id {sym_id} +0x{delta:x}){marker}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
