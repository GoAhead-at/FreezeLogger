#!/usr/bin/env python3
"""
Phase 1, Task 2: locate the constructor function(s) that write to a
singleton's base.

The constructor signature we're looking for:

    mov  qword ptr [rip + <singleton_base>], <vtable_RIP_rel>     ; write vtable
    or
    lea  rax, [rip + <vtable>]
    mov  qword ptr [rip + <singleton_base>], rax

Plus surrounding zero-initialisation of nearby fields.

For the LockB singleton at RVA 0x2f3b798 (LockB at +0x150), this
identifies the function that writes the vtable (the only sensible
"first 8-byte pointer" of a polymorphic object).

Output is every function in Address Library that contains a write to
`[singleton_base]`, with a contextual disasm snippet so we can see
whether it's the constructor or some other write.
"""

from __future__ import annotations

import bisect
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

# Singleton base address(es) to find constructors for.
TARGETS = {
    0x2f3b798: "LockB host (lock at +0x150, RVA 0x2f3b8e8)",
}

# Show this many bytes of context around the matched write.
CONTEXT_BEFORE = 0x40
CONTEXT_AFTER = 0x40

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


def is_write_to_target(ins, target_rva: int, image_base: int) -> bool:
    """Returns True if `ins` writes to a RIP-relative address inside [target, target+8]."""
    if ins.mnemonic not in {"mov", "movq", "movabs"}:
        return False
    try:
        ops = ins.operands
    except capstone.CsError:
        return False
    if not ops:
        return False
    dst = ops[0]
    if dst.type != capstone.x86.X86_OP_MEM:
        return False
    if dst.mem.base != capstone.x86.X86_REG_RIP:
        return False
    target = ins.address + ins.size + dst.mem.disp - image_base
    return target_rva <= target < target_rva + 8


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(
        f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x} ({len(text_bytes):,} bytes)",
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

    matches: dict[int, list[tuple[int, int, capstone.CsInsn]]] = {t: [] for t in TARGETS}

    print("[+] linear disassembly looking for writes...", file=sys.stderr)
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 4_000_000 == 0:
            print(
                f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                file=sys.stderr,
            )
        for target_rva in TARGETS:
            if is_write_to_target(ins, target_rva, image_base):
                # Compute target offset within the singleton.
                target = ins.address + ins.size + ins.operands[0].mem.disp - image_base
                offset = target - target_rva
                matches[target_rva].append((ins.address - image_base, offset, ins))
                break

    for target_rva, note in TARGETS.items():
        ms = matches[target_rva]
        print()
        print("=" * 100)
        print(f"WRITES to {note}  (RVA 0x{target_rva:x})")
        print("=" * 100)
        if not ms:
            print("  (no writes found - the singleton may be initialised elsewhere)")
            continue

        # Group by nearest function.
        by_fn: dict[str, list[tuple[int, int, capstone.CsInsn]]] = {}
        for rva, off, ins in ms:
            sym, _, delta = nearest_sym(rva)
            by_fn.setdefault(sym, []).append((rva, off, ins))

        for sym, hits in sorted(
            by_fn.items(),
            key=lambda kv: int(kv[0][2:]) if kv[0].startswith("id") else 1 << 30,
        ):
            print(f"\n  --- {sym}  ({len(hits)} write(s)) ---")
            for rva, off, ins in hits:
                print(
                    f"    @0x{rva:x}  +0x{off:x}  {ins.mnemonic:8s} {ins.op_str}"
                )

                # Disasm context: a few lines before and after the write.
                ctx_lo = max(0, rva - CONTEXT_BEFORE)
                ctx_hi = rva + CONTEXT_AFTER
                ctx_lo_off = (text_va + (ctx_lo - text_va))
                if ctx_lo < text_va or ctx_hi > text_va + len(text_bytes):
                    continue
                ctx_bytes = text_bytes[ctx_lo - text_va : ctx_hi - text_va]
                md_ctx = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
                md_ctx.detail = False
                md_ctx.skipdata = True
                for ci in md_ctx.disasm(ctx_bytes, image_base + ctx_lo):
                    rel = ci.address - image_base
                    marker = "**" if rel == rva else "  "
                    print(f"    {marker} 0x{rel:<10x}  {ci.mnemonic:8s} {ci.op_str}")
                print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
