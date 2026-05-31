#!/usr/bin/env python3
"""Reverse lookup for the deep-probe RVAs we want to ID-ify for multi-version.

For each target RVA, prints the nearest-below Address Library entry and the
delta. delta == 0 means the RVA is itself a registered ID (ideal: REL::ID
auto-maps it across SE/AE). A non-zero delta on a *data* global usually means
the global is reached relative to a function id and must stay a VariantID.
"""
import bisect
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB

TARGETS = [
    (0xc38130,   "WaitForJobTask (Site B wait helper)"),
    (0x2f26a70,  "Singleton-B task-pool holder (data global)"),
    (0x5b34f9,   "call site -> +0xc38130 inside Main::Update"),
    (0x5b6d70,   "Main::Update (id 35551 expected)"),
]

name, ptr_size, entries = parse(ADDRLIB)
by_off = sorted(entries, key=lambda e: e[1])
offsets = [e[1] for e in by_off]
ids = [e[0] for e in by_off]
exact = {off: eid for eid, off in entries}

print(f"{'RVA':<12s} {'EXACT_ID':>9s} {'NEAREST_ID':>11s} {'ENTRY':>12s} {'DELTA':>8s}  Note")
for rva, note in TARGETS:
    i = bisect.bisect_right(offsets, rva) - 1
    near_off = offsets[i]
    near_id = ids[i]
    ex = exact.get(rva, None)
    ex_s = str(ex) if ex is not None else "-"
    print(f"0x{rva:<10x} {ex_s:>9s} {near_id:>11d} 0x{near_off:<10x} {rva-near_off:>+8d}  {note}")
