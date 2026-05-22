#!/usr/bin/env python3
"""
Phase 1.5: find every direct caller of one or more target RVAs and
dump the preceding instructions so we can see how `rcx`/`rdx`/`r8`/`r9`
are set up at the call site.

Goal: if every caller of `id 40285` (RVA 0x6d37b0) loads `rcx` from
the same global pointer slot, that slot is the class's
`GetSingleton()` storage -- i.e., we have the class identity.
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
    0x6d37b0: "id 40285 (LockB acquirer #1)",
    0x6d9720: "id 40333 (LockB acquirer #2)",
    0x6d9890: "id 40334 (LockB acquirer #3)",
    0x6d99d0: "id 40335 (LockB acquirer #4)",
    0x6ef230: "id 40706 (candidate LockB acquirer via [r14+0x150])",
    0x296c00: "id 19369 (sole LockA acquirer)",
}

CONTEXT_INSNS_BEFORE = 10

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
    print(
        f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x}",
        file=sys.stderr,
    )

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

    target_addrs = {image_base + rva for rva in TARGETS}

    # First pass: collect all instructions into a list so we can index
    # backward for context. Also collect call matches.
    print("[+] linear disassembly (collecting instructions)...", file=sys.stderr)
    insns: list[tuple[int, str, str]] = []  # (addr, mnem, op_str)
    matches: dict[int, list[int]] = defaultdict(list)  # target_rva -> [insn_index]
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        insns.append((ins.address, ins.mnemonic, ins.op_str))
        progress += 1
        if progress % 4_000_000 == 0:
            print(
                f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                file=sys.stderr,
            )
        if ins.mnemonic != "call":
            continue
        try:
            ops = ins.operands
        except capstone.CsError:
            continue
        if not ops or ops[0].type != capstone.x86.X86_OP_IMM:
            continue
        if ops[0].imm in target_addrs:
            target_rva = ops[0].imm - image_base
            matches[target_rva].append(len(insns) - 1)

    print(f"[+] {sum(len(v) for v in matches.values())} direct call sites total\n", file=sys.stderr)

    for target_rva, note in TARGETS.items():
        sites = matches.get(target_rva, [])
        print("=" * 100)
        print(f"DIRECT CALLERS of  RVA 0x{target_rva:x}    ({note})    -- {len(sites)} site(s)")
        print("=" * 100)
        if not sites:
            print("  (no direct callers found - this function is called only indirectly, "
                  "e.g. via vtable/function pointer)\n")
            continue

        # Group call sites by enclosing function for a tidy summary.
        by_fn: dict[str, list[int]] = defaultdict(list)
        for idx in sites:
            call_rva = insns[idx][0] - image_base
            sym, _, _ = nearest_sym(call_rva)
            by_fn[sym].append(idx)

        for sym in sorted(by_fn.keys(), key=lambda s: int(s[2:]) if s.startswith("id") else -1):
            for idx in by_fn[sym]:
                call_addr, mnem, op_str = insns[idx]
                call_rva = call_addr - image_base
                fn_sym, fn_rva, fn_delta = nearest_sym(call_rva)
                print(f"\n  --- caller {fn_sym} (call at 0x{call_rva:x} = {fn_sym}+0x{fn_delta:x}) ---")

                lo = max(0, idx - CONTEXT_INSNS_BEFORE)
                for i in range(lo, idx + 1):
                    addr, m, o = insns[i]
                    rel = addr - image_base
                    marker = "**" if i == idx else "  "
                    print(f"    {marker} 0x{rel:<10x}  {m:8s} {o}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
