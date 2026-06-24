#!/usr/bin/env python3
"""Find direct `call rel32` (E8) sites targeting a given function RVA, in the
unpacked SE or AE binary, and reverse-resolve each call site + return address
to the nearest Address Library id. Used to pin the Main::Update return
addresses just after the call to WaitForJobTask (Site-B) and the Site-A lock
primitive (Site-A).

Usage:
  python find_callers.py se 0xc38130     # WaitForJobTask callers on SE
  python find_callers.py se 0x5765d0     # Site-A callers on SE
  python find_callers.py ae 0xcfd410     # WaitForJobTask callers on AE
  python find_callers.py ae 0x5fc210     # Site-A callers on AE
"""
import bisect
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse
from xref_calls import parse_pe, find_text

SE_EXE = Path(__file__).parent / "se" / "SkyrimSE.exe.unpacked.exe"
AE_EXE = Path(__file__).parent / "ae" / "SkyrimSE.exe.unpacked.exe"
SE_BIN = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")
AE_BIN = Path(r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\AddressLibrary\All in one (1.6.X)\SKSE\Plugins\versionlib-1-6-1170-0.bin")


def main():
    which = sys.argv[1]
    target = int(sys.argv[2], 0)
    EXE, BIN = (SE_EXE, SE_BIN) if which == "se" else (AE_EXE, AE_BIN)
    name, ptr, entries = parse(BIN)
    by_off = sorted(entries, key=lambda e: e[1])
    offs = [e[1] for e in by_off]
    ids = [e[0] for e in by_off]

    def nearest(rva):
        i = bisect.bisect_right(offs, rva) - 1
        if i < 0:
            return None, None
        return ids[i], rva - offs[i]

    data, image_base, sections = parse_pe(EXE)
    tva, tvs, tro, trs = find_text(sections)
    text = data[tro:tro + min(tvs, trs)]

    print(f"[{which}] callers of 0x{target:x}:")
    i = 0
    n = len(text)
    while i < n - 5:
        if text[i] == 0xE8:
            rel = int.from_bytes(text[i + 1:i + 5], "little", signed=True)
            site = tva + i
            tgt = site + 5 + rel
            if tgt == target:
                ret = site + 5
                cid, cd = nearest(site)
                rid, rd = nearest(ret)
                print(f"  call @0x{site:x} (id {cid}+0x{cd:x})  "
                      f"return=0x{ret:x} (id {rid}+0x{rd:x})")
            i += 5
        else:
            i += 1


if __name__ == "__main__":
    main()
