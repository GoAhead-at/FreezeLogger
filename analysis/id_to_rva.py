#!/usr/bin/env python3
"""Look up the entry RVA for a list of Address Library IDs."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from addrlib_lookup import parse, ADDRLIB

IDS = [19369, 40285, 40333, 40334, 40335, 40706]


def main() -> int:
    name, ptr_size, entries = parse(ADDRLIB)
    by_id = {eid: off for eid, off in entries}
    for i in IDS:
        if i in by_id:
            print(f"id {i:>5d}  ->  RVA 0x{by_id[i]:x}")
        else:
            print(f"id {i:>5d}  ->  NOT FOUND")
    return 0


if __name__ == "__main__":
    sys.exit(main())
