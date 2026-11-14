#!/usr/bin/env bash
#
# verify-printer.sh -- the evidence for backport/GBLink-Firmware 0008 (printer
# replies MSB first).
#
#   backport/verify-printer.sh
#
# Upstream only ever replies 0x81 and 0x00, which read the same in either bit
# order, so no test of the shipped firmware can tell the orders apart. This
# builds the simulator twice from upstream's tree (backport/verify.sh's
# checkout) with the printer status set to 0x02 for the experiment, once with
# the series up to 0007 and once with 0008 as well, runs a printer session,
# and prints the status byte the simulated Game Boy -- which samples MSB
# first, as the hardware does -- read after each device ID. Expect 0x40
# (bit-reversed) without 0008 and 0x02 with it.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE=1b49b765c350d2ecd7dd4ed79e5428d9848c7443
CLONE="$ROOT/build/backport/GBLink-Firmware"
WORK="$ROOT/build/backport/printer"
[ -d "$CLONE/.git" ] || { echo "run backport/verify.sh first (it clones upstream)"; exit 1; }

PATCH=$(ls "$HERE"/GBLink-Firmware/*MSB-first*.patch)
rm -rf "$WORK"; mkdir -p "$WORK"
git -C "$CLONE" worktree prune
status=0
for v in without with; do
    git -C "$CLONE" worktree add -q --detach "$WORK/$v" "$BASE" || exit 1
    for p in "$HERE"/GBLink-Firmware/*.patch; do
        [ "$p" = "$PATCH" ] && [ $v = without ] && continue
        git -C "$WORK/$v" -c user.name=verify -c user.email=verify@localhost am -q "$p" || exit 1
    done
    SRC="$WORK/$v/src"
    V="$ROOT/third_party/gblink-firmware/src"
    cp "$V/layers/usbLayer.hpp" "$V/layers/serialLayer.hpp" "$V/layers/transport.hpp" "$V/layers/transport.cpp" "$SRC/layers/"
    sed -i 's/uint8_t printer_status = 0x00;/uint8_t printer_status = 0x02;/' "$SRC/module/gb.cpp"
    grep -q 'printer_status = 0x02' "$SRC/module/gb.cpp" || { echo "could not set the status"; exit 1; }
    make -s -C "$ROOT/test/gblink_sim" V="$SRC" OUT="$WORK/$v/sim" >"$WORK/$v.log" 2>&1 || { echo "build failed"; exit 1; }
    got=$(GBLINK_SIM="$WORK/$v/sim/gblink_sim" python3 -I - "$ROOT" <<'EOF'
import sys, time
sys.path.insert(0, sys.argv[1] + '/test/gblink_sim')
from simdrv import Sim, ST, EV_GB
import test_gblink as T
s = Sim()
s.cmd(0x00, 0x03)
s.wait_status(ST['GBPrinterModeActive'])
s.gba(0x05, *(T.printer_packet(0x01) + T.printer_packet(0x0F)))
s.wait_event(EV_GB, lambda b: b == b'', 20)
time.sleep(0.3)
r = [b[1] for c, b in s.events if c == EV_GB and len(b) == 2]
print(' '.join('%#04x' % r[i + 1] for i in range(len(r) - 1) if r[i] == 0x81))
s.close()
EOF
)
    echo "$v 0008: the Game Boy read status $got"
    want=$([ $v = with ] && echo 0x02 || echo 0x40)
    for b in $got; do [ "$b" = "$want" ] || status=1; done
    [ -n "$got" ] || status=1
done
[ $status = 0 ] && echo "ok: 0x02 arrives bit-reversed without 0008, intact with it"
exit $status
