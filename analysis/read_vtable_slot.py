#!/usr/bin/env python3
"""
Phase 4 prep: given a VTABLE_<NAME> entry from Offsets_VTABLE.h, read
the qword at one or more slots and resolve to (RVA, nearest addrlib id).

The reverse of fingerprint_form_types.py: there we asked "which vtables
have function X in any slot"; here we ask "what function is at slot Y
of vtable Z".

Usage:
  python read_vtable_slot.py <VTABLE_NAME> <slot_off1> [<slot_off2> ...]
  python read_vtable_slot.py --multi <slot> <NAME1> [<NAME2> ...]

Examples:
  python read_vtable_slot.py Actor 0x4c8 0x558 0x790
  python read_vtable_slot.py BGSOutfit 0x790
  python read_vtable_slot.py --multi 0x790 Actor TESObjectREFR Character PlayerCharacter
"""

from __future__ import annotations

import argparse
import bisect
import re
import struct
import sys
from pathlib import Path

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")
OFFSETS_VTABLE = Path(
    r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\armor_crash\build-vs2026"
    r"\vcpkg_installed\vcpkg\blds\commonlibsse-ng\src\e241773cc6-89709b6876.clean"
    r"\include\RE\Offsets_VTABLE.h"
)

VTABLE_RE = re.compile(
    r"VTABLE_(?P<name>[A-Za-z0-9_]+)\s*\{[^}]*?REL::VariantID\s*\(\s*"
    r"(?P<se>\d+)\s*,\s*(?P<ae>\d+)\s*,\s*(?P<vr>0x[0-9a-fA-F]+|\d+)\s*\)"
)

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
    ap = argparse.ArgumentParser()
    ap.add_argument("--multi", action="store_true",
                    help="Multi-vtable mode: scan one slot across many vtables")
    ap.add_argument("args", nargs="+",
                    help="Positional: <vtable_name> <slot1> [<slot2> ...] OR (with --multi) <slot> <name1> [<name2> ...]")
    a = ap.parse_args()

    data, image_base, sections = parse_pe(EXE)
    _, _, entries = parse(ADDRLIB)
    se_id_to_rva = {eid: off for eid, off in entries}
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    def nearest_sym(rva: int):
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            return ("?", 0, 0)
        return (f"id{ids_for_off[i]}", offsets[i], rva - offsets[i])

    text_h = OFFSETS_VTABLE.read_text(encoding="utf-8", errors="replace")
    name_to_rva: dict[str, int] = {}
    for m in VTABLE_RE.finditer(text_h):
        rva = se_id_to_rva.get(int(m.group("se")))
        if rva is not None:
            name_to_rva[m.group("name")] = rva

    def section_for_rva(rva: int):
        for (n, va, vs, ro, rs) in sections:
            if rs > 0 and va <= rva < va + vs:
                return ro + (rva - va), ro + min(vs, rs)
        return None

    def read_slot(vtable_name: str, slot_off: int) -> tuple[int, int, str, str]:
        vt_rva = name_to_rva.get(vtable_name)
        if vt_rva is None:
            return (0, 0, "", "VTABLE_" + vtable_name + " NOT FOUND in Offsets_VTABLE.h")
        sec = section_for_rva(vt_rva + slot_off)
        if sec is None:
            return (vt_rva, 0, "", "slot file-offset out of range")
        sec_start, _ = sec
        qw = struct.unpack_from("<Q", data, sec_start)[0]
        if qw == 0:
            return (vt_rva, 0, "", "slot is null")
        target_rva = qw - image_base
        sym, _, off = nearest_sym(target_rva)
        return (vt_rva, target_rva, sym, "" if off == 0 else f"+0x{off:x}")

    if a.multi:
        if len(a.args) < 2:
            print("usage: --multi <slot> <name1> [<name2> ...]", file=sys.stderr)
            return 2
        slot = int(a.args[0], 0)
        names = a.args[1:]
        print(f"slot +0x{slot:x}  across {len(names)} vtables:\n")
        for name in names:
            vt_rva, tgt, sym, extra = read_slot(name, slot)
            if extra and not tgt:
                print(f"  VTABLE_{name:<40s}  {extra}")
            else:
                print(f"  VTABLE_{name:<40s}  vt RVA 0x{vt_rva:x}  ->  0x{tgt:x}  ({sym}{extra})")
    else:
        if len(a.args) < 2:
            print("usage: <vtable_name> <slot1> [<slot2> ...]", file=sys.stderr)
            return 2
        name = a.args[0]
        slots = [int(s, 0) for s in a.args[1:]]
        vt_rva = name_to_rva.get(name)
        if vt_rva is None:
            print(f"VTABLE_{name} not found in Offsets_VTABLE.h")
            return 2
        print(f"VTABLE_{name}  at RVA 0x{vt_rva:x}\n")
        for slot in slots:
            _, tgt, sym, extra = read_slot(name, slot)
            if not tgt:
                print(f"  +0x{slot:<3x}  {extra}")
            else:
                print(f"  +0x{slot:<4x}  -> 0x{tgt:x}  ({sym}{extra})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
