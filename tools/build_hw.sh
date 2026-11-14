#!/usr/bin/env bash
#
# build_hw.sh -- build a hardware image with idf.py, no PlatformIO needed.
#
#   . $IDF_PATH/export.sh
#   tools/build_hw.sh gate            devkit, gate image
#   tools/build_hw.sh full
#   BOARD=supermini tools/build_hw.sh mini
#   BOARD=carrier-touch tools/build_hw.sh gate   the touch carrier (doc/CARRIER.md);
#                       the knob carrier is not built yet: its keys have no switch map
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
# Overrides get their own build directory: CMake caches -D values, so a
# BOARD_HAS_* given once would otherwise stick to every later build.
OVR=$(env | sed -n 's/^BOARD_HAS_\([A-Z_]*\)=\(.*\)/\1\2/p' | sort | tr 'A-Z\n' 'a-z-' | sed 's/-$//')
DEFAULT_BUILD="$ROOT/build/hw-$VARIANT-$BOARD${OVR:+-$OVR}"
BUILD="${GBAGATE_BUILD_DIR:-$DEFAULT_BUILD}"

case "$VARIANT" in link|gate|full|mini) ;; *)
    echo "usage: $0 {link|gate|full|mini}" >&2; exit 1 ;; esac
case "$BOARD" in devkit|supermini|carrier-touch) ;; *)
    echo "BOARD must be devkit, supermini or carrier-touch" >&2; exit 1 ;; esac
[ -n "${IDF_PATH:-}" ] || { echo "source \$IDF_PATH/export.sh first" >&2; exit 1; }

PARTS=partitions.csv; FLASH=4MB
if [ "$VARIANT" = mini ] && [ "$BOARD" = devkit ]; then PARTS=partitions-16mb.csv; FLASH=16MB; fi

mkdir -p "$BUILD"
cat > "$BUILD/sdkconfig.hw" <<CFG
CONFIG_ESPTOOLPY_FLASHSIZE_${FLASH}=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="pio/${PARTS}"
CFG

# Per-image overrides, the same files PlatformIO applies (sdkconfig.defaults.<env>).
DEFAULTS="$ROOT/pio/sdkconfig.defaults"
[ -f "$ROOT/pio/sdkconfig.defaults.$VARIANT" ] && DEFAULTS="$DEFAULTS;$ROOT/pio/sdkconfig.defaults.$VARIANT"

EXTRA=""
case "$BOARD" in
    supermini) EXTRA="-D BUTTONS_ON_EXPANDER=1 -D GBAGATE_BOARD=supermini" ;;
    carrier-touch) EXTRA="-D GBAGATE_BOARD=$BOARD" ;;   # its touch keys are the buttons
esac
if [ "${GBAGATE_UART_BRIDGE:-0}" = 1 ]; then
    UART_TX="${GBAGATE_UART_TX_GPIO:-37}"
    UART_RX="${GBAGATE_UART_RX_GPIO:-38}"
    EXTRA="$EXTRA -D GBAGATE_UART_BRIDGE=1"
    EXTRA="$EXTRA -D GBAGATE_UART_TX_GPIO=$UART_TX"
    EXTRA="$EXTRA -D GBAGATE_UART_RX_GPIO=$UART_RX"
fi
# Board overrides from the environment, e.g. BOARD_HAS_BRIDGE=1 tools/build_hw.sh gate
for f in $(env | sed -n 's/^\(BOARD_HAS_[A-Z_]*\)=.*/\1/p'); do EXTRA="$EXTRA -D $f=${!f}"; done

idf.py -B "$BUILD" \
    -D SDKCONFIG="$BUILD/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$DEFAULTS;$BUILD/sdkconfig.hw" \
    -D VARIANT="$VARIANT" -D GBAGATE_QEMU=0 -D IDF_TARGET=esp32s3 $EXTRA \
    build
