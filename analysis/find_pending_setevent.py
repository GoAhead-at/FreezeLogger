#!/usr/bin/env python3
"""
Find:
 (a) every instruction `mov dword ptr [rXX + 0x6c], 1` (sets pending=1
     on a singleton-shape struct).
 (b) every `call qword ptr [rip + disp]` whose target is the IAT slot
     for KERNEL32!SetEvent (signals the event).

For each, print the nearest Address Library function id and the source
RVA so we can identify the producer side of the deadlock.
"""

import bisect
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

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


def find_iat_slot(data, image_base, sections, dll, func):
    """Return the RVA of the IAT slot for KERNEL32!SetEvent (or whatever)."""
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    coff_off = pe_off + 4
    opt_off = coff_off + 20
    # PE32+ DataDirectory starts at opt_off + 112; each entry is 8 bytes
    # (RVA, size). Index 1 = Import Directory.
    imp_rva  = struct.unpack_from("<I", data, opt_off + 112 + 1*8 + 0)[0]
    imp_size = struct.unpack_from("<I", data, opt_off + 112 + 1*8 + 4)[0]
    if imp_rva == 0:
        return None

    def rva_to_off(rva):
        for (_n, va, vs, ro, rs) in sections:
            if va <= rva < va + max(vs, rs):
                return ro + (rva - va)
        return None

    def cstr(off):
        end = off
        while data[end] != 0:
            end += 1
        return data[off:end].decode("ascii", errors="replace")

    # Each IMAGE_IMPORT_DESCRIPTOR is 20 bytes; null one terminates.
    p = rva_to_off(imp_rva)
    while True:
        ilt   = struct.unpack_from("<I", data, p +  0)[0]
        nrva  = struct.unpack_from("<I", data, p + 12)[0]
        iat   = struct.unpack_from("<I", data, p + 16)[0]
        if nrva == 0 and iat == 0:
            break
        name = cstr(rva_to_off(nrva)).lower()
        p += 20
        if name != dll.lower():
            continue
        # Walk the ILT (Import Lookup Table) and IAT in parallel.
        thunk_rva = ilt or iat
        thunk_off = rva_to_off(thunk_rva)
        cur_iat   = iat
        while True:
            entry = struct.unpack_from("<Q", data, thunk_off)[0]
            if entry == 0:
                break
            if entry & (1 << 63):
                # Ordinal import; skip
                pass
            else:
                hint_off = rva_to_off(entry & 0x7FFFFFFFFFFFFFFF)
                fname = cstr(hint_off + 2)
                if fname == func:
                    return cur_iat
            thunk_off += 8
            cur_iat   += 8
    return None


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(f"[+] image base 0x{image_base:x}", file=sys.stderr)

    setevent_iat   = find_iat_slot(data, image_base, sections, "KERNEL32.dll", "SetEvent")
    create_event_a = find_iat_slot(data, image_base, sections, "KERNEL32.dll", "CreateEventA")
    create_event_w = find_iat_slot(data, image_base, sections, "KERNEL32.dll", "CreateEventW")
    wait_iat       = find_iat_slot(data, image_base, sections, "KERNEL32.dll", "WaitForSingleObjectEx")

    print(f"[+] IAT slots: SetEvent=0x{setevent_iat or 0:x}  "
          f"CreateEventA=0x{create_event_a or 0:x}  "
          f"CreateEventW=0x{create_event_w or 0:x}  "
          f"WaitForSingleObjectEx=0x{wait_iat or 0:x}", file=sys.stderr)
    iat_targets = {
        setevent_iat:   "KERNEL32!SetEvent",
        create_event_a: "KERNEL32!CreateEventA",
        create_event_w: "KERNEL32!CreateEventW",
        wait_iat:       "KERNEL32!WaitForSingleObjectEx",
    }
    iat_targets = {k: v for k, v in iat_targets.items() if k}

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
    md.detail   = True
    md.skipdata = True

    pending_writes = []   # mov dword [rXX+0x6c], 1
    iat_calls      = {iat: [] for iat in iat_targets}   # call qword [rip+...]

    for ins in md.disasm(text_bytes, image_base + text_va):
        try:
            ops = ins.operands
        except capstone.CsError:
            continue

        if ins.mnemonic == "mov" and len(ops) == 2:
            dst, src = ops
            if (dst.type == capstone.x86.X86_OP_MEM and
                src.type == capstone.x86.X86_OP_IMM and
                src.imm == 1 and
                dst.mem.disp == 0x6c and
                dst.mem.base != capstone.x86.X86_REG_RIP and
                dst.size == 4):
                rva = ins.address - image_base
                pending_writes.append((rva, ins.op_str))

        if ins.mnemonic == "call" and len(ops) == 1:
            (op,) = ops
            if (op.type == capstone.x86.X86_OP_MEM and
                op.mem.base == capstone.x86.X86_REG_RIP and
                op.mem.index == capstone.x86.X86_REG_INVALID):
                target = ins.address + ins.size + op.mem.disp
                target_rva = target - image_base
                if target_rva in iat_targets:
                    rva = ins.address - image_base
                    iat_calls[target_rva].append(rva)

    print()
    print("=" * 100)
    print("Writers of pending=1  (mov dword ptr [reg+0x6c], 1)")
    print("=" * 100)
    print(f"  total: {len(pending_writes)}")
    for rva, op in pending_writes:
        sym, fn_rva, delta = nearest_sym(rva)
        print(f"  0x{rva:<10x}  {sym:<14s} +{delta:<5d}  mov {op}")

    for iat, label in iat_targets.items():
        calls = iat_calls.get(iat, [])
        print()
        print("=" * 100)
        print(f"Callers of {label}  (IAT slot SkyrimSE+0x{iat:x})")
        print("=" * 100)
        print(f"  total: {len(calls)}")
        for rva in calls:
            sym, fn_rva, delta = nearest_sym(rva)
            print(f"  0x{rva:<10x}  {sym:<14s} +{delta:<5d}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
