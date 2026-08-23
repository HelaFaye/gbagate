#!/usr/bin/env python3
"""
build_chipdb.py -- turn the mmbn-chip-tables .asm files into a packed binary
the ESP32 can mmap out of flash and index without parsing anything at runtime.

    python3 build_chipdb.py <tables_dir> <out.bin> [--game bn6g]

Layout:

    magic  "CHDB"            4
    ver    2                 1
    game   ascii, padded     7    e.g. "bn6g"
    count  u16               2    number of records
    max_id u16               2    highest wire ID in the map
    names_off  u32           4    offset of the string pool
    idmap_off  u32           4    offset of the wire-ID lookup
    records[count]           16 each
    name_offsets[count]      4 each, into the string pool
    idmap[max_id + 1]        2 each, record index or 0xFFFF
    string pool              NUL-terminated names

Record, 16 bytes, indexed by chip table index:

    0  u16 index
    2  u8  codes bitmask    bit0..25 = A..Z packed into 4 bytes? no -- see below
    ...

Concretely:

    off size field
    0   2    index          chip table index (== the "ID Sort" order key)
    2   1    element        0 none, 1 fire, 2 aqua, 3 elec, 4 wood
    3   1    library        0 none, 1 standard, 2 mega, 3 giga, 4 dark
    4   1    mb             memory cost
    5   1    gate_uses      Chip Gate Use Limit (1 or 3)
    6   2    damage
    8   2    lib_number     position within its library
    10  1    rarity         1..5 stars
    11  1    flags          bit0 show_in_library, bit1 show_damage
    12  4    codes          four code bytes, 0xFF = none, 26 = '*'

The name pool is separate so records stay fixed width and the whole table can be
bsearched or indexed directly.

The wire-ID map
---------------
`idmap` answers the question the reader actually asks: given the number formed
by a chip's shorted edge pads, which record is it? Pass --names pointing at
mGBA's res/ directory and the builder derives it from mGBA's own chip-names.txt,
where the chip ID is simply the 1-based line number (blank lines still consume
an ID). See BattleChipModel::reloadAssets in mGBA for that.

Names differ between the two sources -- mGBA spells them out ("MegaCannon",
"SuperVulcan") while the chip tables use the 8-character in-game forms
("M-Cannon", "SuprVulc") -- so matching is by abbreviation: a table name matches
when its letters appear in order within the mGBA name. Identity (id == index) is
preferred when it passes that test, so the mapping is verified rather than
assumed, and anything ambiguous is left unmapped and reported.
"""

import argparse
import os
import re
import struct
import sys
import difflib

ELEMENTS = {
    "CHIP_ELEMENT_NONE": 0, "CHIP_ELEMENT_FIRE": 1, "CHIP_ELEMENT_AQUA": 2,
    "CHIP_ELEMENT_ELEC": 3, "CHIP_ELEMENT_WOOD": 4,
}
LIBRARIES = {
    "CHIP_LIBRARY_NONE": 0, "CHIP_LIBRARY_STANDARD": 1, "CHIP_LIBRARY_MEGA": 2,
    "CHIP_LIBRARY_GIGA": 3, "CHIP_LIBRARY_DARK": 4, "CHIP_LIBRARY_PA": 5,
}
RARITY = {"CHIP_RARITY_%d_STAR" % n: n for n in range(1, 6)}

CODE_NONE = 0xFF
CODE_STAR = 26

FLAG_SHOW_IN_LIBRARY = 0x01
FLAG_SHOW_DAMAGE = 0x02


def parse_code(tok):
    tok = tok.strip()
    if tok == "CHIP_CODE_NONE":
        return CODE_NONE
    if tok == "CHIP_CODE_STAR":
        return CODE_STAR
    m = re.match(r"CHIP_CODE_([A-Z])$", tok)
    if m:
        return ord(m.group(1)) - ord("A")
    return CODE_NONE


def parse_int(tok):
    tok = tok.strip()
    try:
        return int(tok, 0)
    except ValueError:
        return 0


