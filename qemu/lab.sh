#!/usr/bin/env bash
#
# lab.sh -- bring the whole emulated rig up with one command, and take it down
# cleanly afterwards.
#
#   ./qemu/lab.sh                    firmware + viewer + tester ROM, automated
#   ./qemu/lab.sh --game MBN6CG.gba  firmware + viewer + a real game
#   ./qemu/lab.sh --no-mgba          just the firmware and its screen
#   ./qemu/lab.sh --no-viewer        leave :5557 free for rawtap.py/fbgrab.py
#   ./qemu/lab.sh --aux-uart 0       for a QEMU that only wires two UARTs
#   ./qemu/lab.sh --sim              use build/gate_sim instead of QEMU
#
# Topology:
#
#   [ mGBA + ROM ] --Lua--> :7000 broker :5556 --UART1--> [ firmware in QEMU ]
#                                                          UART2 :5557
#                                                            |
#                                                    [ viewer.py ] screen + keys
#
# That is the --aux-uart 2 layout, and it needs a QEMU whose esp32s3 machine
# actually attaches three UARTs. One that attaches two discards the framebuffer
# without saying so -- the socket still opens, the viewer still connects, and
# the screen simply stays blank forever. ./qemu/uart2check.sh decides which you
# have. With --aux-uart 0 the framebuffer and the console share UART0 and the
# link keeps UART1, so everything above still runs and only the console moves:
#
#   [ mGBA + ROM ] --Lua--> :7000 broker :5556 --UART1--> [ firmware in QEMU ]
#                                                          UART0 :5557
#                                                            |
#                                            [ viewer.py ] screen + keys + console
#
# The image has to be built to match, or the firmware writes to a UART that is
# not the one the socket is on:  ./qemu/build.sh gate --aux-uart 0
#
# Everything is started in the background here and reaped on exit, including on
# Ctrl-C, so a failed run never leaves a QEMU or a bound port behind -- which is
# the usual way an afternoon of this gets confusing.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

VARIANT="${VARIANT:-gate}"
LINK_PORT="${LINK_PORT:-5556}"
FB_PORT="${FB_PORT:-5557}"
MGBA_PORT="${MGBA_PORT:-7000}"
GAME=""
ROM="$ROOT/rom/tester.gba"
USE_MGBA=1
USE_SIM=0
USE_VIEWER=1
VIEW_MODE=""
AUX_UART="${AUX_UART:-2}"
LOGS="$ROOT/build/lab"

while [ $# -gt 0 ]; do
    case "$1" in
        --game)     GAME="$2"; shift 2 ;;
        --rom)      ROM="$2"; shift 2 ;;
        --no-mgba)  USE_MGBA=0; shift ;;
        --no-viewer) USE_VIEWER=0; shift ;;
        --aux-uart) AUX_UART="$2"; shift 2 ;;
        --sim)      USE_SIM=1; shift ;;
        --term)     VIEW_MODE="--term"; shift ;;
        --tk)       VIEW_MODE="--tk"; shift ;;
        --variant)  VARIANT="$2"; shift 2 ;;
        -h|--help)  sed -n '2,38p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

case "$AUX_UART" in
    0|1|2) ;;
    *) echo "--aux-uart must be 0, 1 or 2 (got '$AUX_UART')" >&2; exit 1 ;;
esac

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mskip:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mfail:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$LOGS"

PIDS=""
cleanup() {
    [ -n "$PIDS" ] && kill $PIDS 2>/dev/null
    sleep 0.3
    [ -n "$PIDS" ] && kill -9 $PIDS 2>/dev/null
    wait 2>/dev/null
    say "stopped; logs in $LOGS"
}
# HUP matters as much as INT here: closing the terminal (or dropping an ssh
# session) sends SIGHUP, and bash does *not* run the EXIT trap when it dies on
# an untrapped fatal signal -- so without HUP listed, shutting the window is
# exactly the case that leaks a QEMU still holding :5556.
trap cleanup EXIT INT TERM HUP

