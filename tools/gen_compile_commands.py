#!/usr/bin/env python3
"""
gen_compile_commands.py -- a compile database for editors.

The suites build and the firmware links, but an editor knows none of that: it
sees `#include "chipgate.h"` and has no reason to look in src/. The result is a
file full of red squiggles that say "unknown type uint16_t", which is what a
missing include path looks like once the first include fails and everything
after it cascades.

This writes compile_commands.json covering all four kinds of source in the
tree, each with the flags it is actually compiled with:

    src/          portable, -Isrc
    test/         host suites, -Isrc
    pio/src/      firmware, plus the ESP-IDF stubs and the variant defines
    port/esp32s3/ hardware glue, same

The ESP-IDF entries point at qemu/stubs rather than a real IDF checkout on
purpose. The stubs are what `make lint` compiles against, so the editor and the
lint agree; pointing at $IDF_PATH would make the editor depend on an
environment variable that is only set inside an export.sh shell, which is
exactly the reason the CMake IntelliSense warning appears.

Run it after adding a file, or via `make compile-commands`.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BASE = ["cc", "-std=c11", "-Wall", "-Wextra", "-Wshadow"]

HOST_INC = ["-I" + os.path.join(ROOT, "src")]

IDF_INC = [
    "-I" + os.path.join(ROOT, "qemu", "stubs"),
    "-I" + os.path.join(ROOT, "src"),
    "-I" + os.path.join(ROOT, "port", "esp32s3"),
    "-I" + os.path.join(ROOT, "pio", "src"),
]

# The firmware only type-checks with these set; without them the editor sees
# every #if as false and greys out most of gate_app.c.
IDF_DEF = [
    "-DVARIANT=VARIANT_GATE",
    '-DGBAGATE_VARIANT_NAME="gate"',
    "-DGBAGATE_HAS_CHIPDB=1",
    "-DGBAGATE_QEMU=1",
]


def entries():
    out = []

    def add(path, args):
        out.append({
            "directory": ROOT,
            "file": path,
            "arguments": args + [path],
        })

    for sub, args in (("src", BASE + HOST_INC),
                      ("test", BASE + HOST_INC),
                      ("rom", BASE + ["-I" + os.path.join(ROOT, "rom")]),
                      ("pio/src", BASE + IDF_INC + IDF_DEF),
                      ("port/esp32s3", BASE + IDF_INC + IDF_DEF)):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".c"):
                add(os.path.join(d, f), args)
    return out


def main() -> int:
    db = entries()
    path = os.path.join(ROOT, "compile_commands.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(db, fh, indent=2)
        fh.write("\n")
    print(f"  compile_commands.json: {len(db)} files")

    # A fallback for anything not in the database -- headers opened on their
    # own, mostly, which clangd otherwise parses with no include path at all.
    clangd = os.path.join(ROOT, ".clangd")
    with open(clangd, "w", encoding="utf-8") as fh:
        fh.write(
            "# Fallback for files not in compile_commands.json -- headers\n"
            "# opened on their own, mainly. Regenerate the database with\n"
            "#   make compile-commands\n"
            "CompileFlags:\n"
            "  Add:\n"
            "    - -std=c11\n"
            "    - -Isrc\n"
            "    - -Iqemu/stubs\n"
            "    - -Iport/esp32s3\n"
            "    - -Ipio/src\n"
            "    - -Irom\n"
            "    - -DGBAGATE_QEMU=1\n"
            "    - -DVARIANT=VARIANT_GATE\n"
            '    - -DGBAGATE_VARIANT_NAME="gate"\n'
            "    - -DGBAGATE_HAS_CHIPDB=1\n"
            "Diagnostics:\n"
            "  # The ESP-IDF stubs deliberately do not define everything the\n"
            "  # real headers do; unused-parameter noise from them is not\n"
            "  # worth reading past.\n"
            "  UnusedIncludes: None\n"
        )
    print("  .clangd: fallback flags written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