def parse_table(path):
    """Yield (index, name, fields dict) per chip entry."""
    text = open(path, encoding="utf-8", errors="replace").read()
    blocks = re.split(r"\n(?=;[0-9A-Fa-f]{4} - )", text)
    for block in blocks:
        head = re.match(r";([0-9A-Fa-f]{4}) - (.*)", block)
        if not head:
            continue
        index = int(head.group(1), 16)
        name = head.group(2).strip()
        fields = {}
        for line in block.split("\n"):
            m = re.search(r"\.(byte|halfword)\s+(.*?);\s*([0-9A-F]{2})(?:-[0-9A-F]{2})?\s+(.*)", line)
            if m:
                fields[m.group(4).strip()] = m.group(2).strip()
        yield index, name, fields


def flag_bits(raw):
    bits = 0
    if "SHOW_IN_LIBRARY" in raw:
        bits |= FLAG_SHOW_IN_LIBRARY
    if "SHOW_DAMAGE" in raw:
        bits |= FLAG_SHOW_DAMAGE
    return bits


FLAVOR = {
    "bn4r": "exe4", "bn4b": "exe4", "exe4r": "exe4", "exe4b": "exe4",
    "bn5p": "exe5", "bn5c": "exe5", "exe5b": "exe5", "exe5c": "exe5",
    "bn6g": "exe6", "bn6f": "exe6", "exe6g": "exe6", "exe6f": "exe6",
}

NO_RECORD = 0xFFFF

# Tuned so BN4 and BN5 land above 90% without inventing matches; anything
# below this is left unmapped and reported.
MATCH_THRESHOLD = 0.72


def load_wire_names(names_dir, game):
    """mGBA chip ID -> spelled-out name. ID is the 1-based line number."""
    flavor = FLAVOR.get(game)
    if not flavor:
        return {}
    path = os.path.join(names_dir, flavor, "chip-names.txt")
    if not os.path.exists(path):
        return {}
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for chip_id, line in enumerate(fh, start=1):
            text = line.strip()
            if text:
                out[chip_id] = text
    return out


