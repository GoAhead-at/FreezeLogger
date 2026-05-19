#!/usr/bin/env python3
"""Find BSSpinLock::Release.

The shape we're looking for is the inverse of 12210:
  - reads [rcx]  to compare to current tid (or simply trusts caller)
  - lock dec [rcx + 4]
  - if recursion count reached 0, clear [rcx]
  - small (< 0x60 bytes), no spin loop.

We scan the address library: any function whose first 64 bytes
contain a `lock dec dword ptr [rcx + 4]` AND a `mov dword ptr [rcx], 0`
or equivalent is a candidate.
"""
import bisect
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from xref_calls import parse_pe
from addrlib_lookup import parse

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    text = next(s for s in sections if s[0] == ".text" and s[2] > 0x100000)
    _, va, vsize, raw_off, raw_size = text
    text_base_rva = va
    text_blob = data[raw_off:raw_off + min(vsize, raw_size)]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    _, _, entries = parse(ADDRLIB)
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]

    candidates = []
    for ix, off in enumerate(offsets):
        if off < text_base_rva or off >= text_base_rva + len(text_blob):
            continue
        next_off = offsets[ix + 1] if ix + 1 < len(offsets) else off + 0x80
        size = min(next_off - off, 0x80)
        if size < 0x10 or size > 0x80:
            continue
        slice_off = off - text_base_rva
        chunk = text_blob[slice_off:slice_off + size]

        has_lock_dec = False
        has_clear_tid = False
        has_lock_cmpxchg = False
        for ins in md.disasm(chunk, image_base + off):
            mn = ins.mnemonic
            ops = ins.op_str
            if mn == "dec" and "[rcx + 4]" in ops:
                has_lock_dec = True
            elif mn == "lock dec" and "[rcx + 4]" in ops:
                has_lock_dec = True
            elif "dword ptr [rcx + 4]" in ops and ("dec" in mn):
                has_lock_dec = True
            if mn == "mov" and ops.startswith("dword ptr [rcx]") and ops.endswith(", 0"):
                has_clear_tid = True
            if "cmpxchg" in mn:
                has_lock_cmpxchg = True
        if has_lock_dec or has_clear_tid:
            candidates.append((off, ids_for_off[ix], size,
                               has_lock_dec, has_clear_tid, has_lock_cmpxchg))

    print(f"{'rva':>10s}  {'id':>6s}  {'size':>5s}  dec  clr  cmpx")
    for off, idn, size, ld, ct, cx in candidates:
        print(f"  0x{off:08x}  id{idn:>5d}  0x{size:03x}  "
              f"{'Y' if ld else '-':>3s}  {'Y' if ct else '-':>3s}  "
              f"{'Y' if cx else '-':>4s}")
    print(f"\n{len(candidates)} candidate(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
