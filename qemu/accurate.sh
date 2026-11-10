#!/usr/bin/env bash
#
# accurate.sh -- the hardware image, not the emulator image, on the more
# accurate ESP32-S3 emulator: Espressif's QEMU with the patch series in
# HelaFaye/qemu_esp32 (patches/espressif-qemu: GPIO pins as wires, the GPIO
# matrix and IO_MUX, GP-SPI2/3 with GDMA, esp32s3.pin-link).
#
#   ./qemu/accurate.sh build              fetch the fork at $FORK_REV, build QEMU
#   ./qemu/accurate.sh boot [variant] [board]
#                       build the hardware image (default gate, carrier-touch),
#                       boot it with the board's parts wired on, check what the
#                       board's own drivers do with them (qemu/accurate_boot.py)
#
# Where ./qemu/build.sh makes an image with the PHY, panel and USB swapped for
# UART stand-ins (GBAGATE_QEMU=1), this boots what goes on the board
# (tools/build_hw.sh, GBAGATE_QEMU=0): the panel driver over GP-SPI2, the
# touch controller, the SD card, and the link PHYs on real GPIO; the picture
# is read back from the emulated glass and the touch is pressed through the
# emulated XPT2046. accurate_boot.py says what each board checks. USB fails,
# as it must: QEMU has no USB-OTG for the S3.
#
# The fork is a work in progress. FORK_REV pins the commit last checked here;
# move it when the fork moves, and say what changed in doc/MMO.md.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORK_URL="${FORK_URL:-https://github.com/HelaFaye/qemu_esp32}"
FORK_REV="${FORK_REV:-21d8244667a0242d6ab10e80148a9c510c4ff747}"   # branch claude/determined-cray-ubv8bw
FORK="$ROOT/build/qemu_esp32"
QDIR="$ROOT/build/esp-qemu"
QEMU="$QDIR/build/qemu-system-xtensa"
SECONDS_UP="${SECONDS_UP:-20}"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

build() {
    if [ ! -d "$FORK/.git" ]; then
        git init -q "$FORK"
        git -C "$FORK" remote add origin "$FORK_URL"
    fi
    say "qemu_esp32 at $FORK_REV"
    git -C "$FORK" fetch -q --depth 1 origin "$FORK_REV"
    git -C "$FORK" checkout -q --detach FETCH_HEAD
    # build.sh configures only when there is no build.ninja, so a build from
    # an earlier commit would keep that commit's configure options (it was
    # how screendump went missing: --enable-pixman came later). A new
    # commit gets a fresh build directory.
    if [ "$(cat "$QDIR/.fork-rev" 2>/dev/null)" != "$FORK_REV" ]; then
        rm -rf "$QDIR/build"
    fi
    say "Espressif QEMU with its patches, in $QDIR"
    sh "$FORK/patches/espressif-qemu/build.sh" "$QDIR"
    echo "$FORK_REV" > "$QDIR/.fork-rev"
    "$QEMU" -machine help | grep -q esp32s3 || die "no esp32s3 machine in $QEMU"
    say "built: $QEMU"
}

boot() {
    local variant="${1:-gate}" board="${2:-carrier-touch}" dir
    [ -x "$QEMU" ] || die "no $QEMU. Run: $0 build"
    [ -n "${IDF_PATH:-}" ] || die "source \$IDF_PATH/export.sh first"
    case "$board" in devkit|carrier-touch) ;; *) die "boards: devkit, carrier-touch" ;; esac
    if [ "$board" = carrier-touch ]; then
        command -v mformat >/dev/null || die "the SD card image needs mtools (apt install mtools)"
    fi
    dir="$ROOT/build/hw-$variant-$board"
    say "hardware image: $variant, $board"
    BOARD="$board" "$ROOT/tools/build_hw.sh" "$variant" >/dev/null
    # merge_bin and --fill-flash-size: esptool 4 (ESP-IDF 5.5) knows only these, and 5 still takes them
    (cd "$dir" && python -m esptool --chip esp32s3 merge_bin --fill-flash-size 4MB \
        -o "$dir/flash-qemu.bin" @flash_args >/dev/null)
    python3 -I "$ROOT/qemu/accurate_boot.py" "$QEMU" "$dir/flash-qemu.bin" "$board" "$variant" \
        "$dir/accurate"
}

case "${1:-}" in
    build) build ;;
    boot)  shift; boot "$@" ;;
    *)     echo "usage: $0 {build|boot [link|gate|full|mini] [carrier-touch|devkit]}" >&2; exit 1 ;;
esac
