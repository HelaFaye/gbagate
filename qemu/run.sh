#!/usr/bin/env bash
#
# run.sh -- run a built image under QEMU.
#
#   ./qemu/run.sh gate              console on this terminal
#   ./qemu/run.sh gate --debug      halt at reset, gdb on :3333
#   ./qemu/run.sh gate --headless   console to a log file, for scripting
#   ./qemu/run.sh gate --aux-uart 0 for a QEMU that only wires two UARTs
#
# UART0 is the ESP-IDF console. UART1 carries link_proto and is exposed on
# TCP :5556 for qemu/broker.py -- they are kept separate because sharing one
# would interleave log lines with SLIP frames and corrupt both. UART2 carries
# the framebuffer stream (port/esp32s3/st7735_qemu.c) on TCP :5557, which
# qemu/fbgrab.py turns into a PPM -- QEMU cannot drive the real ST7735, so this
# is how the firmware's screen is seen under emulation.
#
# QEMU connects each UART peripheral to a -serial chardev in order, so the
# -serial options below map to UART0, UART1 and UART2 in that order -- but only
# on a QEMU whose esp32s3 machine attaches three UARTs. One that attaches two
# accepts the third -serial, opens its socket, and silently discards everything
# the firmware writes there. Run ./qemu/uart2check.sh to find out which you
# have; if it is two, rebuild with -D QEMU_AUX_UART=0 and pass --aux-uart 0
# here so the layout matches. The value must agree with the image: it is
# printed at boot ("framebuffer tap on UARTn") precisely so a mismatch shows.

set -eu

VARIANT="${1:-gate}"; shift || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu-$VARIANT"
FLASH="$BUILD/flash.bin"
LINK_PORT="${LINK_PORT:-5556}"
FB_PORT="${FB_PORT:-5557}"
GDB_PORT="${GDB_PORT:-3333}"
AUX_UART="${AUX_UART:-2}"

DEBUG=0
HEADLESS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --debug)    DEBUG=1; shift ;;
        --headless) HEADLESS=1; shift ;;
        --aux-uart) AUX_UART="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

case "$AUX_UART" in
    0|1|2) ;;
    *) echo "--aux-uart must be 0, 1 or 2 (got '$AUX_UART')" >&2; exit 1 ;;
esac

[ -f "$FLASH" ] || {
    echo "no flash image at $FLASH" >&2
    echo "build it first:  ./qemu/build.sh $VARIANT" >&2
    exit 1
}

command -v qemu-system-xtensa >/dev/null 2>&1 || {
    echo "qemu-system-xtensa not found. Run ./qemu/setup.sh" >&2
    exit 1
}

if [ "$HEADLESS" = 1 ]; then
    CONSOLE="file:$BUILD/console.log"
else
    CONSOLE="mon:stdio"
fi

# The -serial order *is* the UART numbering, so each layout is just a different
# ordering of the same chardevs. Both waiting for a client rather than requiring
# one up front, so QEMU boots whether or not anything has attached yet.
case "$AUX_UART" in
    2)  # UART0 console, UART1 link, UART2 framebuffer. Nothing shared.
        SERIALS=( -serial "$CONSOLE"
                  -serial "tcp::$LINK_PORT,server,nowait"
                  -serial "tcp::$FB_PORT,server,nowait" )
        ;;
    1)  # UART1 carries the framebuffer instead of link_proto: a clean,
        # uncontaminated stream, but there is then no link at all.
        SERIALS=( -serial "$CONSOLE"
                  -serial "tcp::$FB_PORT,server,nowait" )
        ;;
    0)  # UART0 carries the framebuffer *and* the console, so the link keeps a
        # UART of its own and stays testable. The console is not lost: it is
        # interleaved on :$FB_PORT between frames, and viewer.py --console-log
        # writes those bytes back out to a file.
        SERIALS=( -serial "tcp::$FB_PORT,server,nowait"
                  -serial "tcp::$LINK_PORT,server,nowait" )
        ;;
esac

ARGS=(
    -machine esp32s3
    -m 8M
    -drive "file=$FLASH,if=mtd,format=raw"
    "${SERIALS[@]}"
    -nographic
)
if [ "$HEADLESS" = 1 ]; then
    ARGS+=( -no-reboot )
fi

if [ "$DEBUG" = 1 ]; then
    ARGS+=( -gdb "tcp::$GDB_PORT" -S )
    echo "halted at reset. attach with:"
    echo "  xtensa-esp32s3-elf-gdb $BUILD/gbagate.elf -ex 'target remote :$GDB_PORT'"
fi

case "$AUX_UART" in
    2)  echo "link_proto on tcp://127.0.0.1:$LINK_PORT   (python3 qemu/broker.py --loop)"
        echo "framebuffer on tcp://127.0.0.1:$FB_PORT   (python3 qemu/fbgrab.py --one)"
        if [ "$HEADLESS" = 1 ]; then echo "console -> $BUILD/console.log"; fi
        ;;
    1)  echo "framebuffer on tcp://127.0.0.1:$FB_PORT   (python3 qemu/fbgrab.py --one)"
        echo "NO link port: UART1 is carrying the framebuffer, so broker.py and"
        echo "mGBA have nothing to attach to. Use --aux-uart 0 to keep the link."
        if [ "$HEADLESS" = 1 ]; then echo "console -> $BUILD/console.log"; fi
        ;;
    0)  echo "link_proto on tcp://127.0.0.1:$LINK_PORT   (python3 qemu/broker.py --loop)"
        echo "framebuffer AND console share tcp://127.0.0.1:$FB_PORT"
        echo "  python3 qemu/viewer.py --console-log $BUILD/console.log"
        ;;
esac
echo "image must have been built with -D QEMU_AUX_UART=$AUX_UART"
if [ "$HEADLESS" = 0 ] && [ "$AUX_UART" != 0 ]; then
    echo "ctrl-a x to quit"
fi
echo

exec qemu-system-xtensa "${ARGS[@]}"
