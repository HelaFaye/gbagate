#!/data/data/com.termux/files/usr/bin/bash
#
# build-mgba-termux.sh -- build mGBA in Termux on Android (aarch64).
#
# Builds mgba-headless, which is the target that matters here: no X11, no SDL,
# no display at all, but full Lua scripting and the ability to load a savestate
# made by the normal mGBA Android app. That combination lets you dump a running
# game's RAM entirely from a phone.
#
# Optionally also builds the SDL frontend, which needs Termux-X11 to be useful.
#
#   ./build-mgba-termux.sh              # headless only, the fast path
#   ./build-mgba-termux.sh --sdl        # also build the interactive frontend
#   ./build-mgba-termux.sh --jobs 4     # limit parallelism on a hot phone
#
# The flag set here was validated by an actual configure-and-build of mGBA
# 0.11-dev (commit 438c773) on aarch64 Linux. Termux package *names* are the
# part that could not be validated off-device, so every dependency is optional
# and probed: anything missing turns its feature off rather than failing.

set -u

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SRC_DIR="${SRC_DIR:-$HOME/mgba-src}"
BUILD_DIR="$SRC_DIR/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
WANT_SDL=0

while [ $# -gt 0 ]; do
	case "$1" in
		--sdl)   WANT_SDL=1; shift ;;
		--jobs)  JOBS="$2"; shift 2 ;;
		--src)   SRC_DIR="$2"; BUILD_DIR="$2/build"; shift 2 ;;
		-h|--help) sed -n '3,25p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- packages ---

say "installing build tools"
pkg update -y >/dev/null 2>&1 || warn "pkg update failed; continuing"
pkg install -y git cmake ninja clang binutils pkg-config make >/dev/null 2>&1 \
	|| die "could not install the core toolchain -- try 'termux-change-repo' and rerun"

for t in git cmake ninja clang pkg-config; do
	command -v "$t" >/dev/null 2>&1 || die "$t missing after install"
done

# Optional. Each one that fails simply disables its feature below.
say "installing optional libraries"
OPTIONAL="lua54 libpng zlib libzip sqlite libedit"
[ "$WANT_SDL" = 1 ] && OPTIONAL="$OPTIONAL sdl2"
for p in $OPTIONAL; do
	if pkg install -y "$p" >/dev/null 2>&1; then
		printf '  ok    %s\n' "$p"
	else
		printf '  skip  %s (not available)\n' "$p"
	fi
done

# --------------------------------------------------------------- detection ---
# Termux package names drift, so probe for what actually landed rather than
# assuming. Lua is the one that genuinely matters: no Lua, no scripting, and
# scripting is the whole point of this build.

find_lua() {
	for pc in lua54 lua5.4 lua-5.4 lua53 lua5.3 lua; do
		if pkg-config --exists "$pc" 2>/dev/null; then
			LUA_INC="$(pkg-config --variable=includedir "$pc" 2>/dev/null)"
			LUA_LIBDIR="$(pkg-config --variable=libdir "$pc" 2>/dev/null)"
			LUA_PC="$pc"
			return 0
		fi
	done
	for h in "$PREFIX/include/lua.h" "$PREFIX/include/lua5.4/lua.h" "$PREFIX/include/lua54/lua.h"; do
		if [ -f "$h" ]; then
			LUA_INC="$(dirname "$h")"
			LUA_LIBDIR="$PREFIX/lib"
			LUA_PC=""
			return 0
		fi
	done
	return 1
}

LUA_ARGS=""
if find_lua; then
	say "found Lua headers in $LUA_INC"
	LUA_LIB="$(ls "$LUA_LIBDIR"/liblua*.so "$LUA_LIBDIR"/liblua*.a 2>/dev/null | head -1)"
	LUA_ARGS="-DUSE_LUA=ON"
	[ -n "$LUA_INC" ] && LUA_ARGS="$LUA_ARGS -DLUA_INCLUDE_DIR=$LUA_INC"
	[ -n "${LUA_LIB:-}" ] && LUA_ARGS="$LUA_ARGS -DLUA_LIBRARY=$LUA_LIB"
