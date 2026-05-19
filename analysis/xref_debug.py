"""Debug: confirm capstone decodes the known RIP-relative load at +0x5765d6."""
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")

data = EXE.read_bytes()
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
coff_off = pe_off + 4
opt_size = struct.unpack_from("<H", data, coff_off + 16)[0]
opt_off = coff_off + 20
image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]
num_sections = struct.unpack_from("<H", data, coff_off + 2)[0]
sections_off = opt_off + opt_size
secs = []
for i in range(num_sections):
    s = sections_off + i * 40
    name = data[s:s+8].rstrip(b"\0").decode("ascii", errors="replace")
    va = struct.unpack_from("<I", data, s+12)[0]
    vs = struct.unpack_from("<I", data, s+8)[0]
    ro = struct.unpack_from("<I", data, s+20)[0]
    rs = struct.unpack_from("<I", data, s+16)[0]
    secs.append((name, va, vs, ro, rs))

text = next(s for s in secs if s[0] == ".text" and s[2] > 0x100000)
_, va, vs, ro, rs = text
chunk = data[ro + (0x5765d0 - va) : ro + (0x5765d0 - va) + 0x40]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
for ins in md.disasm(chunk, image_base + 0x5765d0):
    print(f"0x{ins.address:x}  {ins.bytes.hex():<24s}  {ins.mnemonic:<8s} {ins.op_str}")
    for j, op in enumerate(ins.operands):
        if op.type == capstone.x86.X86_OP_MEM:
            print(f"     op[{j}] MEM base={op.mem.base} index={op.mem.index} disp=0x{op.mem.disp:x} segment={op.mem.segment}")
            print(f"            X86_REG_RIP={capstone.x86.X86_REG_RIP} X86_REG_INVALID={capstone.x86.X86_REG_INVALID}")
            target = ins.address + ins.size + op.mem.disp
            print(f"            -> target 0x{target:x}  (RVA 0x{target - image_base:x})")
    if (ins.address - image_base) > 0x576620:
        break
