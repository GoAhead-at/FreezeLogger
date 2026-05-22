#!/usr/bin/env python3
"""
Phase 1.5: probe what is at a single global RVA by enumerating all
RIP-relative reads, writes, and `lea`s touching it, then summarising
the access patterns.

Targets:
  0x2eff7d8 -- the 525-references-from-377-functions hot global near
               LockA. id 19369 compares its second arg `rdi` against
               this. Identifying this global often unlocks a known
               singleton accessor.
  0x2f8aef8 -- a global pointer slot loaded inside id 40285 at +0x38;
               candidate "external context" pointer (possibly a
               singleton accessor for the class instance that owns
               id 40285).
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
    0x2eff95c: "Phase 3.5: recursion-active flag inside id 19369 (only persistent global write); id 514953",
    0x2eff958: "Phase 3.5: 0x2eff95c - 4 (probe adjacent fields, 32-bit aligned)",
    0x2eff960: "Phase 3.5: 0x2eff95c + 4 (probe adjacent fields)",
    0x2eff950: "Phase 3.5: 0x2eff95c - 12 (probe further extent)",
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
    print(f"[+] image base 0x{image_base:x}", file=sys.stderr)

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

    # Per target: classify each access as READ / WRITE / LEA / OTHER
    # and group by enclosing function.
    accesses: dict[int, dict[str, dict[str, list[tuple[int, str, str]]]]] = {
        t: defaultdict(lambda: defaultdict(list)) for t in TARGETS
    }

    print("[+] linear disassembly...", file=sys.stderr)
    progress = 0
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

        for op_idx, op in enumerate(ops):
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base != capstone.x86.X86_REG_RIP:
                continue
            target_rva = ins.address + ins.size + op.mem.disp - image_base
            for tgt_base in TARGETS:
                if tgt_base <= target_rva < tgt_base + 8:
                    if ins.mnemonic == "lea":
                        access = "LEA"
                    elif ins.mnemonic in {"mov", "movq"} and op_idx == 0:
                        access = "WRITE"
                    elif ins.mnemonic in {"mov", "movq", "movzx", "movsx", "cmp", "cmpxchg",
                                          "test", "and", "or", "xor", "add", "sub", "inc", "dec",
                                          "shl", "shr", "rol", "ror", "bt", "bts", "btr"}:
                        access = "READ" if op_idx == 1 else "WRITE"
                    else:
                        access = "OTHER"
                    sym, _, _ = nearest_sym(ins.address - image_base)
                    accesses[tgt_base][access][sym].append(
                        (ins.address - image_base, ins.mnemonic, ins.op_str)
                    )
                    break
            break  # only count first mem operand per insn

    for tgt, note in TARGETS.items():
        per_target = accesses[tgt]
        total = sum(sum(len(v) for v in fns.values()) for fns in per_target.values())
        print()
        print("=" * 100)
        print(f"GLOBAL 0x{tgt:x}    ({note})    -- {total:,} access(es)")
        print("=" * 100)
        for access in ("WRITE", "LEA", "READ", "OTHER"):
            fns = per_target.get(access, {})
            if not fns:
                continue
            n_total = sum(len(v) for v in fns.values())
            print(f"\n  {access}  -- {n_total} site(s) across {len(fns)} function(s):")
            for sym in sorted(fns.keys(), key=lambda s: int(s[2:]) if s.startswith("id") else -1):
                hits = fns[sym]
                if access == "WRITE":
                    # Writes are usually rare and informative -- show every one.
                    print(f"    {sym}  ({len(hits)} writes)")
                    for rva, mnem, op in hits:
                        print(f"      @0x{rva:x}  {mnem} {op}")
                else:
                    # Reads/leas can be huge -- show only count and first example.
                    rva, mnem, op = hits[0]
                    print(f"    {sym:<14s}  {len(hits):>3d}  e.g. @0x{rva:x}  {mnem} {op}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
