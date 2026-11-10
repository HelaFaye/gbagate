#!/usr/bin/env bash
#
# accurate.sh -- the hardware image, not the emulator image, on the more
# accurate ESP32-S3 emulator: Espressif's QEMU with the patch series in
# HelaFaye/qemu_esp32 (patches/espressif-qemu: GPIO pins as wires, the GPIO
# matrix and IO_MUX, GP-SPI2/3 with GDMA, esp32s3.pin-link).
#
#   ./qemu/accurate.sh build              fetch the fork at $FORK_REV, build QEMU
#   ./qemu/accurate.sh boot [variant]     build the devkit image (default gate),
#                                         boot it, check the board's own drivers
#
# Where ./qemu/build.sh makes an image with the PHY, panel and USB swapped for
# UART stand-ins (GBAGATE_QEMU=1), this boots what goes on the board
# (tools/build_hw.sh, GBAGATE_QEMU=0): the ST7735 driver over GP-SPI2 and the
# link PHYs on real GPIO. Checked: the panel driver comes up, the Multi-Play
# and Wireless Adapter PHYs start on core 1, the link source is the PHY, every
# task starts, nothing panics or resets. USB fails, as it must: QEMU has no
# USB-OTG for the S3.
#
# The fork is a work in progress. FORK_REV pins the commit last checked here;
# move it when the fork moves, and say what changed in doc/MMO.md.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORK_URL="${FORK_URL:-https://github.com/HelaFaye/qemu_esp32}"
FORK_REV="${FORK_REV:-1632bd052d8e8f63c3710bf14ef42e352f716554}"   # branch claude/determined-cray-ubv8bw
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
    say "Espressif QEMU with its patches, in $QDIR"
    sh "$FORK/patches/espressif-qemu/build.sh" "$QDIR"
    "$QEMU" -machine help | grep -q esp32s3 || die "no esp32s3 machine in $QEMU"
    say "built: $QEMU"
}

boot() {
    local variant="${1:-gate}" dir log
    [ -x "$QEMU" ] || die "no $QEMU. Run: $0 build"
    [ -n "${IDF_PATH:-}" ] || die "source \$IDF_PATH/export.sh first"
    dir="$ROOT/build/hw-$variant-devkit"
    say "hardware image: $variant, devkit"
    "$ROOT/tools/build_hw.sh" "$variant" >/dev/null
    (cd "$dir" && python -m esptool --chip esp32s3 merge-bin --pad-to-size 4MB \
        -o "$dir/flash-qemu.bin" @flash_args >/dev/null)
    log="$dir/accurate-console.log"
    say "booting for ${SECONDS_UP}s; console in $log"
    timeout "$SECONDS_UP" "$QEMU" -machine esp32s3 -m 8M \
        -drive "file=$dir/flash-qemu.bin,if=mtd,format=raw" \
        -serial "file:$log" -nographic -no-reboot -monitor none >"$dir/accurate-qemu.log" 2>&1 || true

    local fails=0
    want() {
        if grep -qE "$2" "$log"; then printf '  ok    %s\n' "$1"
        else printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); fi
    }
    refuse() {
        if grep -qE "$2" "$log"; then printf '  FAIL  %s\n' "$1"; fails=$((fails + 1))
        else printf '  ok    %s\n' "$1"; fi
    }
    want   "app_main runs"                             "Calling app_main\\(\\)"
    want   "the ST7735 driver comes up over GP-SPI2"   "st7735: 128x128"
    want   "the Multi-Play PHY starts on core 1"       "mp_phy: Multi-Play PHY on core 1"
    want   "the Wireless Adapter PHY starts on core 1" "rfu_phy: Wireless Adapter PHY on core 1"
    want   "the link source is the PHY"                "link source: phy"
    want   "every task starts"                         "tasks started for the $variant image"
    refuse "no panic, abort or watchdog"               "Guru Meditation|abort\\(\\)|panic|Task watchdog|Backtrace"
    refuse "no reset after boot"                       "rst:0x[0-9a-f]+ \\((SW|TG|RTCWDT|INT|PANIC)"
    [ "$fails" = 0 ] || { echo "  QEMU said:"; tail -5 "$dir/accurate-qemu.log" | sed 's/^/    /'; }
    [ "$fails" = 0 ] && say "hardware image on qemu_esp32: all checks passed" \
                     || die "hardware image on qemu_esp32: $fails check(s) failed"
}

case "${1:-}" in
    build) build ;;
    boot)  shift; boot "$@" ;;
    *)     echo "usage: $0 {build|boot [link|gate|full|mini]}" >&2; exit 1 ;;
esac
