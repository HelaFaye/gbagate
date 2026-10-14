#!/usr/bin/env python3
"""
srccheck.py -- every source file reaches a build, or is deliberately excluded.

Three separate builds failed because a file was added and the CMake list was
not updated. The linker reports the missing *symbols*, not the missing file, so
each time it read as a code fault rather than a build-config one.

src/ is globbed now, so that half cannot recur. port/esp32s3/ is still an
explicit list -- correctly, because those files need hardware that may be
absent and compiling them everywhere would be wrong. This checks that every
file there is either named in the CMakeLists or listed below as knowingly
unused.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE = os.path.join(ROOT, "pio", "src", "CMakeLists.txt")

# Files that exist and are deliberately not built, with the reason.
EXCLUDED = {
    # Both provide input_active(); the CMakeLists picks one by board. Naming
    # them here as well would hide a genuine omission, so instead both appear
    # in the CMakeLists and this entry is empty of them.
    "cg_phy_esp32s3.c": "superseded by gba_link_esp32s3.c; kept to diff early "
                        "assumptions",
}


def main() -> int:
    with open(CMAKE, encoding="utf-8") as fh:
        cmake = fh.read()

    named = set(re.findall(r'([a-z0-9_]+\.c)', cmake))

    port = os.path.join(ROOT, "port", "esp32s3")
    missing = []
    for f in sorted(os.listdir(port)):
        if not f.endswith(".c"):
            continue
        if f in EXCLUDED or f in named:
            continue
        missing.append(f)

    # And confirm the glob is actually in place for src/, since removing it
    # would silently restore the old failure mode.
    if "file(GLOB PORTABLE" not in cmake:
        print("  FAIL src/ is no longer globbed; every new file there must now "
              "be listed by hand")
        return 1

    if missing:
        for f in missing:
            print(f"  FAIL port/esp32s3/{f} is in no build and not listed as "
                  f"excluded")
        print("        add it to pio/src/CMakeLists.txt, or to EXCLUDED here "
              "with a reason")
        return 1

    n_src = len([f for f in os.listdir(os.path.join(ROOT, "src"))
                 if f.endswith(".c")])
    print(f"  src/ globbed ({n_src} files); every port/esp32s3 source is "
          f"built or excluded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