else
	warn "no Lua found -- building WITHOUT scripting."
	warn "the RAM-dump workflow needs it; try 'pkg install lua54' and rerun."
	LUA_ARGS="-DUSE_LUA=OFF -DENABLE_SCRIPTING=OFF"
fi

have_pc() { pkg-config --exists "$1" 2>/dev/null; }
onoff()   { if have_pc "$1"; then echo ON; else echo OFF; fi; }

USE_LIBZIP="$(onoff libzip)"
USE_SQLITE="$(onoff sqlite3)"
USE_PNG="$(onoff libpng)"

# ------------------------------------------------------------------ source ---

if [ -d "$SRC_DIR/.git" ]; then
	say "updating existing checkout in $SRC_DIR"
	git -C "$SRC_DIR" fetch --depth 1 origin master >/dev/null 2>&1 \
		&& git -C "$SRC_DIR" reset --hard origin/master >/dev/null 2>&1 \
		|| warn "update failed; building whatever is checked out"
else
	say "cloning mGBA into $SRC_DIR"
	git clone --depth 1 https://github.com/mgba-emu/mgba.git "$SRC_DIR" \
		|| die "clone failed"
fi

# ---------------------------------------------------------------- configure ---
# Everything not needed for headless scripted analysis is off. FFmpeg and
# Discord in particular pull in large dependency trees for no benefit here.

say "configuring"
rm -rf "$BUILD_DIR"
# shellcheck disable=SC2086
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" \
	-DBUILD_HEADLESS=ON \
	-DENABLE_SCRIPTING=ON \
	$LUA_ARGS \
	-DBUILD_QT=OFF \
	-DBUILD_SDL=$( [ "$WANT_SDL" = 1 ] && echo ON || echo OFF ) \
	-DUSE_FFMPEG=OFF \
	-DUSE_DISCORD_RPC=OFF \
	-DUSE_ELF=OFF \
	-DUSE_FREETYPE=OFF \
	-DUSE_MINIZIP=OFF \
	-DUSE_LIBZIP=$USE_LIBZIP \
	-DUSE_SQLITE3=$USE_SQLITE \
	-DUSE_PNG=$USE_PNG \
	-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF \
	|| die "cmake configure failed -- see the output above"

# -------------------------------------------------------------------- build ---

say "building with $JOBS jobs (a few minutes; the phone will get warm)"
cmake --build "$BUILD_DIR" -j "$JOBS" || die "build failed"

[ -x "$BUILD_DIR/mgba-headless" ] || die "mgba-headless was not produced"

say "installing to $PREFIX"
cmake --install "$BUILD_DIR" >/dev/null 2>&1 || {
	warn "install failed; copying binaries by hand"
	cp "$BUILD_DIR/mgba-headless" "$PREFIX/bin/" 2>/dev/null
	[ "$WANT_SDL" = 1 ] && cp "$BUILD_DIR/mgba" "$PREFIX/bin/" 2>/dev/null
	cp "$BUILD_DIR"/libmgba.so* "$PREFIX/lib/" 2>/dev/null
}

say "done"
"$BUILD_DIR/mgba-headless" --version 2>&1 | head -2
echo
echo "Scripting: $( [ "$LUA_ARGS" = "-DUSE_LUA=OFF -DENABLE_SCRIPTING=OFF" ] && echo DISABLED || echo enabled )"
echo
echo "Next:"
echo "  1. Play chip_gate_homebrew.gba in the normal mGBA Android app,"
echo "     select your game in the menu, then make a savestate."
echo "  2. Copy the .ss file and the ROM somewhere Termux can read"
echo "     (run 'termux-setup-storage' once, then look in ~/storage/shared)."
echo "  3. mgba-headless -t state.ss1 --script dump_ram.lua chip_gate_homebrew.gba"
echo
echo "That writes ewram.bin and iwram.bin without needing a display."
