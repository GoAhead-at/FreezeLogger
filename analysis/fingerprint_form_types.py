#!/usr/bin/env python3
"""
Phase 3 task 2: enumerate which named CommonLibSSE-NG vtables have
id 39816 / id 16760 / id 41785 in their slot +0x8 or slot +0x790.

These are exactly the vtable slots that close the depth-2 LockB ->
LockA back-edge:
  id40285  --[vt+0x8]----> id39816  --[direct]--> id19369
  id40285  --[vt+0x790]--> id16760  --[direct]--> id19369
  id40285  --[vt+0x8]----> id41785  --[direct]--> id19369

A form whose vtable[1] points at id 39816 / id 41785, OR whose
vtable[+0x790] points at id 16760, can drive a deadlock the moment
id 40285 (or any of the four ProcessLists methods) iterates over it
under LockB.

We scan every vtable in `Offsets_VTABLE.h` (parsed for VTABLE_NAME -
SE id pairs), resolve each SE id to an RVA via the address library,
read the qword at slot +0x8 and +0x790 of each vtable, and report
matches.

Bonus: also scan slots +0x4f0 and +0x88, since the LockA -> LockB
direction at depth 3 goes
  id19369 --[vt+0x2f0]--> id41777 --[vt+0x4f0]--> id36295  --[direct]--> id40333
  id19369 --[vt+0x2f0]--> id27458 --[vt+0x88]--> id36644   --[direct]--> id40333
"""

from __future__ import annotations

import bisect
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")
OFFSETS_VTABLE = Path(
    r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\armor_crash\build-vs2026"
    r"\vcpkg_installed\vcpkg\blds\commonlibsse-ng\src\e241773cc6-89709b6876.clean"
    r"\include\RE\Offsets_VTABLE.h"
)

# (function-of-interest RVA, label, list of slots to check)
INTERESTS = [
    (0x6c02f0, "id39816 (LockB->LockA hop)",        [0x8, 0x790]),
    (0x211c80, "id16760 (LockB->LockA hop)",        [0x8, 0x790]),
    (0x7224e0, "id41785 (LockB->LockA hop)",        [0x8, 0x790]),
    (0x721ff0, "id41777 (LockA->LockB hop, level 1)", [0x4f0, 0x88]),
    (0x3fa320, "id27458 (LockA->LockB hop, level 1)", [0x4f0, 0x88]),
    (0x781d50, "id43950 (LockA->LockB hop, level 1)", [0xb0]),
    (0x956f30, "id54046 (LockA->LockB hop, level 1)", [0x158]),
    (0x5d2b80, "id36295 (LockA->LockB hop, level 2)", []),
    (0x5f4ea0, "id36644 (LockA->LockB hop, level 2)", []),
    (0x5ff4b0, "id36775 (LockA->LockB hop, level 2)", []),
    (0x5ec020, "id36563 (LockA->LockB hop, level 2)", []),
    (0x992690, "id55605 (LockA->LockB hop, level 1)", [0x8]),
    (0x990e50, "id55581 (LockA->LockB hop, level 1)", [0x8]),
    (0x991480, "id55587 (LockA->LockB hop, level 1)", [0x8]),
]

# Generic scan: we want to know, for every interesting target RVA, which
# vtable slot (any slot) of which class points at it.
INTEREST_RVAS = {tgt for (tgt, _l, _s) in INTERESTS}
INTEREST_LABELS = {tgt: lbl for (tgt, lbl, _s) in INTERESTS}

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
    data, image_base, sections = parse_pe(EXE)
    print(f"[+] image base 0x{image_base:x}", file=sys.stderr)

    # Find .text bounds for sanity-checking function pointers
    text_va = text_vs = 0
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            text_va, text_vs = va, vs
            break

    _, _, entries = parse(ADDRLIB)
    se_id_to_rva = {eid: off for eid, off in entries}

    # Parse Offsets_VTABLE.h for VTABLE_NAME entries with their primary SE id
    vtables: list[tuple[int, str]] = []  # (vtable_rva, vtable_name)
    text_h = OFFSETS_VTABLE.read_text(encoding="utf-8", errors="replace")
    for m in VTABLE_RE.finditer(text_h):
        se_id = int(m.group("se"))
        rva = se_id_to_rva.get(se_id)
        if rva is not None:
            vtables.append((rva, m.group("name")))
    vtables.sort()
    print(f"[+] resolved {len(vtables)} vtables from Offsets_VTABLE.h", file=sys.stderr)

    # Build a flat map: file_offset_for_rva
    def section_for_rva(rva: int):
        for (n, va, vs, ro, rs) in sections:
            if rs > 0 and va <= rva < va + vs:
                return ro + (rva - va), ro + min(vs, rs)
        return None

    # For each vtable, walk its first 0x100 slots (2048 bytes).
    # For each slot, if the qword resolves to an RVA we care about, record it.
    INTEREST_QWORDS = {image_base + r: r for r in INTEREST_RVAS}
    matches: dict[int, list[tuple[str, int, int]]] = defaultdict(list)
    # also: scan ALL vtable slots in [0, 0x800) for any ENTRY pointing at any interest target,
    # so we don't miss e.g. a non-canonical slot.
    SLOT_LIMIT = 0x100  # 256 slots = 2 KB; deepest known slot we use is +0x790 = slot 242

    for (vt_rva, vname) in vtables:
        sec = section_for_rva(vt_rva)
        if sec is None:
            continue
        sec_start, sec_end = sec
        for slot_idx in range(SLOT_LIMIT):
            file_off = sec_start + slot_idx * 8
            if file_off + 8 > sec_end:
                break
            qw = struct.unpack_from("<Q", data, file_off)[0]
            if qw in INTEREST_QWORDS:
                tgt_rva = INTEREST_QWORDS[qw]
                slot_disp = slot_idx * 8
                matches[tgt_rva].append((vname, slot_disp, vt_rva))

    # Print results
    print()
    print("=" * 100)
    print("FORM-SUBTYPE / CLASS FINGERPRINT for cycle-intermediate functions")
    print("=" * 100)

    # Print depth-2 LockB->LockA hops first; they are the hot ones.
    PRIORITY_ORDER = [0x6c02f0, 0x211c80, 0x7224e0,                                    # depth-2 LockB->LockA
                       0x992690, 0x990e50, 0x991480,                                   # depth-3 LockA->LockB intermediates (vt+0x8)
                       0x721ff0, 0x3fa320, 0x781d50, 0x956f30,                         # depth-3 LockA->LockB intermediates (varied)
                       0x5d2b80, 0x5f4ea0, 0x5ff4b0, 0x5ec020]                         # depth-3 LockA->LockB sinks
    for tgt in PRIORITY_ORDER:
        hits = matches.get(tgt, [])
        label = INTEREST_LABELS.get(tgt, f"0x{tgt:x}")
        print()
        print("-" * 100)
        print(f"  {label}    RVA 0x{tgt:x}    {len(hits)} vtable hit(s)")
        print("-" * 100)
        if not hits:
            print("    (no named CommonLibSSE-NG vtable references this function)")
            continue
        # group by slot
        by_slot = defaultdict(list)
        for (vname, slot, vt_rva) in hits:
            by_slot[slot].append((vname, vt_rva))
        for slot in sorted(by_slot.keys()):
            entries_at_slot = by_slot[slot]
            print(f"\n    slot +0x{slot:<3x}  ({len(entries_at_slot)} class(es)):")
            for (vname, vt_rva) in sorted(entries_at_slot):
                print(f"      VTABLE_{vname}  (vt at RVA 0x{vt_rva:x})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
