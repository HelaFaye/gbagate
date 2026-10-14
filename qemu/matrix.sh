#!/usr/bin/env bash
#
# matrix.sh -- every configuration, one after the other, and a table at the end.
#
#   ./qemu/matrix.sh              everything
#   ./qemu/matrix.sh --sim        skip QEMU; use build/gate_sim instead
#   ./qemu/matrix.sh --quick      gate image only, all four input modes
#
# Two axes, and they are not equally interesting:
#
#   variant      link / gate / full -- changes what is compiled and which
#                modes exist. The tester ROM only exercises the gate protocol,
#                so `link` has nothing for it to talk to; that row checks the
#                image boots and offers the right menu, nothing more.
#
#   input mode   encoder / 5-way+2 / 5-way+4 / 5-way+5 -- changes the UI only.
#                The ROM cannot see it at all. What this proves is that the
#                firmware boots, links and runs the suite with each setting
#                stored, which is worth knowing because the setting is read at
#                startup and a bad one used to mean dead buttons.
#
# A note on the timeout: the tester ROM keeps clocking after its verdict, so
# mGBA never exits on its own and every row runs the full TIMEOUT. It also does
# not stop on a plain TERM, so `timeout -k` is needed to escalate -- without it
# a row hangs long past its limit and a four-row matrix looks like a crash.
#
# So: the ROM verdict is the same test each time. What varies is whether the
# configuration under it still gets there. A row that fails tells you the
# configuration is broken, not that the protocol is.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

LOGS="$ROOT/build/matrix"
LINK_PORT="${LINK_PORT:-5556}"
FB_PORT="${FB_PORT:-5557}"
MGBA_PORT="${MGBA_PORT:-7000}"
TIMEOUT="${TIMEOUT:-60}"

USE_SIM=0
VARIANTS="link gate full"
MODES="0 1 2 3"

while [ $# -gt 0 ]; do
    case "$1" in
        --sim)   USE_SIM=1; shift ;;
        --quick) VARIANTS="gate"; shift ;;
        --variants) VARIANTS="$2"; shift 2 ;;
        --modes) MODES="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

mode_name() {
    case "$1" in
        0) echo "encoder"   ;;
        1) echo "5-way+2"   ;;
        2) echo "5-way+4"   ;;
        3) echo "5-way+5"   ;;
        *) echo "?"         ;;
    esac
}

mkdir -p "$LOGS"
RESULTS="$LOGS/results.txt"
: > "$RESULTS"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mskip:\033[0m %s\n' "$*"; }

cleanup() {
    pkill -9 -f gate_sim        >/dev/null 2>&1
    pkill -9 -f 'broker.py'     >/dev/null 2>&1
    pkill -9 -f mgba-headless   >/dev/null 2>&1
    pkill -9 -f qemu-system-xtensa >/dev/null 2>&1
    sleep 0.4
}
trap 'cleanup; exit 130' INT TERM

# A port still bound from the previous row is the usual reason a run looks
# hung, so wait for it rather than racing it.
wait_port_free() {
    local p="$1" n=0
    command -v ss >/dev/null 2>&1 || { sleep 1; return 0; }
    while ss -ltn 2>/dev/null | grep -q ":$p "; do
        sleep 0.3
        n=$((n + 1))
        [ "$n" -gt 40 ] && return 1
    done
    return 0
}

find_mgba() {
    for c in mgba-headless mgba-qt mgba; do
        command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }
    done
    [ -x "$ROOT/.reference/mgba-master/build/mgba-headless" ] && \
        echo "$ROOT/.reference/mgba-master/build/mgba-headless"
}

MGBA="$(find_mgba)"
[ -z "$MGBA" ] && { echo "no mGBA found" >&2; exit 1; }
[ -f "$ROOT/rom/tester.gba" ] || { echo "no tester ROM: make -C rom" >&2; exit 1; }

# ---- one row ------------------------------------------------------------

run_one() {
    local variant="$1" mode="$2"
    local tag="$variant-$(mode_name "$mode")"
    local dir="$LOGS/$tag"
    mkdir -p "$dir"

    cleanup
    wait_port_free "$LINK_PORT" || { echo "$tag PORT-BUSY" >> "$RESULTS"; return; }

    if [ "$USE_SIM" = 1 ]; then
        "$ROOT/build/gate_sim" "$LINK_PORT" bn6 --selftest >"$dir/fw.log" 2>&1 &
    else
        local flash="$ROOT/build/qemu-$variant/flash.bin"
        if [ ! -f "$flash" ]; then
            echo "$tag NOT-BUILT" >> "$RESULTS"
            warn "$tag: no image (./qemu/build.sh $variant)"
            return
        fi
        qemu-system-xtensa -machine esp32s3 -m 8M \
            -drive "file=$flash,if=mtd,format=raw" \
            -serial "file:$dir/console.log" \
            -serial "tcp::$LINK_PORT,server,nowait" \
            -serial "tcp::$FB_PORT,server,nowait" \
            -nographic -no-reboot >"$dir/qemu.log" 2>&1 &
    fi
    sleep 1.5

    python3 qemu/broker.py --qemu "127.0.0.1:$LINK_PORT" --mgba "$MGBA_PORT" \
        >"$dir/broker.log" 2>&1 &
    sleep 1

    LINK_PORT="$MGBA_PORT" ROMTEST_RESULT="$dir/rom_test.result" \
        timeout -k 5 "$TIMEOUT" "$MGBA" --script qemu/mgba_tester.lua \
        "$ROOT/rom/tester.gba" >"$dir/mgba.log" 2>&1

    # Record before cleaning up. Cleanup pkills and sleeps, and if the run is
    # interrupted during it the row's result would be lost -- which is exactly
    # what happened the first time this was used: a row passed, and the table
    # showed nothing.
    local verdict tests
    if [ -f "$dir/rom_test.result" ]; then
        verdict="$(head -1 "$dir/rom_test.result")"
        tests="$(grep -m1 '^tests=' "$dir/rom_test.result" | cut -d= -f2)"
        echo "$tag ${verdict:-NO-VERDICT} ${tests:-??}" >> "$RESULTS"
    else
        echo "$tag NO-VERDICT --" >> "$RESULTS"
    fi

    cleanup
    return

}

# ---- the matrix ---------------------------------------------------------

say "mGBA: $MGBA"
[ "$USE_SIM" = 1 ] && say "gate_sim standing in for QEMU"

for v in $VARIANTS; do
    for m in $MODES; do
        printf '  %-18s ' "$v/$(mode_name "$m")"
        run_one "$v" "$m"
        tail -1 "$RESULTS" | awk '{ $1=""; print substr($0,2) }'
    done
done

# ---- the table ----------------------------------------------------------

echo
{
printf '%-20s %-10s %s\n' "configuration" "verdict" "checks"
printf '%-20s %-10s %s\n' "--------------------" "----------" "------"
while read -r tag verdict tests; do
    printf '%-20s %-10s %s\n' "$tag" "$verdict" "$tests"
done < "$RESULTS"
} | tee "$LOGS/table.txt"

fails=$(grep -vc ' PASS ' "$RESULTS" 2>/dev/null || echo 0)
echo
if [ "$fails" -eq 0 ]; then
    echo "every configuration passed"
else
    echo "$fails configuration(s) did not pass -- logs in $LOGS"
fi
exit "$fails"
