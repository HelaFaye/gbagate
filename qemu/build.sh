#!/usr/bin/env bash
#
# build.sh -- build one firmware image for QEMU and assemble a flash image.
#
#   ./qemu/build.sh gate
#
# QEMU boots from a flash image, not from an application binary. That is the
# single most common mistake here: `-kernel firmware.bin` is a Linux option and
# does nothing useful on an ESP32 machine. The image has to contain the
# bootloader at 0x0, the partition table at 0x8000 and the app at 0x10000, and
# esptool's merge_bin is what builds it.

set -eu

VARIANT="${1:-gate}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu-$VARIANT"

case "$VARIANT" in
    link|gate|full) ;;
    *) echo "usage: $0 {link|gate|full}" >&2; exit 1 ;;
esac

[ -n "${IDF_PATH:-}" ] || { echo "source \$IDF_PATH/export.sh first" >&2; exit 1; }

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

say "building the $VARIANT image"

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

idf.py -B "$BUILD" \
       -D SDKCONFIG_DEFAULTS="$ROOT/pio/sdkconfig.defaults;$SDKCONFIG" \
       -D VARIANT="$VARIANT" \
       -D IDF_TARGET=esp32s3 \
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
say "run it:  ./qemu/run.sh $VARIANT"
