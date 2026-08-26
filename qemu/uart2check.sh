#!/usr/bin/env bash
#
# uart2check.sh -- does this QEMU forward UART2 at all?
#
#   ./qemu/uart2check.sh          boot for 6s and report
#   ./qemu/uart2check.sh 12       boot for 12s
#
# The symptom this exists for: qemu/viewer.py connects to :5557, reports no
# error, and never draws. That has two very different causes -- the firmware is
# not emitting on UART2, or QEMU is not wiring UART2 to the third -serial
# chardev -- and a socket cannot tell them apart, because a chardev QEMU created
# but never attached to the machine *still* opens its listening socket and still
# accepts a client. The connect succeeds and nothing ever arrives.
#
# So this takes the sockets out entirely. Every UART goes to a plain file:
#
#     -serial file:uart0-console.log   the ESP-IDF console
#     -serial file:uart1-link.bin      link_proto  (transport_uart.c)
#     -serial file:uart2-fb.bin        framebuffer (st7735_qemu.c)
#
# No ports, no listeners, no clients, no viewer, no Tk -- so nothing downstream
# of QEMU can be blamed for an empty file. UART1 is the control: it is known to
# work (broker.py links against it), so if uart1-link.bin fills and
# uart2-fb.bin stays empty in the same run, the fault is QEMU's UART2 wiring and
# no amount of firmware or viewer work will fix it.
#
# That verdict matters beyond the screen: input_qemu.c reads the same UART2, so
# a UART2 that does not forward also means the encoder is dead under emulation
# and no chip can be slotted by hand.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

VARIANT="${VARIANT:-gate}"
SECS="${1:-6}"
OUT="$ROOT/build/uart2check"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarn:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mfail:\033[0m %s\n' "$*" >&2; exit 1; }

FLASH="$ROOT/build/qemu-$VARIANT/flash.bin"
[ -f "$FLASH" ] || die "no image at $FLASH. Run: ./qemu/build.sh $VARIANT"
command -v qemu-system-xtensa >/dev/null 2>&1 || \
    die "qemu-system-xtensa not found. Run ./qemu/setup.sh"

rm -rf "$OUT"
mkdir -p "$OUT"

QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null
        sleep 0.3
        kill -9 "$QEMU_PID" 2>/dev/null
    fi
    wait 2>/dev/null
}
# HUP as well as INT: closing the terminal must not leak a QEMU, exactly as in
# lab.sh -- bash does not run the EXIT trap when it dies on an untrapped signal.
trap cleanup EXIT INT TERM HUP

say "booting $VARIANT for ${SECS}s with all three UARTs going to files (no sockets)"
qemu-system-xtensa \
    -machine esp32s3 -m 8M \
    -drive "file=$FLASH,if=mtd,format=raw" \
    -serial "file:$OUT/uart0-console.log" \
    -serial "file:$OUT/uart1-link.bin" \
    -serial "file:$OUT/uart2-fb.bin" \
    -nographic -no-reboot >"$OUT/qemu.log" 2>&1 &
QEMU_PID=$!

sleep "$SECS"
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null
QEMU_PID=""

size_of() { [ -f "$1" ] && wc -c <"$1" | tr -d ' ' || echo 0; }

U0="$(size_of "$OUT/uart0-console.log")"
U1="$(size_of "$OUT/uart1-link.bin")"
U2="$(size_of "$OUT/uart2-fb.bin")"

echo
say "bytes captured in ${SECS}s"
printf '    UART0 console     %8s\n' "$U0"
printf '    UART1 link_proto  %8s   (the control: known to work)\n' "$U1"
printf '    UART2 framebuffer %8s\n' "$U2"
echo

# Did the firmware even claim the tap this run? Without this the verdict below
# cannot distinguish "QEMU dropped it" from "st7735_init took the degraded path".
TAP_UP=0
if [ -f "$OUT/uart0-console.log" ] && \
   grep -q 'framebuffer tap on UART2' "$OUT/uart0-console.log" 2>/dev/null; then
    TAP_UP=1
fi

if [ "$U0" = 0 ]; then
    warn "the console is empty -- QEMU did not get as far as running the firmware."
    warn "read $OUT/qemu.log; this says nothing about UART2 either way."
    exit 1
fi

if [ "$TAP_UP" = 0 ]; then
    warn "the firmware did NOT bring the framebuffer tap up this run."
    warn "st7735_init() took its degraded path, so UART2 is silent by the"
    warn "firmware's own choice and QEMU is not implicated. Look for"
    warn "'tap unavailable' in $OUT/uart0-console.log."
    exit 1
fi

say "the firmware did bring the tap up (console says so), so it is emitting"

# Count whole FBUF frames the same way viewer.py does, so this agrees with what
# the viewer would have been able to draw.
python3 - "$OUT/uart2-fb.bin" <<'PY'
import os
import sys

MAGIC = b"FBUF"
HDR = 8

path = sys.argv[1]
if not os.path.exists(path):
    print("    (no UART2 capture file at all -- QEMU never created the chardev)")
    raise SystemExit(0)

with open(path, "rb") as fh:
    data = fh.read()
frames = 0
first = None
i = 0
while True:
    j = data.find(MAGIC, i)
    if j < 0:
        break
    if len(data) - j < HDR:
        break
    w = int.from_bytes(data[j + 4:j + 6], "little")
    h = int.from_bytes(data[j + 6:j + 8], "little")
    if not (0 < w <= 512 and 0 < h <= 512):
        i = j + 4
        continue
    if first is None:
        first = (w, h)
    need = HDR + w * h * 2
    if len(data) - j < need:
        break
    frames += 1
    i = j + need

if first:
    w, h = first
    print(f"    first FBUF header: {w}x{h} -> {HDR + w * h * 2} bytes per frame")
print(f"    complete FBUF frames in the file: {frames}")
PY

echo
if [ "$U2" = 0 ] && [ "$U1" != 0 ]; then
    printf '\033[1;31mVERDICT\033[0m  UART2 is not wired by this QEMU.\n'
    echo "UART1 filled in the same run, so serial chardevs work and the ordering"
    echo "is right -- the third one simply is not attached to a UART peripheral"
    echo "in this build's esp32s3 machine. The firmware wrote to UART2 and QEMU"
    echo "discarded it."
    echo
    echo "This is not fixable in the firmware or in viewer.py. Options:"
    echo "  - a QEMU with 3 UARTs on esp32s3 (idf_tools.py install qemu-xtensa)"
    echo "  - or move the framebuffer onto UART1 and multiplex it with the link"
    echo "  - note the encoder input dies with it: input_qemu.c reads UART2, so"
    echo "    no chip can be slotted by hand under emulation until this is fixed"
    exit 1
fi

if [ "$U2" = 0 ] && [ "$U1" = 0 ]; then
    warn "neither UART1 nor UART2 produced anything, but the console did."
    warn "That is a broader chardev problem than UART2 alone; check $OUT/qemu.log."
    exit 1
fi

printf '\033[1;32mVERDICT\033[0m  UART2 forwards: %s bytes reached the file.\n' "$U2"
echo "So the firmware and QEMU are both fine and a blank viewer is downstream of"
echo "here -- the socket path or viewer.py itself. Next:"
echo "  ./qemu/run.sh $VARIANT --headless      # terminal 1"
echo "  python3 qemu/rawtap.py 5557 10         # terminal 2"
echo "If that shows whole frames too, the fault is in viewer.py."
exit 0
