#!/usr/bin/env bash
#
# run.sh -- run a built image under QEMU.
#
#   ./qemu/run.sh gate              console on this terminal
#   ./qemu/run.sh gate --debug      halt at reset, gdb on :3333
#   ./qemu/run.sh gate --headless   console to a log file, for scripting
#
# UART0 is the ESP-IDF console. UART1 carries link_proto and is exposed on
# TCP :5556 for qemu/broker.py -- they are kept separate because sharing one
# would interleave log lines with SLIP frames and corrupt both.

set -eu

VARIANT="${1:-gate}"; shift || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu-$VARIANT"
FLASH="$BUILD/flash.bin"
LINK_PORT="${LINK_PORT:-5556}"
GDB_PORT="${GDB_PORT:-3333}"

DEBUG=0
HEADLESS=0
for arg in "$@"; do
    case "$arg" in
        --debug)    DEBUG=1 ;;
        --headless) HEADLESS=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

[ -f "$FLASH" ] || {
    echo "no flash image at $FLASH" >&2
    echo "build it first:  ./qemu/build.sh $VARIANT" >&2
    exit 1
}

command -v qemu-system-xtensa >/dev/null 2>&1 || {
    echo "qemu-system-xtensa not found. Run ./qemu/setup.sh" >&2
    exit 1
}

ARGS=(
    -machine esp32s3
    -m 8M
    -drive "file=$FLASH,if=mtd,format=raw"
    # UART1 -> TCP, waiting for the broker rather than requiring it up front.
    -serial "mon:stdio"
    -serial "tcp::$LINK_PORT,server,nowait"
    -nographic
)

if [ "$HEADLESS" = 1 ]; then
    ARGS=( -machine esp32s3 -m 8M
           -drive "file=$FLASH,if=mtd,format=raw"
           -serial "file:$BUILD/console.log"
           -serial "tcp::$LINK_PORT,server,nowait"
           -nographic -no-reboot )
    echo "console -> $BUILD/console.log"
fi

if [ "$DEBUG" = 1 ]; then
    ARGS+=( -gdb "tcp::$GDB_PORT" -S )
    echo "halted at reset. attach with:"
    echo "  xtensa-esp32s3-elf-gdb $BUILD/gbagate.elf -ex 'target remote :$GDB_PORT'"
fi

echo "link_proto on tcp://127.0.0.1:$LINK_PORT   (python3 qemu/broker.py --loop)"
echo "ctrl-a x to quit"
echo

exec qemu-system-xtensa "${ARGS[@]}"
