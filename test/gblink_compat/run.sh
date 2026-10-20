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

# GB-Link checkouts: from $GBLINK_SRC (a directory holding them), else cloned.
SRC="${GBLINK_SRC:-$BUILD/src}"
mkdir -p "$SRC"
for repo in gblink-netplay-bridge Celio-Client Celio-Server gblink-ereader gb-pokemon-web gblink-distributor gblink-launcher; do
    if [ ! -d "$SRC/$repo/.git" ] && [ ! -d "$SRC/$repo/src" ]; then
        git clone -q "https://github.com/GB-Link/$repo" "$SRC/$repo"
    fi
    echo "$repo: $(git -C "$SRC/$repo" log -1 --format='%h %cd' --date=short 2>/dev/null || echo 'not a git checkout')"
done

# The newest public Celio-Client sends status and data with Socket.IO acks
# and retries (b057315, 2026-10-03); the newest public Celio-Server
# (9f02d3e, 2026-06-17) neither acks them nor unwraps the ack argument, so the
# two current repos do not interoperate (see doc/GBLINK.md). The test runs
# the client from just before that change, the pair the public server serves.
LEGACY="$BUILD/src/Celio-Client-pre-ack"
if [ ! -d "$LEGACY/src" ]; then
    git -C "$SRC/Celio-Client" fetch -q --unshallow 2>/dev/null || true
    git -C "$SRC/Celio-Client" worktree add -q --detach "$LEGACY" b057315^ 2>/dev/null \
        || git clone -q "$SRC/Celio-Client" "$LEGACY" && git -C "$LEGACY" checkout -q b057315^
fi

# esbuild pinned to the bridge's version; Celio's runtime deps at the ranges
# its package.json files ask for; playwright-core for the launcher test, which
# drives the preinstalled Chromium (it downloads no browser). One install for
# all of them: with no package.json, npm prunes whatever a later install does
# not name.
if [ ! -d "$BUILD/node_modules/socket.io-client" ] || [ ! -d "$BUILD/node_modules/playwright-core" ]; then
    (cd "$BUILD" && PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 npm install --silent --no-save --no-package-lock \
        esbuild@0.24.2 socket.io@^4.8.1 socket.io-client@^4.8.1 rxjs@^7.8.2 \
        uuid@^11.1.0 nanoid@^5.1.5 true-myth@^9.1.0 ws@^8.18.3 playwright-core@1.56.1 >/dev/null)
fi

echo "Celio-Client used for the relay test: $(git -C "$LEGACY" log -1 --format='%h %cd' --date=short) (pre-ack, matches the public server)"

make -s -C "$ROOT/test/gblink_sim"
node "$HERE/build.mjs" "$BUILD" "$SRC/gblink-netplay-bridge" "$LEGACY" "$SRC/Celio-Server"

fail=0
for t in "$HERE"/test_*.mjs; do
    if ! node "$t" "$BUILD"; then fail=1; fi
done
exit $fail
