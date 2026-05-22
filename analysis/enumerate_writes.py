#!/usr/bin/env python3
"""
Phase 3 task 1: enumerate every memory write inside each lock-cycle
acquirer function. Classify by base register so we can see which
fields of `this` (the ProcessLists / TESObjectREFR) and which globals
each acquirer mutates.

A "write" here is any instruction that uses an operand of form
`[reg + disp]` or `[rip + disp]` as a destination, including:
  - mov  [reg+disp], src
  - mov  [rip+disp], src
  - lock cmpxchg / xadd / inc / dec / and / or / xor on memory
  - movups / movaps / movsd / movss to memory
  - inc / dec / add / sub / and / or / xor on memory (RMW)

Stack-local writes (rsp/rbp) are ignored - they are uninteresting for
the data-structure-replacement question.

Per-function output:
  - GLOBAL writes   (rip-relative, with target RVA + nearest addrlib id)
  - INSTANCE writes (every other base reg, grouped by base reg)
  - ATOMIC ops      (lock-prefixed instructions, separately listed)
  - VTABLE calls    (read-only, but useful context for back-edges)
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

TARGETS = {
    19369: "id 19369  (LockA acquirer; non-virtual REFR-graph helper)",
    40285: "id 40285  (ProcessLists method, LockB)",
    40333: "id 40333  (ProcessLists method, LockB)",
    40334: "id 40334  (ProcessLists method, LockB)",
    40335: "id 40335  (ProcessLists method, LockB)",
}

# Mnemonics that write to the FIRST operand if it is a memory op.
# (For MOV-style instructions and RMW arithmetic, op0 is the destination.)
WRITES_TO_OP0 = {
    "mov", "movups", "movaps", "movdqa", "movdqu", "movsd", "movss",
    "movabs", "movsx", "movzx",
    "add", "sub", "and", "or", "xor", "inc", "dec",
    "shl", "shr", "rol", "ror", "sar",
    "neg", "not",
    "bts", "btr", "btc",
    # cmpxchg/xadd's destination is op0 (read-modify-write)
    "cmpxchg", "xadd",
    # SSE store/load (we treat any op0=mem case as a write below)
    "movnti", "movntdq", "movnti",
    # x87/FPU stores
    "fst", "fstp", "fistp",
}

ATOMIC_PREFIXES = {"lock"}
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


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    # Find .text bounds
    text_va = text_vs = text_ro = text_rs = 0
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            text_va, text_vs, text_ro, text_rs = va, vs, ro, rs
            break

    _, _, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    def nearest_sym(rva: int) -> str:
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            return "?"
        return f"id{ids_for_off[i]}+0x{rva - offsets[i]:x}"

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    # Find each function's size by looking at the next addrlib id's RVA
    # (functions usually end before the next id starts; otherwise heuristic ~0x4000 bytes).
    def fn_size(eid: int) -> int:
        rva = by_id[eid]
        i = bisect.bisect_right(offsets, rva)
        if i < len(offsets):
            return min(offsets[i] - rva, 0x4000)
        return 0x4000

    for eid, label in TARGETS.items():
        if eid not in by_id:
            print(f"\n!! {label}: id NOT in addrlib; skipping\n")
            continue
        fn_rva = by_id[eid]
        size = fn_size(eid)
        file_off = text_ro + (fn_rva - text_va)
        body = data[file_off:file_off + size]

        print()
        print("=" * 100)
        print(f"{label}    RVA 0x{fn_rva:x}   size 0x{size:x}")
        print("=" * 100)

        global_writes: list[tuple[int, str, int]] = []        # (offset, instr, target_rva)
        instance_writes: dict[str, list[tuple[int, int, str]]] = defaultdict(list)
        atomic_ops: list[tuple[int, str]] = []                # (offset, instr)
        vtable_calls: list[tuple[int, str, int]] = []         # (offset, "call/jmp", slot)

        for ins in md.disasm(body, image_base + fn_rva):
            offset = ins.address - (image_base + fn_rva)
            mnem = ins.mnemonic.lower()
            try:
                ops = ins.operands
            except capstone.CsError:
                continue

            # Atomic detection (Capstone exposes 'lock' as part of the prefix in some versions
            # via ins.bytes[0] == 0xF0; in others as part of mnemonic).
            has_lock_prefix = (
                bytes(ins.bytes[:1]) == b"\xf0"
                or any(b == 0xf0 for b in bytes(ins.bytes[:3]))
            )
            if has_lock_prefix:
                atomic_ops.append((offset, f"{mnem} {ins.op_str}"))

            # Vtable call/jmp dispatches: `call/jmp qword ptr [reg + disp]`
            if mnem in {"call", "jmp"} and ops:
                op = ops[0]
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_RIP and op.mem.disp >= 0:
                    vtable_calls.append((offset, mnem, op.mem.disp))
                continue  # never count call/jmp as a write

            # Writes: op0 must be memory and mnemonic must be in WRITES_TO_OP0.
            if mnem not in WRITES_TO_OP0 or not ops:
                continue
            op = ops[0]
            if op.type != capstone.x86.X86_OP_MEM:
                continue

            mem = op.mem
            base_reg = mem.base
            disp = mem.disp
            if base_reg == capstone.x86.X86_REG_RIP:
                target_rva = (ins.address - image_base) + ins.size + disp
                global_writes.append((offset, f"{mnem} {ins.op_str}", target_rva))
                continue
            base_name = ins.reg_name(base_reg) if base_reg != 0 else "?"
            if base_name in {"rsp", "rbp", "esp", "ebp"}:
                continue  # stack-local
            instance_writes[base_name].append((offset, disp, f"{mnem} {ins.op_str}"))

        # --- print summary ---
        if global_writes:
            print(f"\n  GLOBAL writes  ({len(global_writes)} site(s)):")
            for offset, instr, tgt in global_writes:
                print(f"    +0x{offset:<5x}  {instr:<60s}    -> 0x{tgt:x}  ({nearest_sym(tgt)})")
        else:
            print("\n  GLOBAL writes:  none")

        if instance_writes:
            print(f"\n  INSTANCE writes  (grouped by base register):")
            for base in sorted(instance_writes.keys()):
                writes = instance_writes[base]
                # Build a per-offset frequency table
                offsets_seen = sorted({(d, instr) for (_o, d, instr) in writes})
                print(f"    base reg = {base}  ({len(writes)} write(s); {len({d for (_o, d, _i) in writes})} distinct field(s))")
                shown_disps = set()
                for offset, disp, instr in writes:
                    if disp in shown_disps:
                        continue
                    shown_disps.add(disp)
                    sign = "+" if disp >= 0 else "-"
                    print(f"      [{base}{sign}0x{abs(disp):x}]   first-seen @ +0x{offset:<5x}   {instr}")
        else:
            print("\n  INSTANCE writes:  none")

        if atomic_ops:
            print(f"\n  ATOMIC ops  ({len(atomic_ops)} site(s)):")
            for offset, instr in atomic_ops:
                print(f"    +0x{offset:<5x}  {instr}")
        else:
            print("\n  ATOMIC ops:  none")

        if vtable_calls:
            slots = sorted({slot for (_o, _m, slot) in vtable_calls})
            counts = defaultdict(int)
            for (_o, _m, slot) in vtable_calls:
                counts[slot] += 1
            print(f"\n  VTABLE dispatches:  slots = " + ", ".join(f"+0x{s:x}({counts[s]}x)" for s in slots))

    return 0


if __name__ == "__main__":
    sys.exit(main())