# A port left bound by a previous run is the most common cause of a launch that
# looks hung, so check rather than let it fail obscurely.
port_busy() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -q ":$1 "
    else
        return 1
    fi
}
# Naming the offending pid turns "an earlier run is still going" from a dead end
# into something actionable -- the leaked process is rarely in a visible shell.
port_holder() {
    ss -ltnp 2>/dev/null | grep ":$1 " | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2
}
# With the framebuffer on UART1 there is no link port to bind, and nothing to
# drive it either, so neither is checked or started.
LINK_AVAILABLE=1
if [ "$AUX_UART" = 1 ] && [ "$USE_SIM" = 0 ]; then
    LINK_AVAILABLE=0
    if [ "$USE_MGBA" = 1 ]; then
        warn "--aux-uart 1 gives the framebuffer UART1, so there is no link: mGBA and the broker are skipped"
        USE_MGBA=0
    fi
fi

PORTS="$FB_PORT"
if [ "$LINK_AVAILABLE" = 1 ]; then PORTS="$PORTS $LINK_PORT"; fi
if [ "$USE_MGBA" = 1 ];      then PORTS="$PORTS $MGBA_PORT"; fi

for p in $PORTS; do
    if port_busy "$p"; then
        holder="$(port_holder "$p")"
        if [ -n "$holder" ]; then
            die "port $p is already in use by pid $holder -- an earlier run is still going. Clear it with: kill $holder"
        fi
        die "port $p is already in use -- an earlier run is still going"
    fi
done

# ---- 1. the firmware ----------------------------------------------------

if [ "$USE_SIM" = 1 ]; then
    [ -x "$ROOT/build/gate_sim" ] || die "build/gate_sim missing. Run: make gate-sim"
    say "gate_sim on :$LINK_PORT (no QEMU, no screen)"
    "$ROOT/build/gate_sim" "$LINK_PORT" bn6 >"$LOGS/firmware.log" 2>&1 &
    PIDS="$PIDS $!"
else
    FLASH="$ROOT/build/qemu-$VARIANT/flash.bin"
    BUILD_HINT="./qemu/build.sh $VARIANT"
    [ "$AUX_UART" = 2 ] || BUILD_HINT="$BUILD_HINT --aux-uart $AUX_UART"
    [ -f "$FLASH" ] || die "no image at $FLASH. Run: $BUILD_HINT"
    command -v qemu-system-xtensa >/dev/null 2>&1 || die "qemu-system-xtensa not found. Run ./qemu/setup.sh"

    # The layout is compiled into the image *and* laid out on the command line
    # below, and a mismatch between the two is silent: the firmware writes its
    # frames to a UART no socket is on, and all you see is a blank screen -- the
    # exact symptom this option exists to cure. build.sh always passes
    # -D QEMU_AUX_UART, so the value it was built with is recorded in the cache.
    # An image built before that flag existed has no entry and behaves as UART2.
    CACHE="$ROOT/build/qemu-$VARIANT/CMakeCache.txt"
    if [ -f "$CACHE" ]; then
        IMAGE_AUX="$(grep -m1 '^QEMU_AUX_UART[:=]' "$CACHE" | cut -d= -f2)"
        [ -n "$IMAGE_AUX" ] || IMAGE_AUX=2
        if [ "$IMAGE_AUX" != "$AUX_UART" ]; then
            die "$FLASH was built for the --aux-uart $IMAGE_AUX layout, but this run asks for $AUX_UART.
       The firmware would write its frames to a UART nothing is listening on and the
       screen would stay blank. Rebuild to match: $BUILD_HINT"
        fi
    fi

    # The -serial order is the UART numbering; see the header. Whichever layout
    # is chosen, the image must have been built with a matching QEMU_AUX_UART or
    # the firmware will write its frames to a UART nothing is listening on.
    case "$AUX_UART" in
        2)  say "QEMU: $VARIANT image, link :$LINK_PORT, framebuffer :$FB_PORT (UART2)"
            SERIALS=( -serial "file:$LOGS/console.log"
                      -serial "tcp::$LINK_PORT,server,nowait"
                      -serial "tcp::$FB_PORT,server,nowait" )
            ;;
        1)  say "QEMU: $VARIANT image, framebuffer :$FB_PORT (UART1), no link"
            SERIALS=( -serial "file:$LOGS/console.log"
                      -serial "tcp::$FB_PORT,server,nowait" )
            ;;
        0)  say "QEMU: $VARIANT image, link :$LINK_PORT, framebuffer+console :$FB_PORT (UART0)"
            SERIALS=( -serial "tcp::$FB_PORT,server,nowait"
                      -serial "tcp::$LINK_PORT,server,nowait" )
            ;;
    esac

    qemu-system-xtensa \
        -machine esp32s3 -m 8M \
        -drive "file=$FLASH,if=mtd,format=raw" \
        "${SERIALS[@]}" \
        -nographic -no-reboot >"$LOGS/qemu.log" 2>&1 &
    PIDS="$PIDS $!"
