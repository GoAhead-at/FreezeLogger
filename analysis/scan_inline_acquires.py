#!/usr/bin/env python3
"""Scan SkyrimSE.exe for ANY direct memory access to LockA/LockB
fields (tid at +0, state at +4). The intent is to verify whether
every acquire/release of these locks goes through id 12210
(BSSpinLock::Acquire) plus inline release patterns we already
know about.

We classify hits by what kind of memory operation touches the lock:
  - LOCK CMPXCHG  -> atomic compare-exchange (acquire OR release)
  - LOCK XADD     -> atomic add (rare; refcount-style)
  - LOCK INC/DEC  -> atomic recursion bump/dec
  - MOV (write)   -> non-atomic write (release tid clear)
  - CMP/MOV (read)-> read for comparison (e.g. ownership check)
"""
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from xref_calls import parse_pe

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
LOCK_A_RVA  = 0x2eff8e0
LOCK_B_RVA  = 0x2f3b8e8
INTERESTING = {
    LOCK_A_RVA:     "LockA.tid",
    LOCK_A_RVA + 4: "LockA.state",
    LOCK_B_RVA:     "LockB.tid",
    LOCK_B_RVA + 4: "LockB.state",
}


def main() -> int:
    data, image_base, sections = parse_pe(EXE)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    text = next(s for s in sections if s[0] == ".text" and s[2] > 0x100000)
    _, va, vsize, raw_off, raw_size = text
    blob = data[raw_off:raw_off + min(vsize, raw_size)]

    counts = {}
    samples = {}

    for ins in md.disasm(blob, image_base + va):
        for op in ins.operands:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            mem = op.value.mem
            if mem.base != capstone.x86.X86_REG_RIP:
                continue
            target = ins.address + ins.size + mem.disp - image_base
            label = INTERESTING.get(target)
            if not label:
                continue

            kind = ins.mnemonic
            key = (label, kind)
            counts[key] = counts.get(key, 0) + 1
            samples.setdefault(key, [])
            if len(samples[key]) < 5:
                samples[key].append(
                    (ins.address - image_base, f"{ins.mnemonic} {ins.op_str}"))

    if not counts:
        print("No RIP-relative access to LockA/LockB found.")
        return 0

    print(f"{'lock/field':16s} {'mnemonic':12s} {'count':>6s}")
    for (label, kind), c in sorted(counts.items()):
        print(f"  {label:16s} {kind:12s} {c:>6d}")
        for addr, txt in samples[(label, kind)]:
            print(f"      @ 0x{addr:x}  {txt}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
