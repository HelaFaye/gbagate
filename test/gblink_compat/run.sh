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
Celio-Client          e1bf4f831953c134f45ba074b0155ddc9f9bf616 Celio-Link
Celio-Server          8f509bdcf2a8383b936e0f2f31b2f6553ae421ce Celio-Link
gblink-ereader        26fb65f7a54f142c5654651721d58e15b42300c2
gb-pokemon-web        525b2fb4b62864d6d2f6a1b3cf0f9a4fb0212b0e
gblink-distributor    afa48efcee7f4e5cdb4a2e0070d41e82023055a5
gblink-launcher       HEAD                                     HelaFaye
GB-Link-Switch-LDN    37c693b7f7689d8812a06e54018b5468ed4d8075
"
# The launcher is a private mirror. GB-Link/gblink-launcher stopped being
# publicly clonable in November 2026 and has no licence file (all rights
# reserved), so it is not republished: HelaFaye/gblink-launcher is a private
# copy of upstream's 94b9a95, cloned with GBLINK_LAUNCHER_TOKEN (a read
# token; CI's secret of that name), and tested at the mirror's head (HEAD).
# Without the token only the launcher is missing, and test_launcher.mjs says
# so (and fails, under GBLINK_NO_SKIP); the rest of the suite still runs.
#
# Celio is Celio-Link's: GB-Link/Celio-Server is a fork that stopped at
# 9f02d3e (2026-06-17), before upstream answered the client's acks (Oct 3-6).
# Testing against it made the two look incompatible when they are not.
SRC="${GBLINK_SRC:-$BUILD/src}"
mkdir -p "$SRC"

# Retry transient GitHub clone failures without silently skipping required
# compatibility inputs.
clone_public_repo() {
    local url="$1" dest="$2" attempt
    for attempt in 1 2 3; do
        if GIT_TERMINAL_PROMPT=0 git clone -q "$url" "$dest"; then
            return 0
        fi
        rm -rf "$dest"
        if [ "$attempt" -lt 3 ]; then
            echo "clone failed for $url (attempt $attempt/3); retrying..." >&2
            sleep "$attempt"
        fi
    done
    echo "ERROR: unable to clone required upstream $url after 3 attempts." >&2
    echo "Verify that the repository is public and reachable from the runner." >&2
    return 1
}

# The launcher mirror's token, as an auth header for one git command at a
# time: never in a URL, a remote or .git/config.
launcher_git() {
    local auth
    auth=$(printf 'x-access-token:%s' "$GBLINK_LAUNCHER_TOKEN" | base64 | tr -d '\n')
    git -c "http.https://github.com/.extraheader=AUTHORIZATION: basic $auth" "$@"
}

echo "$PINS" | while read -r repo pin owner; do
    [ -n "$repo" ] || continue
    url="https://github.com/${owner:-GB-Link}/$repo.git"
    if [ "$repo" = gblink-launcher ] && [ ! -d "$SRC/$repo/.git" ]; then
        if [ -z "${GBLINK_LAUNCHER_TOKEN:-}" ] ||
           ! GIT_TERMINAL_PROMPT=0 launcher_git clone -q "$url" "$SRC/$repo"; then
            rm -rf "$SRC/$repo"
            echo "$repo: UNAVAILABLE -- the private mirror needs GBLINK_LAUNCHER_TOKEN; test_launcher will say so"
            continue
        fi
    fi
    if [ ! -d "$SRC/$repo/.git" ] && [ ! -d "$SRC/$repo/src" ]; then
        clone_public_repo "$url" "$SRC/$repo"
    fi
    if [ -z "${GBLINK_SRC:-}" ]; then
        git -C "$SRC/$repo" remote set-url origin "$url"   # an older checkout may be the fork's
        if [ "$repo" = gblink-launcher ]; then
            # The mirror is frozen: its head is the pin, heads run or not.
            if [ -n "${GBLINK_LAUNCHER_TOKEN:-}" ]; then launcher_git -C "$SRC/$repo" fetch -q origin || true; fi
            git -C "$SRC/$repo" checkout -q --detach origin/HEAD
            pin=$(git -C "$SRC/$repo" rev-parse HEAD)
        elif [ -n "${GBLINK_COMPAT_HEADS:-}" ]; then
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
# The launcher test patches the mirror's head: upstream's 94b9a95, the base
# backport/README.md gives.
export GBLINK_LAUNCHER_BASE=origin/HEAD

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


"$ROOT/test/gblink_sim/fetch_librfu.sh" || { echo "librfu: not fetched; the Wireless Adapter parts will skip"; [ -z "${GBLINK_NO_SKIP:-}" ]; }
make -s -C "$ROOT/test/gblink_sim"
# Celio-Server with our backport applied: the crowd and latency tests run
# against it too, so the patch file itself is what is tested.
# The copy is its own repository: under build/ it is inside gbagate's, and a
# bare `git apply` there takes the patch's paths relative to gbagate's root
# and silently applies nothing.
PATCHED="$BUILD/celio-server-patched"
rm -rf "$PATCHED"; mkdir -p "$PATCHED"
(cd "$SRC/Celio-Server" && git archive HEAD) | tar -x -C "$PATCHED"
git -C "$PATCHED" init -q
git -C "$PATCHED" add -A
git -C "$PATCHED" -c user.name=gbagate -c user.email=gbagate@localhost commit -qm upstream
for p in "$ROOT"/backport/Celio-Link-Server/*.patch; do
    git -C "$PATCHED" apply "$p" || { echo "$p no longer applies to Celio-Server"; exit 1; }
done
if git -C "$PATCHED" diff --quiet; then
    echo "backport/Celio-Link-Server changed nothing in Celio-Server"; exit 1
fi
# Upstream's own tests (vitest, with the ones the patches add) and a strict
# type-check of the patched tree, with upstream's locked dependencies.
(cd "$PATCHED" && npm ci --silent --no-audit --no-fund >/dev/null \
    && npm i --silent --no-save --no-audit --no-fund typescript@5.6.3 @types/node@22.10.2 >/dev/null \
    && npx tsc --noEmit -p . && npx vitest run --reporter=dot) \
    || { echo "Celio-Server with backport/Celio-Link-Server: its own tests or type-check FAILED"; exit 1; }
node "$HERE/build.mjs" "$BUILD" "$SRC/gblink-netplay-bridge" "$SRC/Celio-Client" "$SRC/Celio-Server" "$PATCHED"

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
