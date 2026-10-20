#!/usr/bin/env bash
#
# fetch_librfu.sh -- pret's decompiled librfu, for the wireless adapter tests.
#
# Three files of pret/pokefirered (the STWI command layer, the SIO interrupt
# handler and the Sio32ID exchange) at a pinned commit, into build/ref. They
# are not part of gbagate and are not redistributed with it; the simulator
# builds them in when they are here (see Makefile) and the tests that need
# them skip when they are not.
#
#   test/gblink_sim/fetch_librfu.sh [dest]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$ROOT/build/ref/pokefirered}"
COMMIT=037335f4c725d7c9aecdac87066f2002b4bd7e14
[ -f "$DEST/src/librfu_stwi.c" ] && exit 0
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
git clone -q --filter=blob:none --no-checkout https://github.com/pret/pokefirered "$DEST"
git -C "$DEST" sparse-checkout set --no-cone /include/librfu.h /src/librfu_stwi.c /src/librfu_intr.c /src/librfu_sio32id.c
git -C "$DEST" checkout -q "$COMMIT"
