#!/usr/bin/env python3
"""
Phase 1.5: search the binary for any 8-byte qword equal to
`image_base + target_rva` for each function we care about.

The hits are vtable slots that point at the target function. By
cross-referencing each hit RVA against CommonLibSSE-NG's
`Offsets_VTABLE.h`, we can identify which class's vtable contains
the function, and at which slot index. That gives us the function's
class identity and (via the slot index) often its name.
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

TARGETS = {
    0x296c00: "id 19369 (sole LockA acquirer)",
    0x6d37b0: "id 40285 (LockB acquirer #1)",
    0x6d9720: "id 40333 (LockB acquirer #2)",
    0x6d9890: "id 40334 (LockB acquirer #3)",
    0x6d99d0: "id 40335 (LockB acquirer #4)",
    0x6ef230: "id 40706 (candidate LockB acquirer)",
}

VTABLE_RE = re.compile(
    r"VTABLE_(?P<name>[A-Za-z0-9_]+)\s*\{\s*REL::VariantID\s*\(\s*"
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


def parse_vtable_offsets() -> dict[int, str]:
    """Return SE_id -> 'VTABLE_NAME' mapping."""
    text = OFFSETS_VTABLE.read_text(encoding="utf-8", errors="replace")
    out: dict[int, str] = {}
    for m in VTABLE_RE.finditer(text):
        se_id = int(m.group("se"))
        out[se_id] = m.group("name")
    return out


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    print(f"[+] image base 0x{image_base:x}", file=sys.stderr)

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

    vtable_name_by_seid = parse_vtable_offsets()
    # Build VTABLE_RVA -> name lookup. The vtable RVA is the SE id resolved.
    vtable_rva_to_name: dict[int, str] = {}
    for se_id, name in vtable_name_by_seid.items():
        if se_id in se_id_to_rva:
            vtable_rva_to_name[se_id_to_rva[se_id]] = name
    print(f"[+] parsed {len(vtable_name_by_seid)} VTABLE_* entries from Offsets_VTABLE.h", file=sys.stderr)
    print(f"[+] resolved {len(vtable_rva_to_name)} of them via Address Library", file=sys.stderr)

    sorted_vt_rvas = sorted(vtable_rva_to_name.keys())

    def find_containing_vtable(rva: int) -> tuple[str | None, int]:
        """Return (vtable name, slot offset bytes) if `rva` is inside any known vtable
        (within first 0x800 bytes)."""
        i = bisect.bisect_right(sorted_vt_rvas, rva) - 1
        if i < 0:
            return (None, 0)
        vt_rva = sorted_vt_rvas[i]
        if rva - vt_rva < 0x800:
            return (vtable_rva_to_name[vt_rva], rva - vt_rva)
        return (None, 0)

    target_qwords = {image_base + rva: rva for rva in TARGETS}

    # Scan every section's bytes for our target qwords.
    matches: dict[int, list[tuple[int, str]]] = defaultdict(list)  # target_rva -> [(hit_rva, section_name)]
    for (n, va, vs, ro, rs) in sections:
        if rs == 0:
            continue
        sec_bytes = data[ro:ro + min(vs, rs)]
        for off in range(0, len(sec_bytes) - 8, 8):  # qword-aligned scan
            qw = struct.unpack_from("<Q", sec_bytes, off)[0]
            if qw in target_qwords:
                matches[target_qwords[qw]].append((va + off, n))

    for target_rva, note in TARGETS.items():
        ms = matches.get(target_rva, [])
        print()
        print("=" * 100)
        print(f"VTABLE-SLOT MATCHES for RVA 0x{target_rva:x}   ({note})   -- {len(ms)} hit(s)")
        print("=" * 100)
        for hit_rva, section in sorted(ms):
            vt_name, slot_offset = find_containing_vtable(hit_rva)
            sym, sym_rva, delta = nearest_sym(hit_rva)
            class_tag = (
                f"  in {vt_name}+0x{slot_offset:x} (slot #{slot_offset // 8})"
                if vt_name
                else "  (no known vtable claims this region)"
            )
            print(f"  hit 0x{hit_rva:<10x}  section={section:<8s}  near {sym}+0x{delta:x}{class_tag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
