#!/usr/bin/env bash
#
# Accurate ESP32-S3 carrier-touch + mGBA end-to-end regression test.
#
# Usage:
#   bash qemu/test_accurate_mgba.sh
#
# Optional environment:
#   GBAGATE_LINK_PORT=5557
#   GBAGATE_TEST_TIMEOUT=180
#   GBAGATE_KEEP_QEMU=1
#   GBAGATE_LIVE_OUT=/custom/output/path
#   GBAGATE_LIVE_DIR=/custom/persistent/state/path
#   GBAGATE_SOURCE_FLASH=/path/to/flash-qemu.bin
#   GBAGATE_MGBA=/path/to/mgba-headless

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || {
    echo "ERROR: cannot enter repository"
    exit 2
}

RUN_ID="$(date +%Y%m%d-%H%M%S)"
OUT="${GBAGATE_LIVE_OUT:-/tmp/gbagate-accurate-mgba-$RUN_ID}"
LIVE="${GBAGATE_LIVE_DIR:-$ROOT/build/live-carrier-touch-mgba-$RUN_ID}"
FLASH="${GBAGATE_SOURCE_FLASH:-$ROOT/build/hw-gate-carrier-touch-mgba/flash-qemu.bin}"
MGBA="${GBAGATE_MGBA:-$ROOT/build/mgba-headless-build/mgba-headless}"
LINK_PORT="${GBAGATE_LINK_PORT:-5557}"
TIMEOUT="${GBAGATE_TEST_TIMEOUT:-180}"
KEEP_QEMU="${GBAGATE_KEEP_QEMU:-0}"

QEMU_PID=""
QEMU_CHILD=""
BROKER_PID=""
MGBA_PID=""
INTERRUPTED=0
FINAL_RC=1

port_listening() {
    ss -ltn 2>/dev/null |
        awk -v p="$1" '
            NR > 1 {
                n = split($4, a, ":")
                if (a[n] == p) found = 1
            }
            END { exit !found }
        '
}

