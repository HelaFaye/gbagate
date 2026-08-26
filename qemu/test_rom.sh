#!/usr/bin/env bash
#
# test_rom.sh -- the fully automated gate test: a custom ROM in mGBA against the
# firmware gate in QEMU, with a pass/fail verdict and a screenshot of each side.
#
#   ./qemu/test_rom.sh
#
# Topology:
#
#   [ tester.gba in mGBA ] --SIO--> mgba_tester.lua --link_proto--> broker
#        ^                                                             |
#        | SIOMULTI0 (gate answer)                                     v
#        +---------------------------- RESULT <---- [ gate firmware in QEMU ]
#                                                     UART1 :5556   UART2 :5557
#                                                                   (framebuffer)
#
# The ROM drives the handshake and self-checks (rom/tester.c + rom/handshake.h),
# so unlike the retail path this does not depend on a game's SIOCNT timing.
#
# Every heavyweight tool is optional: if ESP-IDF, Espressif's QEMU, the GBA
# toolchain or a scriptable mGBA is missing, the step that needs it is SKIPPED
# with an install hint rather than failing -- the same philosophy as the
# `qemu-proto` target. The tool-free logic checks live in test_rom_selftest.py
# and always run first.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu-gate"
LINK_PORT="${LINK_PORT:-5556}"
FB_PORT="${FB_PORT:-5557}"
MGBA_PORT="${MGBA_PORT:-7000}"
OUT="${OUT:-$ROOT/build/rom-test}"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mskip:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mFAIL:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$OUT"

PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---- step 0: the tool-free logic checks always run ----------------------

say "logic self-test (no emulator, no toolchain)"
if ! python3 "$ROOT/qemu/test_rom_selftest.py"; then
    die "the ROM/display logic self-test failed; fix that before the emulator"
fi

# ---- prerequisites for the live run -------------------------------------

MISSING=0
need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        warn "$1 not found -- $2"
        MISSING=1
    fi
}

MGBA=""
for cand in mgba-headless mgba-sdl mgba-qt mgba; do
    if command -v "$cand" >/dev/null 2>&1; then MGBA="$cand"; break; fi
done

need qemu-system-xtensa "run ./qemu/setup.sh (needs ESP-IDF 5.x + Espressif QEMU)"
need arm-none-eabi-gcc  "install a GBA/ARM toolchain (e.g. gcc-arm-none-eabi) to build rom/tester.gba"
[ -n "$MGBA" ] || { warn "no mgba binary -- see tools/build-mgba-termux.sh for a scriptable build"; MISSING=1; }
[ -n "${IDF_PATH:-}" ] || { warn "IDF_PATH unset -- source \$IDF_PATH/export.sh to build the gate image"; MISSING=1; }

if [ "$MISSING" = 1 ]; then
    echo
    say "logic checks passed; the live emulation run is SKIPPED (missing tools above)."
    say "once the tools are present, re-run:  make qemu-rom"
    exit 0
fi

# ---- step 1: build the ROM ----------------------------------------------

say "building the tester ROM"
make -C "$ROOT/rom" >/dev/null || die "ROM build failed"
ROM="$ROOT/rom/tester.gba"
[ -f "$ROM" ] || die "no $ROM after build"

# ---- step 2: build the gate image ---------------------------------------

if [ ! -f "$BUILD/flash.bin" ]; then
    say "building the gate flash image"
    "$ROOT/qemu/build.sh" gate || die "gate build failed"
fi

# ---- step 3: launch QEMU (headless), the broker, and mGBA ---------------

say "starting QEMU (gate) headless"
FB_PORT="$FB_PORT" LINK_PORT="$LINK_PORT" \
    "$ROOT/qemu/run.sh" gate --headless >/dev/null 2>&1 &
PIDS+=("$!")

say "starting the broker (relay to mGBA on :$MGBA_PORT)"
python3 "$ROOT/qemu/broker.py" --qemu "127.0.0.1:$LINK_PORT" \
    --mgba "$MGBA_PORT" >"$OUT/broker.log" 2>&1 &
PIDS+=("$!")

sleep 2   # let QEMU boot and the broker bind

say "running the tester ROM in $MGBA"
rm -f "$OUT/rom_test.result" "$OUT/rom_test.png"
LINK_PORT="$MGBA_PORT" \
ROMTEST_SHOT="$OUT/rom_test.png" \
ROMTEST_RESULT="$OUT/rom_test.result" \
    "$MGBA" --script "$ROOT/qemu/mgba_tester.lua" "$ROM" \
    >"$OUT/mgba.log" 2>&1 &
MGBA_PID="$!"
PIDS+=("$MGBA_PID")

# ---- step 4: grab the gate's screen -------------------------------------

say "capturing the gate screen from QEMU (UART2 :$FB_PORT)"
python3 "$ROOT/qemu/fbgrab.py" --host 127.0.0.1 --port "$FB_PORT" \
    --one --out "$OUT/gate_screen.ppm" --timeout 20 \
    >"$OUT/fbgrab.log" 2>&1 || warn "framebuffer capture did not complete (see $OUT/fbgrab.log)"

# ---- step 5: wait for the verdict ---------------------------------------

say "waiting for the ROM's verdict"
DEADLINE=$(( $(date +%s) + 40 ))
VERDICT=""
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    if [ -f "$OUT/rom_test.result" ]; then
        VERDICT="$(head -1 "$OUT/rom_test.result" 2>/dev/null)"
        break
    fi
    if grep -q "ROM-TEST:" "$OUT/mgba.log" 2>/dev/null; then
        VERDICT="$(grep -o 'ROM-TEST: [A-Z]*' "$OUT/mgba.log" | head -1 | awk '{print $2}')"
        break
    fi
    kill -0 "$MGBA_PID" 2>/dev/null || break
    sleep 1
done

echo
say "artifacts in $OUT:"
ls -1 "$OUT" | sed 's/^/    /'
echo

if [ "$VERDICT" = "PASS" ]; then
    say "ROM test: PASS"
    exit 0
fi

echo "----- mgba.log (tail) -----"; tail -20 "$OUT/mgba.log" 2>/dev/null
echo "----- broker.log (tail) -----"; tail -20 "$OUT/broker.log" 2>/dev/null
die "ROM test: ${VERDICT:-NO VERDICT} (see logs in $OUT)"
