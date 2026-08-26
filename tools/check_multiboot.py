#!/usr/bin/env python3
"""
check_multiboot.py -- structural checks on a GBA multiboot image.

tools/fix_header.py already validates the 192-byte cartridge header that every
GBA image shares. This tool checks the part that is specific to a *multiboot*
image and that nothing else in the tree looks at:

  0xC0   RAM entry point      the BIOS jumps here, not to 0x00
  0xC4   boot mode            must ship as 0x00; the BIOS overwrites it
  0xC5   slave ID             must ship as 0x00; the BIOS overwrites it
  0xE0   JOYBUS entry point
  end    non-zero footer      see below

Why the footer matters
----------------------
rom_effective_size() in src/rom_catalog.c strips trailing 0x00 and 0xFF bytes
before a transfer, on the reasonable grounds that most GBA images are padded
out to a cartridge boundary and none of that padding needs sending. An image
that legitimately ends in zeroes -- alignment padding after the last rodata,
say -- would therefore be transferred short, and the failure would appear as a
program that boots and misbehaves rather than as a transfer error. rom/crt0_mb.s
appends four non-zero bytes to make that impossible, and this tool confirms
they survived the link. The check is exact: it reimplements
rom_effective_size() and asserts it does not shorten the file.

Why the branch displacement at 0xC0 matters
-------------------------------------------
mGBA infers "multiboot or cartridge?" from the branch displacement at 0xC0
rather than from any flag, and rejects three particular values -- including 28,
which is exactly what the natural layout (code at 0xE4) produces. A rejected
image is not reported as an error: it is loaded as a cartridge at 0x08000000,
where an EWRAM-linked program cannot run, and the screen simply stays black.
This tool re-implements that test so the failure is caught at build time.

Why the entry points matter
---------------------------
An image linked for the cartridge instead of for EWRAM is still a perfectly
valid .gba file, still passes every header check, and still transfers -- it
just crashes on arrival. The three ARM branches at 0x00, 0xC0 and 0xE0 must
all resolve to the same in-image address, which is the cheapest available
evidence that this file was linked by rom/multiboot.ld and not by rom/tester.ld.

Usage
-----
    python3 tools/check_multiboot.py rom/tester.mb
    python3 tools/check_multiboot.py *.mb

Exit status is 1 if any image failed a check, 0 otherwise. Warnings -- a blank
logo, an absolute cartridge address in the body -- do not change the status,
because both are legitimate in some builds.
"""

import binascii
import sys

# Mirrors src/gba_multiboot.h. The bounds are GBATEK's payload range for
# SWI 25h -- "minimum length 100h, max 3FF40h", excluding header data -- plus
# the 0xC0 header, since these sizes describe a whole image.
MB_HEADER = 0xC0
MB_MIN_SIZE = MB_HEADER + 0x100      # 0x1C0
MB_MAX_SIZE = MB_HEADER + 0x3FF40    # 0x40000
MB_LOAD_ADDR = 0x02000000

# Mirrors rom/crt0_mb.s.
FOOTER = b"GBMB"

# Header offsets.
FIXED_BYTE = 0xB2
CHECK_BYTE = 0xBD
RAM_ENTRY = 0xC0
BOOT_MODE = 0xC4
SLAVE_ID = 0xC5
JOYBUS_ENTRY = 0xE0
LOGO_START, LOGO_END = 0x04, 0xA0


def header_checksum(data: bytes) -> int:
    """0x00 - (0x19 + sum of bytes 0xA0..0xBC). Same as rom_header_checksum()."""
    return (0 - (sum(data[0xA0:CHECK_BYTE]) + 0x19)) & 0xFF


def effective_size(data: bytes) -> int:
    """A faithful reimplementation of rom_effective_size() in rom_catalog.c."""
    size = len(data)
    if size <= MB_HEADER:
        return size
    end = size
    while end > MB_MIN_SIZE and data[end - 1] in (0x00, 0xFF):
        end -= 1
    payload = (end - MB_HEADER + 0x0F) & ~0x0F
    out = payload + MB_HEADER
    return out if out <= size else size