fi
sleep 1.5

# ---- 2. the broker ------------------------------------------------------

if [ "$USE_MGBA" = 1 ]; then
    say "broker: :$LINK_PORT <-> :$MGBA_PORT"
    python3 qemu/broker.py --qemu "127.0.0.1:$LINK_PORT" --mgba "$MGBA_PORT" -v \
        >"$LOGS/broker.log" 2>&1 &
    PIDS="$PIDS $!"
    sleep 1
fi

# ---- 3. the screen ------------------------------------------------------

VIEWER_PID=""
if [ "$USE_SIM" = 0 ] && [ "$USE_VIEWER" = 0 ]; then
    # A QEMU tcp chardev serves one client at a time, so the viewer and a
    # diagnostic tap cannot both hold :5557. Skipping the viewer is the only way
    # to point rawtap.py or fbgrab.py at a full lab session.
    say "viewer skipped; :$FB_PORT is free for: python3 qemu/rawtap.py $FB_PORT 10"
    if [ "$AUX_UART" = 0 ]; then
        warn "the console shares :$FB_PORT in this layout, so $LOGS/console.log stays empty until something attaches"
    fi
elif [ "$USE_SIM" = 0 ]; then
    say "viewer: up/down to move, enter to select, q to quit"
    # With the console on the same socket, the bytes the viewer skips past while
    # resynchronising are the log -- so hand it somewhere to put them and the
    # console survives the sharing instead of being thrown away.
    VIEW_CONSOLE=""
    if [ "$AUX_UART" = 0 ]; then
        VIEW_CONSOLE="--console-log $LOGS/console.log"
    fi
    python3 qemu/viewer.py --fb "127.0.0.1:$FB_PORT" $VIEW_MODE $VIEW_CONSOLE \
        --shot-dir "$LOGS" 2>"$LOGS/viewer.log" &
    VIEWER_PID=$!
    PIDS="$PIDS $VIEWER_PID"
fi

# ---- 4. the game --------------------------------------------------------

if [ "$USE_MGBA" = 1 ]; then
    MGBA=""
    for c in mgba-headless mgba-qt mgba; do
        command -v "$c" >/dev/null 2>&1 && { MGBA="$c"; break; }
    done
    [ -z "$MGBA" ] && [ -x "$ROOT/.reference/mgba-master/build/mgba-headless" ] && \
        MGBA="$ROOT/.reference/mgba-master/build/mgba-headless"

    if [ -z "$MGBA" ]; then
        warn "no mGBA found; the firmware and viewer are up, nothing is driving the link"
    else
        if [ -n "$GAME" ]; then
            # A retail game drives its own SIO, so the register-poking script
            # is the only option -- and it is the uncertain path. See the
            # caveat in qemu/README.md.
            [ -f "$GAME" ] || die "no such ROM: $GAME"
            say "mGBA: $(basename "$GAME") with mgba_link.lua (retail path)"
            LINK_PORT="$MGBA_PORT" "$MGBA" --script qemu/mgba_link.lua "$GAME" \
                >"$LOGS/mgba.log" 2>&1 &
        else
            [ -f "$ROM" ] || die "no tester ROM at $ROM. Run: make -C rom"
            say "mGBA: $(basename "$ROM") with mgba_tester.lua (self-checking)"
            LINK_PORT="$MGBA_PORT" \
            ROMTEST_RESULT="$LOGS/rom_test.result" \
                "$MGBA" --script qemu/mgba_tester.lua "$ROM" \
                >"$LOGS/mgba.log" 2>&1 &
        fi
        PIDS="$PIDS $!"
    fi
fi

# ---- 5. wait ------------------------------------------------------------

say "running. Ctrl-C to stop."
echo
if [ -n "$VIEWER_PID" ]; then
    # The viewer owns the terminal, so its exit is the signal to tear down.
    wait "$VIEWER_PID" 2>/dev/null
else
    while :; do sleep 1; done
fi

if [ -f "$LOGS/rom_test.result" ]; then
    echo
    say "tester verdict:"
    sed 's/^/    /' "$LOGS/rom_test.result"
fi
