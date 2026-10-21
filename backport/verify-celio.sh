#!/usr/bin/env bash
#
# verify-celio.sh -- check backport/Celio-Server against Celio-Client, old and
# new, through gbagate's simulator.
#
#   backport/verify-celio.sh [--unpatched] [--client-suite]
#
# Needs the checkouts `make compat` makes (build/gblink_compat/src). Applies
# the series to Celio-Server at the commit run.sh pins, bundles it with
# Celio-Client's head and with the client from before b057315 (the one the
# unpatched server serves), and runs test_celio.mjs -- two adapters relayed
# through the server -- for each pairing: new/new, old/old, new/old, old/new.
#
# --client-suite also runs Celio-Client's own tests (tests/, vitest), which
# start the server from a checkout, against the patched server. They need
# port 443 (root, or net.ipv4.ip_unprivileged_port_start=0) and npm.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
COMPAT="$ROOT/build/gblink_compat"
SRC="$COMPAT/src"
SERVER_BASE=9f02d3e6cba624f51fcac9e436be153e1e6e4119
PATCHED=1 SUITE=0
for a in "$@"; do
    case "$a" in --unpatched) PATCHED=0 ;; --client-suite) SUITE=1 ;; esac
done
WORK="$ROOT/build/backport/celio-$([ $PATCHED = 1 ] && echo patched || echo unpatched)"
[ -d "$SRC/Celio-Server" ] && [ -d "$SRC/Celio-Client-pre-ack/src" ] || {
    echo "run make compat first (it clones Celio-Server and Celio-Client)"; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
git -C "$SRC/Celio-Server" worktree prune
git -C "$SRC/Celio-Server" worktree add -q --detach "$WORK/server" "$SERVER_BASE" || exit 1
if [ $PATCHED = 1 ]; then
    git -C "$WORK/server" -c user.name=verify -c user.email=verify@localhost \
        am -q "$HERE"/Celio-Server/*.patch || { echo "the series does not apply"; exit 1; }
fi

make -s -C "$ROOT/test/gblink_sim" >/dev/null || exit 1
for v in new old; do
    client="$SRC/Celio-Client"; [ $v = old ] && client="$SRC/Celio-Client-pre-ack"
    mkdir -p "$WORK/$v"
    ln -sfn "$COMPAT/node_modules" "$WORK/$v/node_modules"
    node "$ROOT/test/gblink_compat/build.mjs" "$WORK/$v" "$SRC/gblink-netplay-bridge" \
        "$client" "$WORK/server" >/dev/null || exit 1
done

echo "Celio-Server $SERVER_BASE $([ $PATCHED = 1 ] && echo "with backport/Celio-Server" || echo unpatched);" \
     "clients: new $(git -C "$SRC/Celio-Client" log -1 --format=%h), old $(git -C "$SRC/Celio-Client-pre-ack" log -1 --format=%h)"
fail=0
for pair in "new new" "old old" "new old" "old new"; do
    set -- $pair
    if node "$ROOT/test/gblink_compat/test_celio.mjs" "$WORK/$1" "$WORK/$2" 2>&1 | tail -1 | grep -q 'frames delivered'; then
        echo "  ok    A $1, B $2"
    else
        echo "  FAIL  A $1, B $2"; fail=1
    fi
done

if [ $SUITE = 1 ]; then
    git -C "$SRC/Celio-Client" worktree add -q --detach "$WORK/client" HEAD || exit 1
    # The suite starts the server from its author's checkout; point it at ours.
    sed -i 's|const externalDir = "[^"]*";|const externalDir = process.env.CELIO_SERVER_DIR!;|' \
        "$WORK/client/tests/scripts/serverStartAndTearDown.ts"
    (cd "$WORK/server" && npm install --silent --no-audit --no-fund >/dev/null) || exit 1
    (cd "$WORK/client" && npm install --silent --no-audit --no-fund >/dev/null) || exit 1
    out=$(cd "$WORK/client" && CELIO_SERVER_DIR="$WORK/server" npx vitest run 2>&1)
    printf '%s\n' "$out" | grep -E '^ +(Tests|Test Files) '
    printf '%s\n' "$out" | grep -E '^ FAIL ' | sort -u
    printf '%s\n' "$out" | grep -qE 'Tests +[0-9]+ passed' && ! printf '%s\n' "$out" | grep -q 'failed' || fail=1
fi
exit $fail
