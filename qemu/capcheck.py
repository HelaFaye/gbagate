#!/usr/bin/env python3
"""
capcheck.py -- every capability the menu requires must be set by something.

ui_shell.c gates each mode on HW_CAP_* bits. app_main.c is the only thing that
sets them. If a bit is required but never set, the mode silently vanishes from
the menu -- no error, no log, just a device that appears to have fewer features
than it was built with.

That happened: HW_CAP_LINK_PORT gated five of the eight modes and nothing set
it, so the firmware booted to a two-entry menu. No host suite could see it,
because app_main.c is ESP-IDF-only, and the shell's own tests pass an explicit
caps mask rather than the one the firmware produces.

This compares the two sets of names directly.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def caps_in(path, pattern=r'\b(HW_CAP_[A-Z_]+)\b'):
    with open(os.path.join(ROOT, path), encoding="utf-8") as fh:
        return set(re.findall(pattern, fh.read()))


# Features that are modes in their own right. Each must appear in the shell's
# table, or the feature is compiled in and unreachable -- which is how the
# infrared codec sat unused after being written and tested.
MODE_FEATURES = {
    "FEAT_CHIPGATE":    "APP_MODE_CHIP_GATE",
    "FEAT_MULTIBOOT":   "APP_MODE_MULTIBOOT",
    "FEAT_CHIP_READER": "APP_MODE_CHIP_SCAN",
    "FEAT_USB_EXPOSE":  "APP_MODE_USB_STORAGE",
    "FEAT_SELFTEST":    "APP_MODE_SELFTEST",
    "FEAT_IR":          "APP_MODE_INFRARED",
}


def check_mode_features(root) -> int:
    with open(os.path.join(root, "src", "variant.c"), encoding="utf-8") as fh:
        variant = fh.read()
    with open(os.path.join(root, "src", "ui_shell.c"), encoding="utf-8") as fh:
        shell = fh.read()

    fails = 0
    for feat, mode in sorted(MODE_FEATURES.items()):
        if feat not in variant:
            continue                     # not a feature in this build at all
        if mode not in shell:
            print(f"  FAIL {feat} is a feature with no menu entry ({mode})")
            fails += 1
    if not fails:
        print(f"  all {len(MODE_FEATURES)} mode-features have a menu entry")
    return fails


def main() -> int:
    required = caps_in("src/ui_shell.c")
    # The enum itself lists them all; the menu is what makes them load-bearing.
    known = caps_in("src/hw_caps.h")
    settable = caps_in("pio/src/app_main.c")

    required &= known
    unset = sorted(required - settable)

    print("required by the menu:", ", ".join(sorted(required)) or "(none)")
    print("set by app_main:     ", ", ".join(sorted(settable & known)) or "(none)")

    if unset:
        print()
        for cap in unset:
            print(f"  FAIL {cap} gates a mode but nothing sets it")
            for line in open(os.path.join(ROOT, "src/ui_shell.c"),
                             encoding="utf-8"):
                if cap in line and "case" not in line:
                    print(f"        {line.strip()[:70]}")
        return 1

    print("every capability the menu needs is set somewhere")
    return check_mode_features(ROOT)


if __name__ == "__main__":
    sys.exit(main())
