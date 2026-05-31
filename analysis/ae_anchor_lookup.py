#!/usr/bin/env python3
"""Resolve the deep-probe anchor IDs on AE 1.6.1170 and cross-check the
SE 1.5.97 RVAs. Confirms whether REL::VariantID(seID, aeID, ...) with the
SAME id maps cleanly across SE and AE.

Anchors:
  WaitForJobTask   -> ID 68167  (SE RVA 0xc38130)
  Singleton-B ptr  -> ID 516902 (SE RVA 0x2f26a70, a .data global)
Hooks (sanity):
  Main::Update     -> ID 35551 (SE) / 36544 (AE)
  Init_InitD3D     -> ID 75595 (SE) / 77226 (AE)
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse

SE_BIN = Path(r"D:\SPIELE\nolvus\Instances\Nolvus Awakening\MODS\mods\Address Library for SKSE Plugins\SKSE\Plugins\version-1-5-97-0.bin")
AE_BIN = Path(r"E:\SHARED\_STAEUBER\DEV\Projects\Cursor\Skyrim\freeze-detector\AddressLibrary\All in one (1.6.X)\SKSE\Plugins\versionlib-1-6-1170-0.bin")

ANCHORS = [
    ("WaitForJobTask",  68167,  68167),
    ("Singleton-B ptr", 516902, 516902),
]
HOOKS = [
    ("Main::Update",  35551, 36544),
    ("Init_InitD3D",  75595, 77226),
]


def load(p):
    name, ptr, entries = parse(p)
    return {eid: off for eid, off in entries}


def main():
    se = load(SE_BIN)
    ae = load(AE_BIN)
    print(f"\nSE entries: {len(se)}   AE entries: {len(ae)}\n")
    print(f"{'what':<18s} {'id(se/ae)':>14s} {'SE RVA':>12s} {'AE RVA':>12s}")
    for name, sid, aid in ANCHORS + HOOKS:
        srva = se.get(sid)
        arva = ae.get(aid)
        s = f"0x{srva:x}" if srva is not None else "MISSING"
        a = f"0x{arva:x}" if arva is not None else "MISSING"
        idcol = f"{sid}/{aid}" if sid != aid else str(sid)
        print(f"{name:<18s} {idcol:>14s} {s:>12s} {a:>12s}")


if __name__ == "__main__":
    sys.exit(main())
