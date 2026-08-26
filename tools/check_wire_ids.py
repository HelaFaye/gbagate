#!/usr/bin/env python3
"""
check_wire_ids.py -- does the word the gate puts in slot 5 identify the chip the
user actually picked?

Background
----------
Two different numbers identify a chip and they are easy to confuse:

  * the **chip table index** -- `chip_record_t.index`, the "ID Sort" order key
    from the mmbn-chip-tables .asm files;
  * the **wire ID** -- the binary number formed by a physical Battle Chip's
    shorted edge pads. `chipdb.h` says this is "the value slot 5 of the gate
    rotation carries", and `chipgate.h` says `chipgate_load()` takes "the raw
    value the game expects, which for a physical Battle Chip is literally the
    binary number formed by its shorted edge pads".

So slot 5 must carry the wire ID. The database stores the mapping one way only,
`idmap[wire_id] -> record slot`, and `gate_app.c` used to reach for the nearest
thing to hand -- `chipdb_get(...); gate_send_chip(rec.index)` -- which sends the
table index. `build_chipdb.py` prefers identity (`wire_id == index`) and falls
back to best-first name matching for the rest, so the two agree for some chips
and diverge for the rest. Where they diverge the game was told about a different
chip than the one on screen, silently, because both values are plausible.

That is now fixed: `chipdb_id_for_slot()` inverts the map and `gate_app.c` sends
its result. So a divergence below is no longer a bug -- it is the measure of how
badly `rec.index` would have been wrong, i.e. why the accessor has to exist.

What this script still guards is the property the inverse *depends* on: the map
must be injective. `build_chipdb.py` enforces that with its `used` set (one table
entry cannot be claimed twice), but nothing re-checks it after packing, and if it
ever broke, `chipdb_id_for_slot()` would silently return the first of several
IDs. Two wire IDs sharing a record slot, or a slot pointing off the end of the
table, fail the run.

    python3 tools/check_wire_ids.py                       # every db in data/chipdb
    python3 tools/check_wire_ids.py data/chipdb/bn6g.chdb
    python3 tools/check_wire_ids.py --list                 # show each divergence

Exit status is 1 only on a broken map, so it can gate a build.
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

MAGIC = b"CHDB"
VERSION = 2
HDR_SIZE = 24
REC_SIZE = 16
NO_RECORD = 0xFFFF


def load(path: str):
    """Parse a .chdb far enough to walk the id map. Mirrors chipdb_open()."""
    with open(path, "rb") as fh:
        blob = fh.read()

    if len(blob) < HDR_SIZE or blob[:4] != MAGIC:
        raise ValueError(f"{path}: not a CHDB blob")
    if blob[4] != VERSION:
        raise ValueError(f"{path}: version {blob[4]}, expected {VERSION}")

    game = blob[5:12].split(b"\0")[0].decode("ascii", "replace")
    count, max_id = struct.unpack_from("<HH", blob, 12)
    names_off, idmap_off = struct.unpack_from("<II", blob, 16)

    records_end = HDR_SIZE + count * REC_SIZE
    names_end = records_end + count * 4
    if idmap_off != names_end:
        raise ValueError(f"{path}: idmap_off {idmap_off} != names_end {names_end}")
    if max_id and idmap_off + (max_id + 1) * 2 > len(blob):
        raise ValueError(f"{path}: id map runs off the end")

    # record slot -> that record's chip table index (field 0 of the record)
    rec_index = [
        struct.unpack_from("<H", blob, HDR_SIZE + slot * REC_SIZE)[0]
        for slot in range(count)
    ]

    idmap = (
        list(struct.unpack_from(f"<{max_id + 1}H", blob, idmap_off))
        if max_id
        else []
    )

    # Names, so a mismatch can be read rather than just counted.
    name_offsets = struct.unpack_from(f"<{count}I", blob, records_end) if count else ()
    names = []
    for off in name_offsets:
        end = blob.find(b"\0", names_off + off)
        names.append(blob[names_off + off:end].decode("ascii", "replace"))

    return game, count, max_id, rec_index, idmap, names


def check(path: str, show: bool) -> tuple[int, int]:
    """Returns (divergences, breakages). Only breakages are fatal."""
    game, count, max_id, rec_index, idmap, names = load(path)

    mapped = [(cid, slot) for cid, slot in enumerate(idmap) if slot != NO_RECORD]

    # Fatal: a slot off the end of the table, or one record claimed by two wire
    # IDs -- either makes chipdb_id_for_slot()'s answer wrong or arbitrary.
    owners: dict[int, list[int]] = {}
    broken = []
    for cid, slot in mapped:
        if slot >= count:
            broken.append(f"wire ID 0x{cid:04X} -> slot {slot}, "
                          f"past the {count}-record table")
            continue
        owners.setdefault(slot, []).append(cid)
    for slot, cids in sorted(owners.items()):
        if len(cids) > 1:
            ids = ", ".join(f"0x{c:04X}" for c in cids)
            broken.append(f"slot {slot} ({names[slot]}) claimed by {ids}")

    # Informational: where rec.index would have named a different chip.
    diverged = [(cid, slot) for cid, slot in mapped
                if slot < count and rec_index[slot] != cid]

    label = os.path.basename(path)
    print(f"{label:16s} game={game:5s} records={count:4d} max_id={max_id:4d} "
          f"mapped={len(mapped):4d} unsendable={count - len(owners):4d} "
          f"diverged={len(diverged):4d}")

    for msg in broken:
        print(f"    BROKEN MAP: {msg}")

    if diverged and show:
        print(f"    {'wire_id':>7s}  {'was_sent':>8s}  chip")
        for cid, slot in diverged:
            was = f"0x{rec_index[slot]:04X}"
            print(f"    0x{cid:04X}  {was:>8s}  {names[slot]}")

    return len(diverged), len(broken)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="one or more .chdb files")
    ap.add_argument("--list", action="store_true", dest="show",
                    help="print every mismatching chip, not just the count")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    paths = args.paths or sorted(
        glob.glob(os.path.join(root, "data", "chipdb", "*.chdb")))
    if not paths:
        print("no .chdb files found", file=sys.stderr)
        return 2

    diverged = broken = 0
    for path in paths:
        try:
            d, b = check(path, args.show)
            diverged += d
            broken += b
        except (OSError, ValueError, struct.error) as exc:
            print(f"{os.path.basename(path)}: {exc}", file=sys.stderr)
            return 2

    print()
    if broken:
        print(f"{broken} broken mapping(s): the wire ID -> record map is not "
              f"injective, so chipdb_id_for_slot() cannot invert it reliably.")
        print("Rebuild with tools/build_chipdb.py and investigate before "
              "trusting slot 5.")
        return 1

    if diverged:
        print(f"map is sound; {diverged} chip(s) have a wire ID that differs "
              f"from their chip table index.")
        print("Those are exactly the chips the old gate_app.c would have "
              "misnamed by sending rec.index. chipdb_id_for_slot() now sends "
              "the wire ID, so they are correct -- this count is the value of "
              "that fix, not a fault.")
    else:
        print("map is sound; every mapped wire ID also equals its record's "
              "chip table index for these databases.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