def normalise(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def is_abbreviation(short, long_):
    """True if every character of short appears in order within long_.

    The chip tables cap names at 8 characters, so "M-Cannon" abbreviates
    "MegaCannon" and "SuprVulc" abbreviates "SuperVulcan". A subsequence test
    catches both without hand-written rules.
    """
    if short == long_:
        return True
    if len(short) > len(long_):
        return False
    it = iter(long_)
    return all(ch in it for ch in short)


def build_id_map(wire_names, table_names):
    """wire ID -> chip table index.

    Two passes. First accept identity (id == index) wherever the names agree,
    since that is the expected relationship and confirming it beats assuming
    it. Then resolve whatever is left by global best-first assignment with
    exclusivity, so one table entry cannot be claimed twice.

    Whatever remains unmapped is reported rather than guessed. For BN6 that is
    a large block above id 320 -- the Navi V-chips, which the Gregar table does
    not carry at those indices -- and it is correct for those to stay unmapped.
    """
    id_map = {}
    used = set()

    norm_table = {idx: normalise(n) for idx, n in table_names.items() if n}

    for chip_id, wire_name in sorted(wire_names.items()):
        target = normalise(wire_name)
        if chip_id in norm_table and is_abbreviation(norm_table[chip_id], target):
            id_map[chip_id] = chip_id
            used.add(chip_id)

    scored = []
    for chip_id, wire_name in wire_names.items():
        if chip_id in id_map:
            continue
        target = normalise(wire_name)
        for idx, table_name in norm_table.items():
            if idx in used:
                continue
            score = difflib.SequenceMatcher(None, table_name, target).ratio()
            if is_abbreviation(table_name, target):
                score += 0.25
            if table_name[:1] == target[:1]:
                score += 0.10
            scored.append((score, chip_id, idx))

    scored.sort(reverse=True)
    for score, chip_id, idx in scored:
        if score < MATCH_THRESHOLD:
            break
        if chip_id in id_map or idx in used:
            continue
        id_map[chip_id] = idx
        used.add(idx)

    unmapped = [(cid, wire_names[cid]) for cid in sorted(wire_names)
                if cid not in id_map]
    return id_map, unmapped


def build(table_path, game, out_path, names_dir=None):
    records = []
    pool = bytearray()
    offsets = {}

    def intern(name):
        if name not in offsets:
            offsets[name] = len(pool)
            pool.extend(name.encode("ascii", "replace") + b"\0")
        return offsets[name]

    entries = list(parse_table(table_path))
    if not entries:
        sys.exit("no chip entries found in %s" % table_path)

    index_to_record = {}
    name_offsets = []
    table_names = {}

    for index, name, f in entries:
        codes = bytes(parse_code(f.get("Code%d" % n, "CHIP_CODE_NONE")) for n in range(1, 5))
        rec = struct.pack(
            "<HBBBBHHBB4s",
            index,
            ELEMENTS.get(f.get("Element", ""), 0),
            LIBRARIES.get(f.get("Library", ""), 0),
            parse_int(f.get("MB", "0")),
            parse_int(f.get("Chip Gate Use Limit", f.get("Number of Uses", "0"))),
            parse_int(f.get("Damage", "0")) & 0xFFFF,
            parse_int(f.get("Library Number", "0")) & 0xFFFF,
            RARITY.get(f.get("Chip Rarity", ""), 0),
            flag_bits(f.get("Flags", "")),
            codes,
        )
        assert len(rec) == 16, len(rec)
        index_to_record[index] = len(records)
        records.append(rec)
        name_offsets.append(intern(name))
        table_names[index] = name

    stats = {"ids": 0, "mapped": 0, "unmapped": 0, "identity": 0}
    id_map = {}
    if names_dir:
        wire_names = load_wire_names(names_dir, game)
        if wire_names:
            resolved, unmapped = build_id_map(wire_names, table_names)
            for chip_id, index in resolved.items():
                if index in index_to_record:
                    id_map[chip_id] = index_to_record[index]
                    if chip_id == index:
                        stats["identity"] += 1
            stats["ids"] = len(wire_names)
            stats["mapped"] = len(id_map)
            stats["unmapped"] = len(unmapped)

    max_id = max(id_map) if id_map else 0

    header_size = 4 + 1 + 7 + 2 + 2 + 4 + 4
    records_size = len(records) * 16
    names_array = b"".join(struct.pack("<I", o) for o in name_offsets)
    idmap = b"".join(struct.pack("<H", id_map.get(i, NO_RECORD))
                     for i in range(max_id + 1)) if id_map else b""

    idmap_off = header_size + records_size + len(names_array)
    names_off = idmap_off + len(idmap)

    with open(out_path, "wb") as fh:
        fh.write(b"CHDB")
        fh.write(bytes([2]))
        fh.write(game.encode("ascii")[:7].ljust(7, b"\0"))
        fh.write(struct.pack("<HH", len(records), max_id))
        fh.write(struct.pack("<II", names_off, idmap_off))
        for r in records:
            fh.write(r)
        fh.write(names_array)
        fh.write(idmap)
        fh.write(pool)

    return len(records), stats, os.path.getsize(out_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tables_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--names", metavar="MGBA_RES_DIR",
                    help="mGBA res/ directory, for exeN/chip-names.txt")
    ap.add_argument("--game", action="append", default=None)
    args = ap.parse_args()

    games = args.game or ["bn4r", "bn4b", "bn5p", "bn5c", "bn6g", "bn6f"]
    os.makedirs(args.out_dir, exist_ok=True)

    total = 0
    print("%-6s %5s %5s %6s %6s %6s %8s"
          % ("game", "chips", "ids", "mapped", "ident", "unmap", "bytes"))
    for game in games:
        path = os.path.join(args.tables_dir, "%s_chip_table.asm" % game)
        if not os.path.exists(path):
            print("skip %-6s (no table)" % game)
            continue
        out = os.path.join(args.out_dir, "%s.chdb" % game)
        n, st, size = build(path, game, out, args.names)
        total += size
        print("%-6s %5d %5d %6d %6d %6d %8d"
              % (game, n, st["ids"], st["mapped"], st["identity"],
                 st["unmapped"], size))
    print("---\ntotal %d B" % total)
    if not args.names:
        print("note: no --names given, so the wire-ID map is empty")


if __name__ == "__main__":
    main()
