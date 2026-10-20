#!/usr/bin/env bash
#
# run.sh -- GB-Link's own host software against the gbagate firmware.
#
#   test/gblink_compat/run.sh
#   GBLINK_SRC=/path/to/checkouts test/gblink_compat/run.sh
#
# Bundles, unmodified, the netplay bridge's engine, Celio-Client's link-session
# code and Celio-Server, swapping only packages that need libusb, a browser or
# Angular for stubs, and runs the tests in this directory against
# build/gblink_sim -- GB-Link's firmware on the gbagate shim.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build/gblink_compat"
mkdir -p "$BUILD"

# GB-Link checkouts: from $GBLINK_SRC (a directory holding them, used as they
# are), else cloned into build/ at the commits below -- the ones this suite
# was last verified against, so an upstream change cannot turn it red on its
# own. GBLINK_COMPAT_HEADS=1 tests upstream's heads instead (CI does that
# weekly, apart from the gate).
PINS="
gblink-netplay-bridge c554c691615c677a67c6b6190e4800ea49d8b5e0
Celio-Client          be562a93b48c6025e398714af82bddfb6498e00c
Celio-Server          9f02d3e6cba624f51fcac9e436be153e1e6e4119
gblink-ereader        26fb65f7a54f142c5654651721d58e15b42300c2
gb-pokemon-web        525b2fb4b62864d6d2f6a1b3cf0f9a4fb0212b0e
gblink-distributor    afa48efcee7f4e5cdb4a2e0070d41e82023055a5
gblink-launcher       bac443a78c05476a2b82ebab021ea58747966998
GB-Link-Switch-LDN    37c693b7f7689d8812a06e54018b5468ed4d8075
"
SRC="${GBLINK_SRC:-$BUILD/src}"
mkdir -p "$SRC"
echo "$PINS" | while read -r repo pin; do
    [ -n "$repo" ] || continue
    if [ ! -d "$SRC/$repo/.git" ] && [ ! -d "$SRC/$repo/src" ]; then
        git clone -q "https://github.com/GB-Link/$repo" "$SRC/$repo"
    fi
    if [ -z "${GBLINK_SRC:-}" ]; then
        if [ -n "${GBLINK_COMPAT_HEADS:-}" ]; then
            git -C "$SRC/$repo" fetch -q origin && git -C "$SRC/$repo" checkout -q --detach origin/HEAD
        else
            git -C "$SRC/$repo" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$SRC/$repo" fetch -q origin
            git -C "$SRC/$repo" checkout -q --detach "$pin"
        fi
    fi
    at=$(git -C "$SRC/$repo" rev-parse HEAD 2>/dev/null || echo none)
    note=""; [ "$at" = "$pin" ] || note=" (pinned: ${pin:0:7})"
    echo "$repo: $(git -C "$SRC/$repo" log -1 --format='%h %cd' --date=short 2>/dev/null || echo 'not a git checkout')$note"
done
# The launcher test patches this commit (backport/README.md's base).
export GBLINK_LAUNCHER_BASE
if [ -n "${GBLINK_COMPAT_HEADS:-}" ]; then GBLINK_LAUNCHER_BASE=origin/HEAD
else GBLINK_LAUNCHER_BASE=bac443a78c05476a2b82ebab021ea58747966998; fi

# The newest public Celio-Client sends status and data with Socket.IO acks
# and retries (b057315, 2026-10-03); the newest public Celio-Server
# (9f02d3e, 2026-06-17) neither acks them nor unwraps the ack argument, so the
# two current repos do not interoperate (see doc/GBLINK.md). The test runs
# the client from just before that change, the pair the public server serves.
LEGACY="$BUILD/src/Celio-Client-pre-ack"
if [ ! -d "$LEGACY/src" ]; then
    git -C "$SRC/Celio-Client" fetch -q --unshallow 2>/dev/null || true
    if ! git -C "$SRC/Celio-Client" worktree add -q --detach "$LEGACY" b057315^ 2>/dev/null; then
        git clone -q "$SRC/Celio-Client" "$LEGACY" && git -C "$LEGACY" checkout -q b057315^
    fi
fi

# esbuild pinned to the bridge's version; Celio's runtime deps at the ranges
# its package.json files ask for; playwright-core for the launcher test, which
# drives the preinstalled Chromium (it downloads no browser). One install for
# all of them: with no package.json, npm prunes whatever a later install does
# not name.
if [ ! -d "$BUILD/node_modules/socket.io-client" ] || [ ! -d "$BUILD/node_modules/playwright-core" ]; then
    (cd "$BUILD" && PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm install --silent --no-save --no-package-lock \
        esbuild@0.24.2 socket.io@4.8.4 socket.io-client@4.8.4 rxjs@7.8.2 \
        uuid@11.1.1 nanoid@5.1.16 true-myth@9.4.0 ws@8.22.0 playwright-core@1.56.1 >/dev/null)
fi

echo "Celio-Client used for the relay test: $(git -C "$LEGACY" log -1 --format='%h %cd' --date=short) (pre-ack, matches the public server)"

"$ROOT/test/gblink_sim/fetch_librfu.sh" || { echo "librfu: not fetched; the Wireless Adapter parts will skip"; [ -z "${GBLINK_NO_SKIP:-}" ]; }
make -s -C "$ROOT/test/gblink_sim"
node "$HERE/build.mjs" "$BUILD" "$SRC/gblink-netplay-bridge" "$LEGACY" "$SRC/Celio-Server"

fail=0
for t in "$HERE"/test_*.mjs; do
    if ! node "$t" "$BUILD"; then fail=1; fi
done

# GB-Link-Switch-LDN's link task (AGPL-3.0, built here from the clone, never
# copied into gbagate) with its UART on the simulator's bridge UART.
LDN="$SRC/GB-Link-Switch-LDN/firmware"
cc -std=c11 -D_GNU_SOURCE -O1 -I "$LDN/tools/host_stubs" -I "$LDN/common" \
    -DLDN_PICO_LINK="\"$LDN/common/pico_link.c\"" "$HERE/ldn_link_host.c" \
    -o "$BUILD/ldn_link_host" -lpthread
if ! python3 -I "$HERE/test_ldn.py" "$BUILD/ldn_link_host"; then fail=1; fi
exit $fail
