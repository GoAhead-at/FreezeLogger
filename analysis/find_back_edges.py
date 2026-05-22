#!/usr/bin/env python3
"""
Phase 2: locate the two AB-BA back-edges between the LockA and LockB
acquirer sets.

The deadlock requires:
  - Thread T1 inside id 19369 (LockA acquirer) calls something that
    transitively reaches a LockB acquirer (id 40285/40333/40334/40335).
  - Thread T2 inside any LockB acquirer calls something that
    transitively reaches id 19369.

This script disassembles the entire `.text` once, builds a function-
level direct-call graph keyed by Address Library id, then runs BFS
from each lock acquirer with a depth limit. For each match the
shortest path is reported.

Vtable-dispatched calls (`call qword ptr [reg + disp]`) are recorded
as opaque "indirect+disp" edges. They are *not* expanded by default.
If direct-call BFS finds nothing, add `--expand-vtables` to fan
indirect calls out across every named CommonLibSSE-NG vtable that has
a hit for an acquirer in the matching slot.
"""

from __future__ import annotations

import argparse
import bisect
import re
import struct
import sys
from collections import defaultdict, deque
from pathlib import Path

import capstone

EXE = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe")
ADDRLIB = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")
OFFSETS_VTABLE = Path(
    r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\armor_crash\build-vs2026"
    r"\vcpkg_installed\vcpkg\blds\commonlibsse-ng\src\e241773cc6-89709b6876.clean"
    r"\include\RE\Offsets_VTABLE.h"
)

# Final, audited acquirer sets per Phase 1.5.
LOCKA_ACQUIRERS = {0x296c00}  # id 19369
LOCKB_ACQUIRERS = {0x6d37b0, 0x6d9720, 0x6d9890, 0x6d99d0}  # id 40285/40333/40334/40335
ACQUIRER_NAMES = {
    0x296c00: "id19369 (LockA)",
    0x6d37b0: "id40285 (LockB)",
    0x6d9720: "id40333 (LockB)",
    0x6d9890: "id40334 (LockB)",
    0x6d99d0: "id40335 (LockB)",
}

DEPTH_LIMIT = 7

VTABLE_RE = re.compile(
    r"VTABLE_(?P<name>[A-Za-z0-9_]+)\s*\{[^}]*?REL::VariantID\s*\(\s*"
    r"(?P<se>\d+)\s*,\s*(?P<ae>\d+)\s*,\s*(?P<vr>0x[0-9a-fA-F]+|\d+)\s*\)"
)

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


