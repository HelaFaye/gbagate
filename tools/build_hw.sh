#!/usr/bin/env bash
#
# build_hw.sh -- build a hardware image with idf.py, no PlatformIO needed.
#
#   . $IDF_PATH/export.sh
#   tools/build_hw.sh gate            devkit, gate image
#   tools/build_hw.sh full
#   BOARD=supermini tools/build_hw.sh mini
#
# platformio.ini remains the reference for flashing; this exists because
# PlatformIO fetches its toolchain from a registry, and a plain ESP-IDF checkout
# is all a CI machine (or a sandbox with GitHub but no registry) can be sure of.
# It compiles exactly the sources the PlatformIO envs compile: the component
# CMakeLists chooses them from VARIANT and GBAGATE_QEMU=0.
set -eu

VARIANT="${1:-gate}"
BOARD="${BOARD:-devkit}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/hw-$VARIANT-$BOARD"

case "$VARIANT" in link|gate|full|mini) ;; *)
    echo "usage: $0 {link|gate|full|mini}" >&2; exit 1 ;; esac
case "$BOARD" in devkit|supermini) ;; *)
    echo "BOARD must be devkit or supermini" >&2; exit 1 ;; esac
[ -n "${IDF_PATH:-}" ] || { echo "source \$IDF_PATH/export.sh first" >&2; exit 1; }

PARTS=partitions.csv; FLASH=4MB
if [ "$VARIANT" = mini ] && [ "$BOARD" = devkit ]; then PARTS=partitions-16mb.csv; FLASH=16MB; fi

mkdir -p "$BUILD"
cat > "$BUILD/sdkconfig.hw" <<CFG
CONFIG_ESPTOOLPY_FLASHSIZE_${FLASH}=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="pio/${PARTS}"
CFG

EXTRA=""
[ "$BOARD" = supermini ] && EXTRA="-D BUTTONS_ON_EXPANDER=1 -D GBAGATE_BOARD=supermini"

idf.py -B "$BUILD" \
    -D SDKCONFIG="$BUILD/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$ROOT/pio/sdkconfig.defaults;$BUILD/sdkconfig.hw" \
    -D VARIANT="$VARIANT" -D GBAGATE_QEMU=0 -D IDF_TARGET=esp32s3 $EXTRA \
    build
