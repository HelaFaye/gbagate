#!/usr/bin/env bash
#
# verify.sh -- run gbagate's GB-Link simulator suites against upstream's own
# firmware tree, with the patch series in backport/GBLink-Firmware applied or
# not, under ASan and UBSan.
#
#   backport/verify.sh [--unpatched] [UPSTREAM_CLONE]
#
# The simulator is built from upstream's src/ (at the series' base commit,
# plus the series unless --unpatched), not from third_party/. Only what a host
# build cannot take from upstream comes from gbagate:
#
#   layers/usbLayer.hpp, layers/serialLayer.hpp
#       upstream's sit on Zephyr's USB stack and UART driver; gbagate's keep
#       their interface over the simulator's pipes (PATCHES.md, Replaced files);
#   backport/harness/transport-local*.diff
#       applied to the tree's own layers/transport.{hpp,cpp}: the Local route
#       the wireless-cable tests drive (PATCHES.md P8), and nothing else.
#       transport-local.diff goes on top of the series (which changes those
#       files), transport-local-base.diff on the unpatched base.
#
# So tests of gbagate-only features (0x4F, 0x4E, mode 0x80, the wireless
# cable) fail here by design; every other test is upstream's code under test.
# Prints each suite's verdict and the names of the failing tests.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE=1b49b765c350d2ecd7dd4ed79e5428d9848c7443
PATCHED=1
if [ "${1:-}" = "--unpatched" ]; then PATCHED=0; shift; fi
CLONE="${1:-$ROOT/build/backport/GBLink-Firmware}"
WORK="$ROOT/build/backport/$([ $PATCHED = 1 ] && echo patched || echo unpatched)"

if [ ! -d "$CLONE/.git" ]; then
    mkdir -p "$(dirname "$CLONE")"
    git clone -q https://github.com/GB-Link/GBLink-Firmware "$CLONE" || exit 1
fi
git -C "$CLONE" cat-file -e "$BASE^{commit}" 2>/dev/null || git -C "$CLONE" fetch -q origin || exit 1

rm -rf "$WORK"
git -C "$CLONE" worktree prune
git -C "$CLONE" worktree add -q --detach "$WORK/tree" "$BASE" || exit 1
if [ $PATCHED = 1 ]; then
    git -C "$WORK/tree" -c user.name=verify -c user.email=verify@localhost \
        am -q "$HERE"/GBLink-Firmware/*.patch || { echo "the series does not apply to $BASE"; exit 1; }
fi
SRC="$WORK/tree/src"
V="$ROOT/third_party/gblink-firmware/src"
cp "$V/layers/usbLayer.hpp" "$V/layers/serialLayer.hpp" "$SRC/layers/"
HARNESS="$HERE/harness/transport-local$([ $PATCHED = 1 ] || echo -base).diff"
patch -s -d "$WORK/tree" -p1 < "$HARNESS" || { echo "the harness ($HARNESS) does not apply"; exit 1; }

"$ROOT/test/gblink_sim/fetch_librfu.sh" >/dev/null 2>&1 || true
make -s -C "$ROOT/test/gblink_sim" V="$SRC" OUT="$WORK/sim" \
    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    >"$WORK/build.log" 2>&1 || { echo "build failed: $WORK/build.log"; tail -20 "$WORK/build.log"; exit 1; }

echo "upstream $BASE, $([ $PATCHED = 1 ] && echo "with the series ($(ls "$HERE"/GBLink-Firmware/*.patch | wc -l) patches)" || echo unpatched), ASan+UBSan"
for t in test_gblink.py test_rfu.py test_peer.py; do
    out=$(GBLINK_SIM="$WORK/sim/gblink_sim" ASAN_OPTIONS=detect_leaks=0 \
          python3 -I "$ROOT/test/gblink_sim/$t" 2>&1)
    printf '%s\n' "$out" | tail -1
    printf '%s\n' "$out" | sed -n 's/^  FAIL  /    failed: /p'
done
