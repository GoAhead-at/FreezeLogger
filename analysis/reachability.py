#!/usr/bin/env python3
"""
Phase 4 prep: given a set of source function ids and a set of goal
function ids, find the shortest direct-call path from any source to
any goal.

Direct calls only -- no vtable expansion. We've confirmed in Phase
3 / 3.5 that vtable expansion produces the right back-edges for the
runtime cycle, but here we want to ask "if I unconditionally enter
this concrete function, can I reach this concrete function via direct
calls only?". That tells us whether a dispatch site's runtime target
will pull control flow into id 19369 (the LockA acquirer) or any
LockB acquirer transitively.
"""

from __future__ import annotations

import argparse
import bisect
import struct
import sys
from collections import defaultdict, deque
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")

LOCKA_RVAS = {0x296c00}                                 # id 19369
LOCKB_RVAS = {0x6d37b0, 0x6d9720, 0x6d9890}             # id 40285/40333/40334 (id 40335 is BSSpinLock::Unlock, see doc 21)

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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=8)
    ap.add_argument("--sources", required=True,
                    help="Comma-separated addrlib ids to start BFS from")
    ap.add_argument("--goals", default="locka,lockb",
                    help='"locka", "lockb", "all", OR a comma-separated id list')
    ap.add_argument("--max-paths", type=int, default=4)
    a = ap.parse_args()

    data, image_base, sections = parse_pe(EXE)
    text_va = text_vs = text_ro = text_rs = 0
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            text_va, text_vs, text_ro, text_rs = va, vs, ro, rs
            break
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]

    _, _, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]
    rva_to_id = {off: eid for eid, off in entries}

    def function_rva(rva: int) -> int:
        i = bisect.bisect_right(offsets, rva) - 1
        return offsets[i] if i >= 0 else 0

    def name(rva: int) -> str:
        eid = rva_to_id.get(rva)
        return f"id{eid}" if eid else f"@0x{rva:x}"

    def parse_id_set(spec: str) -> set[int]:
        if spec == "locka":
            return set(LOCKA_RVAS)
        if spec == "lockb":
            return set(LOCKB_RVAS)
        if spec == "all":
            return set(LOCKA_RVAS) | set(LOCKB_RVAS)
        out: set[int] = set()
        for part in spec.split(","):
            part = part.strip()
            if not part:
                continue
            if part in ("locka", "lockb", "all"):
                out |= parse_id_set(part)
            else:
                eid = int(part)
                if eid not in by_id:
                    raise SystemExit(f"unknown addrlib id {eid}")
                out.add(by_id[eid])
        return out

    sources = parse_id_set(a.sources)
    goals = parse_id_set(a.goals)
    print(f"[+] sources: {sorted(name(s) for s in sources)}", file=sys.stderr)
    print(f"[+] goals:   {sorted(name(g) for g in goals)}", file=sys.stderr)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True

    call_graph: dict[int, set[int]] = defaultdict(set)
    print("[+] building direct-call graph...", file=sys.stderr)
    progress = 0
    for ins in md.disasm(text_bytes, image_base + text_va):
        progress += 1
        if progress % 4_000_000 == 0:
            print(f"    [{progress:,} insns]", file=sys.stderr)
        if ins.mnemonic != "call":
            continue
        try:
            ops = ins.operands
        except capstone.CsError:
            continue
        if not ops:
            continue
        op = ops[0]
        if op.type != capstone.x86.X86_OP_IMM:
            continue
        cur_fn = function_rva(ins.address - image_base)
        tgt_rva = op.imm - image_base
        tgt_fn = function_rva(tgt_rva)
        if tgt_fn == cur_fn:
            continue
        call_graph[cur_fn].add(tgt_fn)
    print(
        f"[+] call_graph: {len(call_graph)} fns, "
        f"{sum(len(v) for v in call_graph.values()):,} edges",
        file=sys.stderr,
    )

    def bfs(start: int, depth_limit: int) -> dict[int, list[int]]:
        """For one start, return goal -> shortest path RVA list. Empty if unreachable."""
        if start in goals:
            return {start: [start]}
        global_visited: dict[int, int] = {start: 0}
        parent: dict[int, int | None] = {start: None}
        frontier: deque[int] = deque([start])
        found: dict[int, list[int]] = {}
        while frontier:
            cur = frontier.popleft()
            depth = global_visited[cur]
            if depth >= depth_limit:
                continue
            for tgt in call_graph.get(cur, ()):
                if tgt in global_visited:
                    continue
                global_visited[tgt] = depth + 1
                parent[tgt] = cur
                if tgt in goals:
                    path = [tgt]
                    p = parent[tgt]
                    while p is not None:
                        path.append(p)
                        p = parent[p]
                    path.reverse()
                    found[tgt] = path
                    if len(found) == len(goals):
                        return found
                frontier.append(tgt)
        return found

    print()
    print("=" * 96)
    print(f"REACHABILITY (direct calls only, depth <= {a.depth})")
    print("=" * 96)
    for src in sorted(sources):
        print(f"\n  from {name(src)}  (RVA 0x{src:x}):")
        found = bfs(src, a.depth)
        if not found:
            print(f"    NO path to any goal at depth <= {a.depth}")
            continue
        for goal, path in found.items():
            display = " -> ".join(name(r) for r in path)
            print(f"    -> {name(goal):<10s}  depth {len(path) - 1}  via  {display}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
