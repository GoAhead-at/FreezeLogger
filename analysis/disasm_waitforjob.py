#!/usr/bin/env python3
"""Disassemble WaitForJobTask on SE and AE, surface rip-relative global
loads, and compute their targets. Goal: confirm that Singleton-B is reached
by a `mov rax,[rip+disp]` early in the function, and derive the AE Singleton-B
RVA (AE addrlib omits the data-global ID 516902).
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, x86

TARGETS = {
    "SE 1.5.97": (
        r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\analysis\se\SkyrimSE.exe.unpacked.exe",
        0xc38130, 0x2f26a70,
    ),
    "AE 1.6.1170": (
        r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\analysis\ae\SkyrimSE.exe.unpacked.exe",
        0xcc6b20, None,
    ),
}


def parse_pe(path):
    data = open(path, "rb").read()
    po = struct.unpack_from("<I", data, 0x3C)[0]
    coff = po + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt = coff + 20
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
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
    return data, image_base, secs


def rva_to_off(secs, rva):
    for name, va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def main():
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for label, (path, fn_rva, expect) in TARGETS.items():
        data, ib, secs = parse_pe(path)
        off = rva_to_off(secs, fn_rva)
        print(f"\n===== {label}  WaitForJobTask @ 0x{fn_rva:x} (imageBase 0x{ib:x}) =====")
        if off is None:
            print("  RVA not in any section!")
            continue
        code = data[off:off + 120]
        first_global = None
        for ins in md.disasm(code, fn_rva):
            ripline = ""
            for op in ins.operands:
                if op.type == CS_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    ripline = f"   ; rip-> 0x{tgt:x}"
                    if first_global is None:
                        first_global = (ins.mnemonic, tgt, ins.address)
            print(f"  0x{ins.address:08x}: {ins.mnemonic:<6s} {ins.op_str}{ripline}")
            if ins.address - fn_rva > 48 and first_global:
                break
        if first_global:
            mn, tgt, at = first_global
            print(f"  --> first rip-global: {mn} @0x{at:x} -> 0x{tgt:x}")
            if expect is not None:
                print(f"      expected Singleton-B 0x{expect:x}  ->  {'MATCH' if tgt == expect else 'DIFFERENT'}")


if __name__ == "__main__":
    sys.exit(main())