def branch_target(data: bytes, at: int):
    """Decode an unconditional ARM branch, returning an image offset.

    Returns None if the word at `at` is not a `b` instruction. The +8 is the
    ARM pipeline offset: the PC reads two instructions ahead.
    """
    if at + 4 > len(data):
        return None
    word = int.from_bytes(data[at:at + 4], "little")
    if (word >> 24) != 0xEA:
        return None
    off = word & 0x00FFFFFF
    if off & 0x00800000:          # sign-extend the 24-bit offset
        off -= 0x01000000
    return at + 8 + off * 4


def mgba_rejects_as_mb(data: bytes):
    """Would mGBA's GBAIsMB() refuse to see this file as a multiboot image?

    Returns a reason string, or None if mGBA would accept it.

    mGBA does not read a flag to decide whether a file is multiboot or a
    cartridge: it decodes the word at 0xC0 and judges the branch displacement.
    From GBAIsMB() in src/gba/gba.c:

        if (info.op1.immediate <= 0)       return false;
        else if (info.op1.immediate == 28) return false;  // "ancient toolchain"
        else if (info.op1.immediate != 24) return true;
        // a displacement of exactly 24 falls through to a heuristic scan

    The displacement is in bytes and excludes the ARM pipeline's +8, because
    decoder-arm.c computes it as (opcode << 8) >> 6.

    This matters because rejection is silent and does not look like an error.
    A rejected image is simply loaded by GBALoadROM() at 0x08000000 instead of
    by GBALoadMB() into EWRAM, so an EWRAM-linked program is placed somewhere
    it cannot run and the screen stays black with nothing logged.

    The natural layout -- header entries through 0xE3, code immediately after
    at 0xE4 -- produces a displacement of exactly 28 and is rejected. That is
    why rom/crt0_mb.s pads .Lstart to 0xE8.
    """
    if len(data) < RAM_ENTRY + 4:
        return None
    word = int.from_bytes(data[RAM_ENTRY:RAM_ENTRY + 4], "little")
    if (word >> 24) != 0xEA:            # not a plain `b`; not our concern here
        return None
    off = word & 0x00FFFFFF
    if off & 0x00800000:
        off -= 0x01000000
    disp = off * 4                       # what mGBA compares against
    if disp <= 0:
        return (f"the branch at 0xC0 has displacement {disp}; mGBA's GBAIsMB() "
                f"rejects any displacement <= 0")
    if disp == 28:
        return ("the branch at 0xC0 has displacement 28, which mGBA's "
                "GBAIsMB() rejects outright (its 'ancient toolchain' case). "
                "This is what a layout with code at 0xE4 produces -- pad the "
                "entry to 0xE8 as rom/crt0_mb.s does")
    if disp == 24:
        return ("the branch at 0xC0 has displacement 24, which sends mGBA's "
                "GBAIsMB() down a heuristic instruction scan instead of "
                "accepting the image outright; detection becomes contingent "
                "on the body. Pad the entry so the displacement is neither "
                "24 nor 28")
    return None


