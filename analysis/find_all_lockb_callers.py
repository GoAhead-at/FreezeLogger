#!/usr/bin/env python3
"""For every direct CALL to id 12210 (BSSpinLock::Acquire) in the binary,
back-track and identify the lock pointer in rcx.

We accept three sources:
  1. `lea rcx, [rip + disp32]`              (rcx = absolute RVA)
  2. `mov rcx, [rip + disp32]`              (rcx = *(global pointer))
  3. `mov rcx, [reg + small_offset]`        (rcx = struct field; not resolvable
                                             statically)

For (2), we read the .data section and check the actual stored pointer.

We also enumerate every direct CALL to the four LockB helpers (id 40285,
id 40333, id 40334, id 40335) to find sites we have not hooked yet.
"""
import bisect
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB
from xref_calls import parse_pe

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")

LOCK_A_RVA   = 0x2eff8e0
LOCK_B_RVA   = 0x2f3b8e8
ACQUIRE_RVA  = 0x132bd0      # id 12210 = BSSpinLock::Acquire
HELPERS = {
    0x6d37b0: 40285,
    0x6d9720: 40333,
    0x6d9890: 40334,
    0x6d99d0: 40335,
    0x6ef230: 40706,
}


def main() -> int:
    name, ptr_size, entries = parse(ADDRLIB)
    sorted_entries = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in sorted_entries]
    ids_for_off = [e[0] for e in sorted_entries]

    def nearest(rva):
        idx = bisect.bisect_right(offsets, rva) - 1
        if idx < 0:
            return None, 0
        return ids_for_off[idx], rva - offsets[idx]

    data, image_base, sections = parse_pe(EXE)

    def read_qword(rva):
        for (n, va, vs, ro, rs) in sections:
            if va <= rva < va + vs:
                file_off = ro + (rva - va)
                if file_off + 8 <= len(data):
                    return int.from_bytes(data[file_off:file_off + 8], "little")
        return None

    text = next(s for s in sections if s[0] == ".text")
    name_, va, vs, ro, rs = text
    blob = data[ro:ro + rs]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    # Collect direct CALL/JMP sites for BSSpinLock::Acquire and helpers
    targets = {ACQUIRE_RVA: "BSSpinLock::Acquire (id 12210)"}
    for tgt, fid in HELPERS.items():
        targets[tgt] = f"id {fid} helper"

    call_sites = {tgt: [] for tgt in targets}
    i = 0
    n = len(blob)
    while i < n - 5:
        op = blob[i]
        if op == 0xE8 or op == 0xE9:
            disp = int.from_bytes(blob[i+1:i+5], "little", signed=True)
            target = (va + i + 5 + disp) & 0xFFFFFFFF
            if target in call_sites:
                call_sites[target].append((va + i, op))
            i += 5
        else:
            i += 1

    print("[*] direct call/jmp counts:")
    for tgt, lbl in targets.items():
        print(f"    {lbl:<40s}  {len(call_sites[tgt])}")

    # For each Acquire call, look back 64 bytes for rcx setup
    md_back = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md_back.detail = True

    direct_lockA = {}
    direct_lockB = {}
    indirect_via_global_lockB = {}
    indirect_via_struct = {}

    for site_va, op in call_sites[ACQUIRE_RVA]:
        start = site_va - 64
        s_off = start - va
        if s_off < 0:
            continue
        chunk = blob[s_off:s_off + 64 + 5]
        last_kind = None  # ('lea', target_rva) | ('mov_global', target_rva, stored) | ('mov_struct', None) | None
        for ins in md_back.disasm(chunk, start):
            if ins.address >= site_va:
                break
            if len(ins.operands) >= 2:
                op0 = ins.operands[0]
                op1 = ins.operands[1]
                if (op0.type == capstone.x86.X86_OP_REG and
                    op0.reg == capstone.x86.X86_REG_RCX):
                    if ins.mnemonic == "lea" and op1.type == capstone.x86.X86_OP_MEM:
                        if op1.value.mem.base == capstone.x86.X86_REG_RIP:
                            target = ins.address + ins.size + op1.value.mem.disp
                            last_kind = ("lea", target)
                    elif ins.mnemonic == "mov" and op1.type == capstone.x86.X86_OP_MEM:
                        if op1.value.mem.base == capstone.x86.X86_REG_RIP:
                            target = ins.address + ins.size + op1.value.mem.disp
                            stored = read_qword(target)
                            if stored is not None:
                                stored_rva = stored - image_base
                                last_kind = ("mov_global", target, stored_rva)
                            else:
                                last_kind = ("mov_global", target, None)
                        else:
                            last_kind = ("mov_struct", None)
                    elif ins.mnemonic == "mov" and op1.type == capstone.x86.X86_OP_REG:
                        last_kind = ("mov_reg", None)

        fid, fdelta = nearest(site_va)
        key = fid

        if last_kind is None:
            indirect_via_struct.setdefault(("unknown", fid), []).append(site_va)
            continue

        kind = last_kind[0]
        if kind == "lea":
            tgt = last_kind[1]
            if tgt == LOCK_A_RVA:
                direct_lockA.setdefault(key, []).append(site_va)
            elif tgt == LOCK_B_RVA:
                direct_lockB.setdefault(key, []).append(site_va)
        elif kind == "mov_global":
            global_rva = last_kind[1]
            stored_rva = last_kind[2]
            if stored_rva == LOCK_B_RVA:
                indirect_via_global_lockB.setdefault(key, []).append((site_va, global_rva))
            elif stored_rva == LOCK_A_RVA:
                # very rare but check
                pass
            else:
                # global doesn't statically point to LockB - might be variable
                pass
        else:
            # mov_struct or mov_reg - can't determine statically
            indirect_via_struct.setdefault(("indirect", fid), []).append(site_va)

    print("\n=================================================")
    print("LockA direct acquirers (lea rcx, [LockA]; call Acquire):")
    print("=================================================")
    for fid in sorted(direct_lockA, key=lambda x: x or 0):
        sites = direct_lockA[fid]
        print(f"  id {fid}  ({len(sites)} site(s))")

    print("\n=================================================")
    print("LockB direct acquirers (lea rcx, [LockB]; call Acquire):")
    print("=================================================")
    for fid in sorted(direct_lockB, key=lambda x: x or 0):
        sites = direct_lockB[fid]
        print(f"  id {fid}  ({len(sites)} site(s))")

    print("\n=================================================")
    print("LockB indirect acquirers via global (mov rcx, [rip+disp])")
    print("where the global statically holds LockB's address:")
    print("=================================================")
    for fid in sorted(indirect_via_global_lockB, key=lambda x: x or 0):
        sites = indirect_via_global_lockB[fid]
        for site_va, gva in sites:
            print(f"  id {fid:<6}  global @ 0x{gva:x}  call site 0x{site_va:x}")

    # Print direct CALL sites for each LockB helper (we want to know what we miss)
    print("\n=================================================")
    print("Direct CALL sites for each LockB helper:")
    print("=================================================")
    for tgt in (0x6d37b0, 0x6d9720, 0x6d9890, 0x6d99d0, 0x6ef230):
        helper_id = HELPERS[tgt]
        print(f"\n  id {helper_id} helper sites: {len(call_sites[tgt])}")
        for site_va, op in call_sites[tgt]:
            fid, fdelta = nearest(site_va)
            print(f"    @ 0x{site_va:x}  {'JMP' if op == 0xE9 else 'CALL'}  in id {fid} +0x{fdelta:x}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