def find_text(sections):
    for (n, va, vs, ro, rs) in sections:
        if n == ".text" and vs > 0x100000:
            return (va, vs, ro, rs)
    raise SystemExit("no main .text section found")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=DEPTH_LIMIT,
                    help=f"BFS depth limit (default {DEPTH_LIMIT})")
    ap.add_argument("--expand-vtables", action="store_true",
                    help="Also expand `call qword ptr [reg + disp]` to all candidates")
    ap.add_argument("--max-paths", type=int, default=8,
                    help="Cap the number of distinct paths to print per direction")
    args = ap.parse_args()

    data, image_base, sections = parse_pe(EXE)
    text_va, text_vs, text_ro, text_rs = find_text(sections)
    text_bytes = data[text_ro:text_ro + min(text_vs, text_rs)]
    print(f"[+] image base 0x{image_base:x}, .text va=0x{text_va:x}", file=sys.stderr)

    _, _, entries = parse(ADDRLIB)
    by_off = sorted(entries, key=lambda e: e[1])
    offsets = [e[1] for e in by_off]
    ids_for_off = [e[0] for e in by_off]
    rva_to_id = {off: eid for eid, off in entries}

    def function_rva(rva: int) -> int:
        """Return the RVA of the enclosing function (the nearest preceding addrlib id)."""
        i = bisect.bisect_right(offsets, rva) - 1
        return offsets[i] if i >= 0 else 0

    def name(rva: int) -> str:
        eid = rva_to_id.get(rva)
        return f"id{eid}@0x{rva:x}" if eid else f"?@0x{rva:x}"

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True

    # call_graph[fn_rva] = {(target_fn_rva, kind, detail), ...}
    call_graph: dict[int, set[tuple[int, str, int]]] = defaultdict(set)
    # reverse_graph for "who calls X?" lookups
    reverse_graph: dict[int, set[int]] = defaultdict(set)
    # Index for vtable expansion
    indirect_sites_by_offset: dict[int, set[int]] = defaultdict(set)

    print("[+] linear disassembly + call-graph build...", file=sys.stderr)
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
        cur_fn = function_rva(ins.address - image_base)

        if op.type == capstone.x86.X86_OP_IMM:
            tgt_rva = op.imm - image_base
            tgt_fn = function_rva(tgt_rva)
            if tgt_fn == cur_fn:
                continue  # intra-function (e.g., nested labels), ignore
            call_graph[cur_fn].add((tgt_fn, "direct", 0))
            reverse_graph[tgt_fn].add(cur_fn)
        elif op.type == capstone.x86.X86_OP_MEM:
            mem = op.mem
            # `call qword ptr [reg + disp]` with no rip base is a vtable / function-pointer
            # dispatch; treat the disp as the slot offset.
            if mem.base != capstone.x86.X86_REG_RIP and mem.disp != 0 and mem.disp >= 0:
                slot = mem.disp
                indirect_sites_by_offset[slot].add(cur_fn)
                # Place a placeholder edge: cur_fn -> indirect@slot. The vtable-expand
                # pass below replaces these with concrete target sets when --expand-vtables.
                call_graph[cur_fn].add((-1, "indirect", slot))

    print(f"[+] graph: {len(call_graph)} functions, "
          f"{sum(len(v) for v in call_graph.values()):,} edges", file=sys.stderr)

    vtable_offset_to_targets: dict[int, set[int]] = defaultdict(set)

    if args.expand_vtables:
        print("[+] expanding vtables (parsing Offsets_VTABLE.h, scanning .rdata)...", file=sys.stderr)
        # Parse VTABLE_* { REL::VariantID(SE, ...) ... } and resolve SE id to RVA.
        text_h = OFFSETS_VTABLE.read_text(encoding="utf-8", errors="replace")
        vtable_rvas: list[tuple[int, str]] = []  # (rva, name)
        for m in VTABLE_RE.finditer(text_h):
            se_id = int(m.group("se"))
            if se_id in rva_to_id.values() or se_id in {eid for eid, _ in entries}:
                # We need the rva for this se_id; use the addrlib entries dict.
                pass
        # Build se_id -> rva via entries.
        se_to_rva = {eid: off for eid, off in entries}
        for m in VTABLE_RE.finditer(text_h):
            se_id = int(m.group("se"))
            vname = m.group("name")
            rva = se_to_rva.get(se_id)
            if rva is not None:
                vtable_rvas.append((rva, vname))
        vtable_rvas.sort()

        # Walk every vtable: for each slot, read the qword (image_base + target_rva),
        # bucket by slot offset, accumulate function targets.
        # Limit each vtable to first 0x800 bytes (256 slots).
        for (vt_rva, vname) in vtable_rvas:
            # Find the section containing vt_rva
            for (n, va, vs, ro, rs) in sections:
                if rs == 0 or va <= vt_rva < va + vs:
                    sec_off = ro + (vt_rva - va)
                    sec_end = ro + min(vs, rs)
                    max_slots = 0x100  # 256 slots = 2 KB
                    for slot_idx in range(max_slots):
                        off = sec_off + slot_idx * 8
                        if off + 8 > sec_end:
                            break
                        qw = struct.unpack_from("<Q", data, off)[0]
                        if qw == 0:
                            continue
                        tgt_rva = qw - image_base
                        # Sanity: a valid function pointer is in .text
                        if not (text_va <= tgt_rva < text_va + text_vs):
                            continue
                        slot_disp = slot_idx * 8
                        vtable_offset_to_targets[slot_disp].add(tgt_rva)
                    break

        print(f"    expanded {len(vtable_rvas)} vtables, recorded {len(vtable_offset_to_targets)} distinct slot offsets",
              file=sys.stderr)
        # Update call_graph: replace each (-1, 'indirect', slot) with all candidate targets.
        # Don't bloat; cap fanout per slot.
        FANOUT_CAP = 4000  # 4000 covers slot +0x8 (the dtor slot) which is in nearly every vtable
        replaced = 0
        for cur_fn, edges in list(call_graph.items()):
            new_edges: set[tuple[int, str, int]] = set()
            for (tgt, kind, slot) in edges:
                if kind == "indirect":
                    targets = vtable_offset_to_targets.get(slot, set())
                    if 0 < len(targets) <= FANOUT_CAP:
                        for t in targets:
                            new_edges.add((function_rva(t), f"vtable+0x{slot:x}", slot))
                            reverse_graph[function_rva(t)].add(cur_fn)
                            replaced += 1
                    else:
                        # Keep the placeholder so we can still see it
                        new_edges.add((tgt, kind, slot))
                else:
                    new_edges.add((tgt, kind, slot))
            call_graph[cur_fn] = new_edges
        print(f"    replaced {replaced} indirect edges with concrete vtable targets",
              file=sys.stderr)

    # ---- BFS for shortest paths ----
    def bfs_paths(starts: set[int], goals: set[int], depth_limit: int) -> list[tuple[list[int], list[str]]]:
        """Return list of (path_rvas, path_edge_kinds) -- shortest first, no duplicates per goal."""
        results: list[tuple[list[int], list[str]]] = []
        seen_per_goal: dict[int, int] = {}  # goal -> best depth seen
        # frontier: list of (current_fn, path_so_far_rvas, path_kinds)
        frontier: deque[tuple[int, list[int], list[str]]] = deque()
        global_visited: dict[int, int] = {}  # fn_rva -> shortest depth reached
        for s in starts:
            frontier.append((s, [s], []))
            global_visited[s] = 0
        while frontier:
            cur, path, kinds = frontier.popleft()
            depth = len(path) - 1
            if depth >= depth_limit:
                continue
            for (tgt, kind, _detail) in call_graph.get(cur, ()):
                if tgt < 0:
                    continue  # unexpanded indirect placeholder
                if tgt in starts:
                    continue  # don't go back into starting set
                new_depth = depth + 1
                # Visit-once-per-shortest-depth optimisation:
                if tgt in global_visited and global_visited[tgt] <= new_depth:
                    # We've reached tgt by a shorter or equal path; don't enqueue again
                    if tgt not in goals:
                        continue
                else:
                    global_visited[tgt] = new_depth
                if tgt in goals:
                    if seen_per_goal.get(tgt, 99) > new_depth or seen_per_goal.get(tgt, 99) == new_depth:
                        if seen_per_goal.get(tgt, 99) > new_depth:
                            seen_per_goal[tgt] = new_depth
                            results.append((path + [tgt], kinds + [kind]))
                        elif len(results) < args.max_paths:
                            results.append((path + [tgt], kinds + [kind]))
                    continue
                frontier.append((tgt, path + [tgt], kinds + [kind]))
        return results

    def report(label: str, starts: set[int], goals: set[int]) -> None:
        print()
        print("=" * 100)
        print(f"BFS  {label}    starts={len(starts)}  goals={len(goals)}  depth<= {args.depth}")
        print("=" * 100)
        results = bfs_paths(starts, goals, args.depth)
        if not results:
            print("  NO PATH FOUND.\n  The back-edge for this direction is not via direct calls;")
            print("  re-run with --expand-vtables to include vtable dispatches.")
            return
        # Sort by depth, then by goal id
        results.sort(key=lambda r: (len(r[0]), r[0][-1]))
        for path, kinds in results[:args.max_paths]:
            arrow = []
            for i, fn in enumerate(path):
                arrow.append(name(fn))
                if i < len(kinds):
                    arrow.append(f" --[{kinds[i]}]--> ")
            print(f"  depth {len(path) - 1}:  " + "".join(arrow))

    report("LockA  ->  LockB   (id 19369 transitively reaches a ProcessLists method)",
           starts=LOCKA_ACQUIRERS, goals=LOCKB_ACQUIRERS)
    report("LockB  ->  LockA   (any ProcessLists method transitively reaches id 19369)",
           starts=LOCKB_ACQUIRERS, goals=LOCKA_ACQUIRERS)

    print()
    print("=" * 100)
    print("Indirect-call slot-offset histogram inside the lock-acquirer functions")
    print("=" * 100)
    for fn_rva in sorted(LOCKA_ACQUIRERS | LOCKB_ACQUIRERS):
        fn_name = ACQUIRER_NAMES.get(fn_rva, f"0x{fn_rva:x}")
        slots = [(slot, kind) for (_t, kind, slot) in call_graph.get(fn_rva, ()) if kind in ("indirect",)]
        slots_set = sorted({s for s, _ in slots})
        if not slots_set:
            print(f"\n  {fn_name}:  no `call [reg+N]` indirect dispatches in body")
            continue
        print(f"\n  {fn_name}:  indirect dispatch slots: " +
              ", ".join(f"+0x{s:x}" for s in slots_set))

    return 0


if __name__ == "__main__":
    sys.exit(main())
