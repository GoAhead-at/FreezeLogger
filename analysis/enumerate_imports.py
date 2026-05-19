#!/usr/bin/env python3
"""Enumerate every imported kernel32 function and print its IAT-slot RVA."""

import struct
import sys
from pathlib import Path

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
TARGET_DLLS = {"kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll"}
TARGET_RVA  = 0x1509288   # the IAT slot we want to identify


def main() -> int:
    data = EXE.read_bytes()
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
        vsize = struct.unpack_from("<I", data, s + 8)[0]
        vaddr = struct.unpack_from("<I", data, s + 12)[0]
        rsize = struct.unpack_from("<I", data, s + 16)[0]
        roff  = struct.unpack_from("<I", data, s + 20)[0]
        sections.append((name, vaddr, vsize, roff, rsize))

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

    imp_rva  = struct.unpack_from("<I", data, opt_off + 112 + 1*8 + 0)[0]

    p = rva_to_off(imp_rva)
    target_match = None
    while True:
        ilt   = struct.unpack_from("<I", data, p +  0)[0]
        nrva  = struct.unpack_from("<I", data, p + 12)[0]
        iat   = struct.unpack_from("<I", data, p + 16)[0]
        if nrva == 0 and iat == 0:
            break
        dll_name = cstr(rva_to_off(nrva))
        p += 20
        if dll_name.lower() not in TARGET_DLLS:
            continue
        thunk_rva = ilt or iat
        thunk_off = rva_to_off(thunk_rva)
        cur_iat   = iat
        while True:
            entry = struct.unpack_from("<Q", data, thunk_off)[0]
            if entry == 0:
                break
            if not (entry & (1 << 63)):
                hint_off = rva_to_off(entry & 0x7FFFFFFFFFFFFFFF)
                fname = cstr(hint_off + 2)
                marker = " <==" if cur_iat == TARGET_RVA else ""
                print(f"  0x{cur_iat:08x}  {dll_name:<14s} {fname}{marker}")
                if cur_iat == TARGET_RVA:
                    target_match = (dll_name, fname)
            thunk_off += 8
            cur_iat   += 8

    print()
    if target_match:
        print(f"[FOUND] IAT slot 0x{TARGET_RVA:x} -> {target_match[0]}!{target_match[1]}")
    else:
        print(f"[?] IAT slot 0x{TARGET_RVA:x} not found among target DLLs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
