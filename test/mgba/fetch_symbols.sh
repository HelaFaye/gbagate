#!/bin/sh
# pret/pokeemerald's linker symbols for the retail ROM it rebuilds, from
# its `symbols` branch, pinned. Into build/ref, never committed.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST="$ROOT/build/ref/pokeemerald.sym"
PIN=dba968c67d85caf9595abe12a51ff739d4dc5937
[ -f "$DEST" ] && exit 0
mkdir -p "$ROOT/build/ref"
T=$(mktemp -d)
git -C "$T" init -q
git -C "$T" fetch -q --depth 1 https://github.com/pret/pokeemerald "$PIN"
git -C "$T" show FETCH_HEAD:pokeemerald.sym > "$DEST"
rm -rf "$T"
echo "pokeemerald.sym: $(wc -l < "$DEST") symbols at $PIN"