def check(path: str) -> tuple[list[str], list[str], str]:
    """Returns (errors, warnings, one-line summary) for one image."""
    errors: list[str] = []
    warnings: list[str] = []

    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        return ([f"cannot read: {exc}"], [], "")

    size = len(data)
    if size < MB_MIN_SIZE:
        errors.append(f"size {size} < MB_MIN_SIZE ({MB_MIN_SIZE})")
        return (errors, warnings, "")   # nothing below is meaningful
    if size > MB_MAX_SIZE:
        errors.append(f"size {size} > MB_MAX_SIZE ({MB_MAX_SIZE})")

    # ---- the shared cartridge header ------------------------------------
    if data[FIXED_BYTE] != 0x96:
        errors.append(f"fixed byte 0xB2 is 0x{data[FIXED_BYTE]:02X}, want 0x96")
    want = header_checksum(data)
    if data[CHECK_BYTE] != want:
        errors.append(f"complement 0xBD is 0x{data[CHECK_BYTE]:02X}, "
                      f"want 0x{want:02X} (run tools/fix_header.py)")
    if data[LOGO_START:LOGO_END] == bytes(LOGO_END - LOGO_START):
        warnings.append("logo area is blank -- a real GBA will reject this "
                        "image after the full transfer, with no diagnostic. "
                        "Rebuild with DONOR=a-rom-you-own.gba.")

    # ---- multiboot-specific header entries ------------------------------
    if data[BOOT_MODE] != 0x00:
        errors.append(f"boot mode 0xC4 is 0x{data[BOOT_MODE]:02X}, must ship "
                      f"as 0x00 (the BIOS writes it)")
    if data[SLAVE_ID] != 0x00:
        errors.append(f"slave ID 0xC5 is 0x{data[SLAVE_ID]:02X}, must ship "
                      f"as 0x00 (the BIOS writes it)")

    targets = {}
    for name, at in (("cart entry 0x00", 0x00),
                     ("RAM entry 0xC0", RAM_ENTRY),
                     ("JOYBUS entry 0xE0", JOYBUS_ENTRY)):
        tgt = branch_target(data, at)
        if tgt is None:
            errors.append(f"{name} is not an unconditional ARM branch")
        elif not (0 <= tgt < size):
            errors.append(f"{name} branches to +0x{tgt:X}, outside the image")
        else:
            targets[name] = tgt

    if len(targets) == 3 and len(set(targets.values())) != 1:
        errors.append("the three entry points disagree: "
                      + ", ".join(f"{k} -> +0x{v:X}" for k, v in targets.items())
                      + " (a cartridge-linked image mislabelled .mb?)")

    # ---- will an emulator actually recognise this as multiboot? ----------
    reason = mgba_rejects_as_mb(data)
    if reason is not None:
        errors.append(f"mGBA would not load this as a multiboot image: {reason}")

    # ---- the footer, and what it protects -------------------------------
    if data[-len(FOOTER):] != FOOTER:
        errors.append(f"image does not end with the {FOOTER.decode()} footer "
                      f"(last 4 bytes: {data[-4:].hex()}) -- see rom/crt0_mb.s")
    eff = effective_size(data)
    if eff != size:
        errors.append(f"rom_effective_size() would send {eff} of {size} bytes, "
                      f"discarding {size - eff} as if it were padding")

    # ---- was this really linked for EWRAM? ------------------------------
    cart_refs = sum(1 for i in range(MB_HEADER, size - 3, 4)
                    if 0x08000000 <= int.from_bytes(data[i:i + 4], "little")
                    <= 0x09FFFFFF)
    if cart_refs:
        warnings.append(f"{cart_refs} word(s) in the body look like cartridge "
                        f"addresses (0x08xxxxxx) -- an EWRAM-linked image "
                        f"should have none, so check rom/multiboot.ld was the "
                        f"script used. This is a warning and not an error "
                        f"because the scan cannot tell an address from an "
                        f"instruction: an `ldmeq`/`stmeq` encodes with the "
                        f"same top byte.")

    # ---- informational ---------------------------------------------------
    payload = size - MB_HEADER
    if payload % 16:
        warnings.append(f"payload is {payload} bytes, not a multiple of 16; "
                        f"mb_send() zero-pads to {(payload + 15) & ~15}")

    entry = targets.get("RAM entry 0xC0")
    crc = binascii.crc32(data[LOGO_START:LOGO_END]) & 0xFFFFFFFF
    summary = (f"{size} bytes, runs at 0x{MB_LOAD_ADDR + entry:08X}, "
               f"logo crc32 0x{crc:08X}") if entry is not None else \
              f"{size} bytes"

    return (errors, warnings, summary)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    failures = 0
    for path in argv[1:]:
        errors, warnings, summary = check(path)

        if errors:
            print(f"FAIL  {path}")
            failures += 1
        else:
            print(f"OK    {path}  ({summary})")
        for e in errors:
            print(f"        ! {e}")
        for w in warnings:
            print(f"        - {w}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
