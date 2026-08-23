#!/usr/bin/env bash
#
# smoke.sh -- boot each image, confirm it starts, and stop.
#
#   ./qemu/smoke.sh              all three
#   ./qemu/smoke.sh gate         just one
#
# Checks the one thing QEMU is genuinely good at: that the firmware boots, the
# variant resolves, and the tasks start. Not a substitute for the bench work.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIMEOUT="${TIMEOUT:-20}"
VARIANTS="${*:-link gate full}"
fails=0

for V in $VARIANTS; do
    BUILD="$ROOT/build/qemu-$V"
    LOG="$BUILD/console.log"

    printf '%-6s ' "$V"
    if [ ! -f "$BUILD/flash.bin" ]; then
        echo "SKIP (not built: ./qemu/build.sh $V)"
        continue
    fi

    rm -f "$LOG"
    timeout "$TIMEOUT" qemu-system-xtensa \
        -machine esp32s3 -m 8M \
        -drive "file=$BUILD/flash.bin,if=mtd,format=raw" \
        -serial "file:$LOG" -serial null \
        -nographic -no-reboot >/dev/null 2>&1

    # Boot is proven by the banner; correctness by the feature line app_main
    # logs after variant_resolve().
    if ! grep -q "gbagate $V" "$LOG" 2>/dev/null; then
        echo "FAIL (no banner; see $LOG)"
        fails=$((fails + 1))
        continue
    fi
    if grep -q "incoherent image" "$LOG"; then
        echo "FAIL (variant did not resolve)"
        fails=$((fails + 1))
        continue
    fi
    if ! grep -q "tasks started" "$LOG"; then
        echo "FAIL (tasks never started)"
        fails=$((fails + 1))
        continue
    fi
    FEATURES="$(grep -o 'features: .*' "$LOG" | head -1 | cut -c11-)"
    echo "ok -- $FEATURES"
done

echo
[ "$fails" = 0 ] && echo "all images boot" || echo "$fails image(s) failed"
exit "$fails"
