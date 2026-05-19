#!/usr/bin/env python3
"""
Find every direct CALL instruction (E8 imm32) whose target is the entry
point of id 19369 (RVA 0x296c00) or id 40706 (RVA 0x6ef230).

The output is the list of CALL site RVAs in SkyrimSE.exe that the
WorkerSpinLockFix plugin must hook with `write_call<5>` to wrap entry
to those two functions.

Indirect calls (vtable, register-relative) are NOT detected. If any
such call site exists for either function it is silently missed.
"""

import bisect
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

TARGETS = {
    0x132bd0: "id 12210",   # BSSpinLock::Acquire
    0xc075a0: "id 66983",   # BSSpinLock::Release
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
        roff  = struct.unpack_from("<I", data, s + 20)[0]
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
    print(f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x} ({len(text_bytes):,} bytes)",
          file=sys.stderr)

    _, _, entries = parse(ADDRLIB)
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    def nearest_sym(rva):
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            return ("?", 0, 0)
        return (f"id{ids_for_off[i]}", offsets[i], rva - offsets[i])

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True

    matches = {tgt: [] for tgt in TARGETS}
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 2_000_000 == 0:
            print(f"    [{progress:,} insns, RVA 0x{ins.address - image_base:x}]",
                  file=sys.stderr)
        # E8 cd / direct CALL rel32  OR  E9 cd / direct JMP rel32 (tail call)
        if ins.mnemonic not in ("call", "jmp"):
            continue
        if len(ins.bytes) != 5:
            continue
        if ins.bytes[0] not in (0xE8, 0xE9):
            continue
        try:
            ops = ins.operands
        except capstone.CsError:
            continue
        if len(ops) != 1 or ops[0].type != capstone.x86.X86_OP_IMM:
            continue
        target = ops[0].imm
        target_rva = target - image_base
        if target_rva in TARGETS:
            kind = "call" if ins.bytes[0] == 0xE8 else "jmp"
            matches[target_rva].append(
                (ins.address - image_base, ins.bytes.hex(), kind))

    print(f"[+] scan complete\n", file=sys.stderr)

    print("=" * 100)
    print("CALL/JMP sites to wrap with write_call<5> / write_branch<5>")
    print("=" * 100)
    for tgt_rva, label in TARGETS.items():
        print(f"\n  {label}  (entry = 0x{tgt_rva:x}):")
        sites = matches[tgt_rva]
        if not sites:
            print(f"    (none found - either function is only called indirectly)")
            continue
        for site_rva, hexb, kind in sorted(sites):
            sym, fn_rva, delta = nearest_sym(site_rva)
            print(f"    @ 0x{site_rva:08x}  ({sym} +0x{delta:x})  {kind.upper()}  bytes={hexb}")

    print()
    print("Plugin-side constants (paste into Hooks.cpp):")
    for tgt_rva, label in TARGETS.items():
        var = "kCallSites_" + label.split()[-1]
        sites = sorted(matches[tgt_rva])
        print(f"\n  // {label}: {len(sites)} site(s)")
        print(f"  constexpr CallSite {var}[] = {{")
        for site_rva, _, kind in sites:
            sym, fn_rva, delta = nearest_sym(site_rva)
            id_num = int(sym[2:]) if sym.startswith("id") else 0
            kind_macro = "kCall" if kind == "call" else "kJmp "
            print(f"    {{ {id_num}, 0x{delta:x}, {kind_macro} }},  // 0x{site_rva:x}")
        print(f"  }};")

    return 0


if __name__ == "__main__":
    sys.exit(main())