alive() {
    [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null
}

stop_process() {
    pid="${1:-}"
    [ -n "$pid" ] || return 0

    if alive "$pid"; then
        kill -TERM "$pid" 2>/dev/null || true

        for i in $(seq 1 50); do
            alive "$pid" || break
            sleep 0.1
        done

        if alive "$pid"; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi

    wait "$pid" 2>/dev/null || true
}

stop_qemu() {
    [ -n "$QEMU_PID" ] || return 0

    # Terminate QEMU first. The Tk launcher notices its child has stopped
    # and runs its normal QMP/window cleanup path.
    if [ -n "$QEMU_CHILD" ] && alive "$QEMU_CHILD"; then
        kill -TERM "$QEMU_CHILD" 2>/dev/null || true
    fi

    for i in $(seq 1 50); do
        alive "$QEMU_PID" || break
        sleep 0.1
    done

    if alive "$QEMU_PID"; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
    fi

    for i in $(seq 1 20); do
        alive "$QEMU_PID" || break
        sleep 0.1
    done

    if alive "$QEMU_PID"; then
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi

    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
}

cleanup() {
    trap - EXIT

    stop_process "$MGBA_PID"
    MGBA_PID=""

    stop_process "$BROKER_PID"
    BROKER_PID=""

    if [ -n "$QEMU_PID" ]; then
        if [ "$FINAL_RC" -eq 0 ] && [ "$KEEP_QEMU" = "1" ]; then
            echo "Keeping QEMU running: launcher PID $QEMU_PID"
        else
            stop_qemu
        fi
    fi
}

trap cleanup EXIT
trap 'INTERRUPTED=1' INT TERM

echo "========== PREFLIGHT =========="

for file in \
    "$FLASH" \
    "$MGBA" \
    "$ROOT/qemu/accurate_live.py" \
    "$ROOT/qemu/broker.py" \
    "$ROOT/qemu/mgba_tester.lua" \
    "$ROOT/rom/tester.gba"
do
    if [ ! -f "$file" ]; then
        echo "ERROR: required file missing: $file"
        exit 2
    fi
done

if [ ! -x "$MGBA" ]; then
    echo "ERROR: mGBA is not executable: $MGBA"
    exit 2
fi

if ! command -v ss >/dev/null 2>&1; then
    echo "ERROR: ss is required (iproute2)."
    exit 2
fi

if port_listening "$LINK_PORT"; then
    echo "ERROR: TCP port $LINK_PORT is already occupied."
    echo "Choose another GBAGATE_LINK_PORT."
    exit 2
fi

if port_listening 7000; then
    echo "ERROR: TCP port 7000 is already occupied."
    exit 2
fi

mkdir -p "$OUT" "$LIVE"

echo "Repository: $ROOT"
echo "Flash source: $FLASH"
echo "Fresh live state: $LIVE"
echo "Logs: $OUT"
echo "UART1 TCP port: $LINK_PORT"

echo
echo "========== START ACCURATE QEMU =========="

rm -f "$OUT/console.log" \
      "$OUT/qemu.log" \
      "$OUT/accurate-qemu.log" \
      "$OUT/rom_test.result" \
      "$OUT/broker-chip-test.log" \
      "$OUT/mgba-chip-test.log"

GBAGATE_SOURCE_FLASH="$FLASH" \
GBAGATE_LIVE_DIR="$LIVE" \
GBAGATE_LIVE_OUT="$OUT" \
GBAGATE_MGBA_BRIDGE=1 \
GBAGATE_LINK_PORT="$LINK_PORT" \
GBAGATE_AUTO_SELFTEST=1 \
python3 qemu/accurate_live.py >"$OUT/accurate-qemu.log" 2>&1 &

QEMU_PID=$!
echo "QEMU launcher PID: $QEMU_PID"

BOOT_READY=0

for i in $(seq 1 300); do
    if [ "$INTERRUPTED" -eq 1 ]; then
        echo "Interrupted during QEMU startup."
        exit 130
    fi

    if ! alive "$QEMU_PID"; then
        echo "ERROR: QEMU launcher exited during startup."
        tail -60 "$OUT/accurate-qemu.log"
        exit 1
    fi

    QEMU_CHILD="$(pgrep -P "$QEMU_PID" -f 'qemu-system-xtensa' | head -1)"

    if grep -q 'shell started with caps 0x00000121' "$OUT/console.log" 2>/dev/null &&
       [ -n "$QEMU_CHILD" ] &&
       port_listening "$LINK_PORT"; then
        BOOT_READY=1
        break
    fi

    sleep 0.1
done

if [ "$BOOT_READY" -ne 1 ]; then
    echo "ERROR: accurate QEMU firmware did not reach a ready state."
    tail -60 "$OUT/console.log" 2>/dev/null || true
    tail -60 "$OUT/qemu.log" 2>/dev/null || true
    exit 1
fi

if ! grep -q 'link source: bridge' "$OUT/console.log" ||
   ! grep -q 'bn6g: 411 chips, gate id FF00' "$OUT/console.log"; then
    echo "ERROR: wrong firmware bridge or gate configuration."
    exit 1
fi

echo "Firmware boot verified."
echo "Waiting for the six bounded Self Test menu taps..."

# The launcher schedules the first QMP touch 3.5 seconds after its
# Tk event loop starts. Six taps run at 120 ms down / 450 ms spacing.
# Nine seconds allows that sequence to finish and all touches to release.
sleep 9

if ! alive "$QEMU_PID" ||
   ! alive "$QEMU_CHILD" ||
   ! port_listening "$LINK_PORT"; then
    echo "ERROR: QEMU stopped during automated menu input."
    exit 1
fi

echo "QEMU survived automated input; proceeding to link test."

echo
echo "========== START BROKER =========="

python3 -u qemu/broker.py \
    --qemu "127.0.0.1:$LINK_PORT" \
    --mgba 7000 -v \
    >"$OUT/broker-chip-test.log" 2>&1 &

BROKER_PID=$!
echo "Broker PID: $BROKER_PID"

BROKER_READY=0

for i in $(seq 1 100); do
    if ! alive "$BROKER_PID"; then
        break
    fi

    if port_listening 7000; then
        BROKER_READY=1
        break
    fi

    sleep 0.1
done

if [ "$BROKER_READY" -ne 1 ]; then
    echo "ERROR: broker failed to listen."
    tail -60 "$OUT/broker-chip-test.log"
    exit 1
fi

echo
echo "========== START mGBA =========="

LINK_HOST=127.0.0.1 \
LINK_PORT=7000 \
ROMTEST_RESULT="$OUT/rom_test.result" \
ROMTEST_MAX_FRAMES=600000 \
"$MGBA" --script "$ROOT/qemu/mgba_tester.lua" \
    "$ROOT/rom/tester.gba" >"$OUT/mgba-chip-test.log" 2>&1 &

MGBA_PID=$!
echo "mGBA PID: $MGBA_PID"

echo
echo "========== SUPERVISED TEST (${TIMEOUT}s) =========="

RESULT_READY=0
REASON="test timeout"

for i in $(seq 1 $((TIMEOUT * 10))); do
    if [ "$INTERRUPTED" -eq 1 ]; then
        REASON="interrupted by user"
        break
    fi

    if [ -s "$OUT/rom_test.result" ] &&
       grep -q '^relink[[:space:]]*pass$' "$OUT/rom_test.result"; then
        RESULT_READY=1
        REASON="complete ROM verdict received"
        break
    fi

    if ! alive "$QEMU_PID" ||
       ! alive "$QEMU_CHILD" ||
       ! port_listening "$LINK_PORT"; then
        REASON="QEMU process or UART1 listener disappeared"
        break
    fi

    if ! alive "$BROKER_PID"; then
        REASON="broker process exited"
        break
    fi

    if ! alive "$MGBA_PID"; then
        REASON="mGBA exited without a complete result"
        break
    fi

    sleep 0.1
done

if [ "$INTERRUPTED" -eq 1 ]; then
    echo "ERROR: $REASON"
    exit 130
fi

if [ "$RESULT_READY" -ne 1 ]; then
    echo "ERROR: no complete ROM verdict: $REASON"
    tail -60 "$OUT/mgba-chip-test.log" 2>/dev/null || true
    exit 1
fi

# Stop mGBA and broker before examining the broker's final summary.
stop_process "$MGBA_PID"
MGBA_PID=""

stop_process "$BROKER_PID"
BROKER_PID=""

echo
echo "========== STRICT REGRESSION VALIDATION =========="

if python3 - "$OUT/rom_test.result" "$OUT/broker-chip-test.log" <<'PY'
from pathlib import Path
import re
import sys

result_path = Path(sys.argv[1])
broker_path = Path(sys.argv[2])

result = result_path.read_text(errors="replace")
broker = broker_path.read_text(errors="replace")
errors = []

lines = result.splitlines()
if not lines or lines[0].strip() != "PASS":
    errors.append("ROM did not report PASS")

values = {}
for line in lines:
    if "=" in line:
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()

required_values = {
    "status": "1",
    "tests": "3F",
    "chip": "0101",
    "gate_id": "FF00",
}

for key, expected in required_values.items():
    actual = values.get(key, "")
    if actual.upper() != expected.upper():
        errors.append(
            f"{key}: expected {expected}, received {actual or '<missing>'}"
        )

checks = ("link", "shape", "counters", "chip", "repeats", "relink")
for check in checks:
    if not re.search(rf"^{check}\s+pass$", result, re.MULTILINE):
        errors.append(f"{check}: explicit pass entry missing")

packets = re.findall(
    r"fw -> game: RESULT seq=(\d+) \[0000 0101 FFFF FFFF\] players=2",
    broker,
)

if len(packets) != 18:
    errors.append(f"expected 18 chip packets, found {len(packets)}")

if len(set(packets)) != len(packets):
    errors.append("chip delivery sequence numbers are duplicated")

summaries = re.findall(
    r"HELLO=(\d+)\s+HELLO_ACK=(\d+)\s+RESULT=(\d+)\s+WORD=(\d+)\s+dropped=(\d+)",
    broker,
)

if not summaries:
    errors.append("broker final summary is missing")
else:
    hello, ack, results, words, dropped = map(int, summaries[-1])

    if hello < 1 or ack < 1:
        errors.append("broker handshake did not complete")

    if results < 18 or words < 18:
        errors.append(
            f"insufficient traffic: RESULT={results}, WORD={words}"
        )

    if dropped != 0:
        errors.append(f"broker dropped {dropped} packets")

if errors:
    print("FAIL — regression requirements not satisfied:")
    for error in errors:
        print(" -", error)
    sys.exit(1)

print("PASS — all regression requirements verified")
print(
    f"ROM: tests={values['tests']} chip={values['chip']} "
    f"gate={values['gate_id']}"
)
print("Chip deliveries: 18 (two groups of nine)")
print(
    f"Broker: HELLO={hello} HELLO_ACK={ack} "
    f"RESULT={results} WORD={words} dropped={dropped}"
)
for check in checks:
    print(f"  {check:9} PASS")
PY
then
    FINAL_RC=0
else
    FINAL_RC=1
fi

echo
echo "========== FINAL ROM RESULT =========="
cat "$OUT/rom_test.result"

echo
echo "========== BROKER SUMMARY =========="
grep -E 'connected to|game connected|disconnected|HELLO=.*RESULT=' \
    "$OUT/broker-chip-test.log" | tail -12 || true

echo
echo "========== CHIP 0101 DELIVERY TRACE =========="
grep -E 'RESULT.*0101' "$OUT/broker-chip-test.log" || true

echo
echo "Artifacts: $OUT"

if [ "$FINAL_RC" -eq 0 ]; then
    echo "Regression test passed."
else
    echo "Regression test failed."
fi

exit "$FINAL_RC"
