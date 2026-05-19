#!/usr/bin/env python3
"""Quick reverse lookup: name some specific RVAs we discovered."""
import bisect
import sys

sys.path.insert(0, r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\analysis")
from addrlib_lookup import parse, ADDRLIB

EXTRA = [
    (0x5b3e40, "helper called RIGHT BEFORE the wait inside Main::Update (dispatches work)"),
    (0x576d80, "helper called from id 34557 lock method (slot 1)"),
    (0x576da0, "helper called from id 34557 lock method (slot 2)"),
    (0x5e2b60, "helper called from JOB BODY entry"),
    (0x6e4540, "helper called from JOB BODY (table lookup)"),
    (0xc0dc30, "called from worker entry (registers task?)"),
    (0xc31e50, "called from id 34556"),
    (0xc026d0, "called by many functions (debug log helper?)"),
    (0xc01800, "called by many (logger?)"),
    (0xc31f40, "iter helper from id 34555"),
    (0xc37fa0, "called early in Main::Update"),
]

_, _, entries = parse(ADDRLIB)
by_off = sorted(entries, key=lambda e: e[1])
offsets = [e[1] for e in by_off]
ids_for_off = [e[0] for e in by_off]

for rva, note in EXTRA:
    i = bisect.bisect_right(offsets, rva) - 1
    near_off = offsets[i]
    near_id = ids_for_off[i]
    print(f"0x{rva:<8x}  ID={near_id:<8d}  fn_entry=0x{near_off:<8x}  delta=+{rva-near_off:<5d}  {note}")
