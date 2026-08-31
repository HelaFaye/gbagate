#!/usr/bin/env python3
"""
bsscheck.py -- catch buffers that will not fit in DRAM, before the linker does.

The ESP32-S3 has about 320 KB of internal DRAM, shared with ESP-IDF, FreeRTOS,
TinyUSB, the WiFi stack and our framebuffer. A single 256 KB static array in a
function overflowed it by 3 KB and stopped the build with a message about
`dram0_0_seg` that says nothing about which array caused it.

Static analysis rather than a link: this runs in a second, names the file and
the line, and works without a toolchain. It cannot see stack or heap use, only
the fixed allocations that land in .bss -- which is the class that fails at
link time and therefore the class worth catching early.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Anything at or above this is almost certainly a mistake in DRAM. The
# framebuffer at 32 KB is the largest legitimate one.
WARN = 16 * 1024
FAIL = 64 * 1024

SIZES = {"uint8_t": 1, "char": 1, "int8_t": 1, "bool": 1,
         "uint16_t": 2, "int16_t": 2,
         "uint32_t": 4, "int32_t": 4, "float": 4, "int": 4,
         "uint64_t": 8, "double": 8}

# Constants worth resolving, so MB_MAX_SIZE reads as 256 KB rather than unknown.
CONSTS = {
    "MB_MAX_SIZE": 256 * 1024,
    "UI_SCREEN_W": 128, "UI_SCREEN_H": 128,
    "SETTINGS_FAV_BYTES": 64,
    "CHIPVIEW_MAX_RECORDS": 512,
    "ROM_MAX_ENTRIES": 32,
}

DECL = re.compile(
    r'^\s*static\s+(?:const\s+)?(\w+)\s+(\w+)\s*\[([^\]]+)\]', re.M)


def dims(expr):
    """Evaluate a simple array extent, or None."""
    e = expr.strip()
    for name, val in CONSTS.items():
        e = re.sub(r'\b%s\b' % name, str(val), e)
    if not re.fullmatch(r'[0-9\s*+/()-]+', e) or not e.strip():
        return None
    try:
        return int(eval(e))            # noqa: S307 - arithmetic only, checked above
    except Exception:
        return None


def main() -> int:
    targets = []
    for sub in ("pio/src", "port/esp32s3"):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".c"):
                targets.append(os.path.join(sub, f))

    worst = 0
    fails = 0
    for rel in targets:
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()

        for m in DECL.finditer(text):
            ctype, name, extent = m.group(1), m.group(2), m.group(3)
            unit = SIZES.get(ctype)
            n = dims(extent)
            if unit is None or n is None:
                continue
            size = unit * n
            worst = max(worst, size)
            if size >= FAIL:
                line = text[:m.start()].count("\n") + 1
                print(f"  FAIL {rel}:{line}  {name}[{extent}] = "
                      f"{size // 1024} KB of DRAM")
                print(f"        allocate it when needed instead; "
                      f"see send_program() in gate_app.c")
                fails += 1
            elif size >= WARN:
                line = text[:m.start()].count("\n") + 1
                print(f"  note {rel}:{line}  {name} = {size // 1024} KB")

    if not fails:
        print(f"  no static buffer over {FAIL // 1024} KB "
              f"(largest is {worst // 1024} KB)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
