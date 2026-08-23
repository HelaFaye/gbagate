#!/usr/bin/env bash
#
# setup.sh -- check the toolchain and install Espressif's QEMU.
#
# ESP-IDF 5.x ships a QEMU integration; this installs the emulator binary and
# tells you what it can and cannot do before you spend time on it.

set -u

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- ESP-IDF -----------------------------------------------------------

[ -n "${IDF_PATH:-}" ] || die "IDF_PATH is not set. Source \$IDF_PATH/export.sh first."
command -v idf.py >/dev/null 2>&1 || die "idf.py not on PATH. Source export.sh first."

IDF_VER="$(idf.py --version 2>/dev/null | head -1)"
say "ESP-IDF: $IDF_VER"

case "$IDF_VER" in
    *v4.*) die "ESP-IDF 4.x. QEMU integration and tusb_msc_storage both need 5.x." ;;
    *v5.*) ;;
    *)     warn "could not parse the IDF version; expecting 5.x" ;;
esac

# ---- QEMU ---------------------------------------------------------------
# The Xtensa build covers ESP32 and ESP32-S3. There is no such binary as
# qemu-system-esp32s3; the machine is selected with -machine.

say "installing the QEMU tool via idf_tools"
python3 "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa || \
    warn "idf_tools install failed; you may already have it"

if command -v qemu-system-xtensa >/dev/null 2>&1; then
    say "qemu-system-xtensa: $(qemu-system-xtensa --version | head -1)"
else
    warn "qemu-system-xtensa not on PATH."
    warn "Run: . \$IDF_PATH/export.sh   (it adds the tool directories)"
fi

say "machines this QEMU knows about:"
qemu-system-xtensa -machine help 2>/dev/null | grep -i esp | sed 's/^/    /' || \
    warn "could not list machines"

if ! qemu-system-xtensa -machine help 2>/dev/null | grep -qi esp32s3; then
    warn "This QEMU has no esp32s3 machine."
    warn "Espressif's S3 support arrived later than ESP32/C3 and some"
    warn "distributions still ship without it. Options:"
    warn "  - update the tool:  idf_tools.py install qemu-xtensa"
    warn "  - or build from https://github.com/espressif/qemu"
    warn "  - or test the portable logic on -machine esp32, which is mature"
fi

# ---- optional ------------------------------------------------------------

command -v socat >/dev/null 2>&1 || warn "socat not found (only needed for ad-hoc plumbing)"
python3 -c 'import selectors' 2>/dev/null || warn "python3 selectors missing?"

# ---- the part people get wrong -------------------------------------------

cat <<'NOTE'

What QEMU can and cannot do for this project
--------------------------------------------
CAN:
  boot the firmware and run app_main
  variant resolution, task creation, core pinning
  NVS and the FAT partition on emulated flash
  the portable logic on real Xtensa: alignment, stack, endianness
  link_bridge over UART, against qemu/broker.py

CANNOT:
  the GBA link PHY. No peripheral is wired to anything; bit-banged GPIO
  timing is exactly what QEMU does not model.
  USB-OTG. No CDC, no MSC. Use UART instead -- that is why
  port/esp32s3/transport_uart.c exists.
  WiFi or ESP-NOW.
  the ST7735. No SPI device on the other end.

So QEMU tests the firmware's logic in situ. It does not reduce the bench work
listed in PROJECT_STATUS.md by one item. Both are worth doing; neither
substitutes for the other.
NOTE

say "setup finished"
