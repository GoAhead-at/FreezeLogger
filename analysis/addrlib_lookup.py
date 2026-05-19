#!/usr/bin/env python3
"""
Parse meh321 Address Library .bin (format v1) and resolve RVAs to IDs.

Decoder mirrors REL::IDDatabase::unpack_file in CommonLibSSE-NG.
"""

import bisect
import struct
import sys
from pathlib import Path

ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

# RVAs to look up (came from WinDbg as SkyrimSE+0x????)
RVAS = [
    # Main thread
    (0x5765ff, "main: lock primitive call (blocker)"),
    (0x5b35dd, "main: caller inside Main::Update"),
    (0x5af4f4, "main: tick caller"),
    (0x5acc05, "main: outer game loop"),
    (0x134b17a, "main: WinMain / message loop"),
    # Render thread
    (0x576770, "render: lock primitive (sibling)"),
    (0x576d56, "render: caller using lock"),
    # Worker pool entry chain (shared)
    (0xc0d6bd, "workers: thread entry"),
    (0xc34c48, "workers: outer dispatcher"),
    (0xc32a81, "workers: inner dispatcher"),
    (0x6d4a31, "workers: dispatch"),
    (0x6d4b5a, "workers: dispatch variant 1"),
    (0x6d43ec, "workers: dispatch variant 2"),
    (0x5d6b16, "workers: deeper"),
    (0x5d842e, "workers: deeper 2"),
    # The job all 4 workers are running
    (0x6d468a, "workers: JOB BODY (common)"),
    (0x6ef480, "workers: called from job body"),
    (0x61c780, "workers: deeper job"),
    (0x6025fb, "workers: deeper job"),
    (0x296c3d, "workers: spinwait/sleep wrapper"),
    (0x297ddb, "workers: variant"),
    (0x22d176, "workers: variant"),
    (0x2971a4, "workers: variant"),
    (0x6d9750, "workers: variant"),
    (0x132c5a, "workers: Skyrim Sleep wrapper"),
    (0xc349c3, "workers: idle wait"),
]


def parse(path: Path):
    data = path.read_bytes()
    pos = 0

    def u8():
        nonlocal pos
        v = data[pos]
        pos += 1
        return v

    def u16():
        nonlocal pos
        v = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        return v

    def u32():
        nonlocal pos
        v = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        return v

    def u64():
        nonlocal pos
        v = struct.unpack_from("<Q", data, pos)[0]
        pos += 8
        return v

    fmt = u32()
    if fmt != 1:
        raise SystemExit(f"unexpected format: {fmt}")
    ver = (u32(), u32(), u32(), u32())
    name_len = u32()
    name = data[pos:pos + name_len].decode("ascii", errors="replace")
    pos += name_len
    ptr_size = u32()
    count = u32()
    print(f"[+] addrlib: format={fmt} version={'.'.join(map(str,ver))} name={name!r} ptrSize={ptr_size} count={count}", file=sys.stderr)

    prev_id = 0
    prev_off = 0
    entries = []

    for _ in range(count):
        t = u8()
        lo = t & 0x0F
        hi = (t >> 4) & 0x0F

        if lo == 0:
            cur_id = u64()
        elif lo == 1:
            cur_id = prev_id + 1
        elif lo == 2:
            cur_id = prev_id + u8()
        elif lo == 3:
            cur_id = prev_id - u8()
        elif lo == 4:
            cur_id = prev_id + u16()
        elif lo == 5:
            cur_id = prev_id - u16()
        elif lo == 6:
            cur_id = u16()
        elif lo == 7:
            cur_id = u32()
        else:
            raise SystemExit(f"bad id_method {lo}")

        tmp = (prev_off // ptr_size) if (hi & 8) else prev_off
        method = hi & 7

        if method == 0:
            cur_off = u64()
        elif method == 1:
            cur_off = tmp + 1
        elif method == 2:
            cur_off = tmp + u8()
        elif method == 3:
            cur_off = tmp - u8()
        elif method == 4:
            cur_off = tmp + u16()
        elif method == 5:
            cur_off = tmp - u16()
        elif method == 6:
            cur_off = u16()
        elif method == 7:
            cur_off = u32()

        if hi & 8:
            cur_off *= ptr_size

        entries.append((cur_id, cur_off))
        prev_id = cur_id
        prev_off = cur_off

    return name, ptr_size, entries


def main() -> int:
    name, ptr_size, entries = parse(ADDRLIB)
    print(f"[+] entries parsed: {len(entries)}", file=sys.stderr)

    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]
    print(f"[+] offset range: 0x{offsets[0]:x} .. 0x{offsets[-1]:x}", file=sys.stderr)
    print()
    print(f"{'RVA':<10s}  {'NEAREST_RVA':<11s}  {'DELTA':>7s}  {'ID':>8s}  Note")
    for rva, note in RVAS:
        i = bisect.bisect_right(offsets, rva) - 1
        if i < 0:
            print(f"0x{rva:<8x}  (none)            -        -  {note}")
            continue
        near_off = offsets[i]
        near_id = ids_for_off[i]
        delta = rva - near_off
        print(f"0x{rva:<8x}  0x{near_off:<9x}  {delta:>+7d}  {near_id:>8d}  {note}")

    # Also: show top of nearest blocks (so we know the function entry)
    print()
    print("RVA -> next 3 entries (helpful when delta is large):")
    for rva, note in RVAS[:5]:
        i = bisect.bisect_right(offsets, rva) - 1
        for k in range(max(0, i - 0), min(len(offsets), i + 3)):
            off = offsets[k]
            mid = ids_for_off[k]
            tag = "<--" if k == i else ""
            print(f"  0x{rva:<8x}  block#{k}  off=0x{off:<8x} id={mid:<8d}  {tag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
