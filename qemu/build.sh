#!/usr/bin/env bash
#
# build.sh -- build one firmware image for QEMU and assemble a flash image.
#
#   ./qemu/build.sh gate
#   ./qemu/build.sh gate --aux-uart 0    for a QEMU that only wires two UARTs
#
# QEMU boots from a flash image, not from an application binary. That is the
# single most common mistake here: `-kernel firmware.bin` is a Linux option and
# does nothing useful on an ESP32 machine. The image has to contain the
# bootloader at 0x0, the partition table at 0x8000 and the app at 0x10000, and
# esptool's merge_bin is what builds it.

set -eu

VARIANT="${1:-gate}"
if [ $# -gt 0 ]; then shift; fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu-$VARIANT"

# Checked before the options are parsed so that omitting the variant entirely
# ("build.sh --aux-uart 0") reports the missing variant, rather than the first
# option being mistaken for one and the *next* word blamed as unknown.
case "$VARIANT" in
    link|gate|full) ;;
    *) echo "usage: $0 {link|gate|full} [--aux-uart 0|1|2]" >&2; exit 1 ;;
esac

# Which UART st7735_qemu.c streams the framebuffer out of, and input_qemu.c
# reads keys back on. See the long note in the top-level CMakeLists.txt; the
# short version is that 2 is correct but needs a QEMU that actually wires three
# UARTs to the esp32s3 machine, and ./qemu/uart2check.sh says whether this one
# does. The value has to match the -serial ordering run.sh/lab.sh use, so pass
# the same --aux-uart to both.
AUX_UART="${AUX_UART:-2}"

while [ $# -gt 0 ]; do
    case "$1" in
        --aux-uart)
            [ $# -ge 2 ] || { echo "--aux-uart needs a value (0, 1 or 2)" >&2; exit 1; }
            AUX_UART="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

case "$AUX_UART" in
    0|1|2) ;;
    *) echo "--aux-uart must be 0, 1 or 2 (got '$AUX_UART')" >&2; exit 1 ;;
esac

[ -n "${IDF_PATH:-}" ] || { echo "source \$IDF_PATH/export.sh first" >&2; exit 1; }

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

say "building the $VARIANT image (framebuffer/input on UART$AUX_UART)"

# QEMU-specific configuration on top of the normal defaults.
SDKCONFIG="$BUILD/sdkconfig.qemu"
mkdir -p "$BUILD"
cat > "$SDKCONFIG" <<'CFG'
# QEMU has no USB-OTG on the S3, so CDC and MSC cannot work there. The UART
# transport stands in for them; see port/esp32s3/transport_uart.c.
CONFIG_TINYUSB_CDC_ENABLED=n
CONFIG_TINYUSB_MSC_ENABLED=n

# QEMU's S3 machine does not model PSRAM. The firmware only uses it for
# staging buffers, so the emulated build does without.
CONFIG_SPIRAM=n

# Flash suspend is a real-silicon feature; leaving it on under QEMU only
# produces noise.
CONFIG_SPI_FLASH_AUTO_SUSPEND=n

CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_FREERTOS_UNICORE=n

# platformio.ini pins a 4 MB flash with a custom partition table (nvs / phy /
# 2M factory / 1500K fat). The native idf.py path has no board file to inherit
# that from, so select it explicitly; otherwise idf.py falls back to the 2 MB
# single-factory default and the app overflows / mismatches merge_bin below.
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="pio/partitions.csv"
CFG

# QEMU_AUX_UART is passed on every build, never only when --aux-uart is given.
# CMake would otherwise cache it in $BUILD: building once with --aux-uart 0 and
# then plainly would silently keep 0, producing an image whose UART does not
# match the -serial layout run.sh sets up -- which looks exactly like the blank
# screen this option exists to fix. Always passing it makes the default win.
idf.py -B "$BUILD" \
       -D SDKCONFIG_DEFAULTS="$ROOT/pio/sdkconfig.defaults;$SDKCONFIG" \
       -D VARIANT="$VARIANT" \
       -D IDF_TARGET=esp32s3 \
       -D QEMU_AUX_UART="$AUX_UART" \
       build

say "assembling the flash image"

APP="$BUILD/gbagate.bin"
[ -f "$APP" ] || APP="$(find "$BUILD" -maxdepth 1 -name '*.bin' \
                        ! -name 'bootloader*' ! -name 'partition*' | head -1)"

# IDF 6.0 ships esptool v5, where the console script is `esptool` (no `.py`).
# Fall back through the forms so this works on both 4.x and 5.x toolchains.
if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="esptool.py"
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL="esptool"
else
    ESPTOOL="python -m esptool"
fi

# esptool v4 uses snake_case (merge_bin, --flash_mode ...); v5 renamed these to
# kebab-case (merge-bin, --flash-mode ...). Try the legacy spelling first, then
# fall back to the modern one so either toolchain produces the image.
if ! $ESPTOOL --chip esp32s3 merge_bin \
        --fill-flash-size 4MB \
        --flash_mode dio --flash_freq 80m --flash_size 4MB \
        -o "$BUILD/flash.bin" \
        0x0     "$BUILD/bootloader/bootloader.bin" \
        0x8000  "$BUILD/partition_table/partition-table.bin" \
        0x10000 "$APP" 2>/dev/null; then
    say "legacy esptool syntax failed; retrying with esptool v5 (kebab-case)"
    $ESPTOOL --chip esp32s3 merge-bin \
        --fill-flash-size 4MB \
        --flash-mode dio --flash-freq 80m --flash-size 4MB \
        -o "$BUILD/flash.bin" \
        0x0     "$BUILD/bootloader/bootloader.bin" \
        0x8000  "$BUILD/partition_table/partition-table.bin" \
        0x10000 "$APP"
fi

say "flash image: $BUILD/flash.bin ($(du -h "$BUILD/flash.bin" | cut -f1))"
if [ "$AUX_UART" = 2 ]; then
    say "run it:  ./qemu/run.sh $VARIANT"
else
    # The -serial ordering has to match what this image was compiled for, so
    # hand back the exact command rather than the default one.
    say "run it:  ./qemu/run.sh $VARIANT --aux-uart $AUX_UART"
    say "     or: ./qemu/lab.sh --aux-uart $AUX_UART"
fi
