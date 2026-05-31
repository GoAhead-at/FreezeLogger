#!/usr/bin/env python3
"""Locate WaitForJobTask by signature on any unpacked Skyrim build, and derive
the Singleton-B global from its rip-relative load. The SE dispatcher body is:

    mov  rax,[rip+disp]   ; Singleton-B  (48 8B 05 disp32)   <-- prologue, disp varies
    mov  r8d,ecx          ; 44 8B C1
    mov  rcx,[rax+8]      ; 48 8B 48 08
    mov  rcx,[rcx+r8*8]   ; 4A 8B 0C C1
    test rcx,rcx          ; 48 85 C9
    je   short            ; 74 ??
    mov  rcx,[rcx]        ; 48 8B 09
    ...

We anchor on the byte run AFTER the variable prologue load, then walk back to
the `48 8B 05` to read its disp32 and compute Singleton-B.
"""
import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

BUILDS = {
    "SE 1.5.97":   r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\analysis\se\SkyrimSE.exe.unpacked.exe",
    "AE 1.6.1170": r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\analysis\ae\SkyrimSE.exe.unpacked.exe",
}

# Body signature starting at `mov r8d,ecx` (skips the version-variable
# `mov rax,[rip+disp]` prologue). 0x90 = wildcard.
SIG = bytes.fromhex("448BC1488B48084A8B0CC14885C974")  # last 74 = je opcode


def parse_pe(path):
    data = open(path, "rb").read()
    po = struct.unpack_from("<I", data, 0x3C)[0]
    coff = po + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt = coff + 20
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    ib = struct.unpack_from("<Q", data, opt + 24)[0]
    secs = []
    s = opt + opt_size
    for _ in range(nsec):
        name = data[s:s + 8].rstrip(b"\x00").decode("latin1")
        vsize = struct.unpack_from("<I", data, s + 8)[0]
        vaddr = struct.unpack_from("<I", data, s + 12)[0]
        rsize = struct.unpack_from("<I", data, s + 16)[0]
        roff = struct.unpack_from("<I", data, s + 20)[0]
        secs.append((name, vaddr, vsize, roff, rsize))
        s += 40
    return data, ib, secs


def text_sections(secs):
    return [(va, vs, ro, rs) for (n, va, vs, ro, rs) in secs if n == ".text"]


def main():
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for label, path in BUILDS.items():
        data, ib, secs = parse_pe(path)
        print(f"\n===== {label}  (imageBase 0x{ib:x}) =====")
        hits = []
        for (va, vs, ro, rs) in text_sections(secs):
            blob = data[ro:ro + rs]
            start = 0
            while True:
                i = blob.find(SIG, start)
                if i < 0:
                    break
                hits.append((va + i, ro + i))
                start = i + 1
        print(f"  body-signature hits: {len(hits)}")
        for body_rva, body_off in hits[:8]:
            # Walk back up to 16 bytes to find `48 8B 05` (mov rax,[rip+d32]).
            window = data[body_off - 16:body_off]
            j = window.rfind(bytes.fromhex("488B05"))
            if j < 0:
                print(f"  body@0x{body_rva:x}: no preceding 48 8B 05 prologue found")
                continue
            mov_rva = body_rva - 16 + j
            disp = struct.unpack_from("<i", data, (body_off - 16 + j) + 3)[0]
            singleton = mov_rva + 7 + disp
            fn_rva = mov_rva  # function entry = the singleton load
            print(f"  WaitForJobTask @ 0x{fn_rva:x}   Singleton-B global @ 0x{singleton:x}")


if __name__ == "__main__":
    sys.exit(main())
