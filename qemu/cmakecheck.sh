#!/usr/bin/env bash
#
# cmakecheck.sh -- evaluate the component CMakeLists the way ESP-IDF does.
#
# ESP-IDF reads every component CMakeLists twice: once during the real
# configure, and once in *script* mode (cmake -P) to work out which components
# require which. Script mode forbids things that are perfectly legal otherwise,
# and the failure arrives as a wall of CMake output in the middle of a build.
#
# CONFIGURE_DEPENDS is the one that caught us. Running the file under `cmake -P`
# here reproduces that pass in a second, with idf_component_register and friends
# stubbed out so it gets far enough to matter.

set -u
cd "$(dirname "$0")/.." || exit 1

command -v cmake >/dev/null 2>&1 || {
    echo "  skipped: cmake not installed"
    exit 0
}

STUB=$(mktemp /tmp/idfstub.XXXXXX.cmake)
trap 'rm -f "$STUB"' EXIT

# Enough of ESP-IDF's component API for the file to run to the end. The point
# is to exercise CMake's own parsing and its script-mode restrictions, not to
# model the build.
cat > "$STUB" <<'CMAKE'
function(idf_component_register)
endfunction()
function(idf_build_get_property var prop)
    set(${var} "" PARENT_SCOPE)
endfunction()
set(CMAKE_CURRENT_LIST_DIR_ORIG "${CMAKE_CURRENT_LIST_DIR}")
CMAKE

fail=0
for f in pio/src/CMakeLists.txt CMakeLists.txt; do
    [ -f "$f" ] || continue
    printf '%-30s script mode ' "$f"

    # The top-level file calls project(), which script mode cannot do; only the
    # component file is meaningful here.
    case "$f" in
        CMakeLists.txt) echo "skipped (calls project())"; continue ;;
    esac

    out=$(cmake -DVARIANT=gate -DGBAGATE_QEMU=1 \
                -P "$STUB" -P "$f" 2>&1)
    if [ $? -eq 0 ]; then
        echo ok
    else
        echo FAIL
        echo "$out" | sed 's/^/    /' | head -12
        fail=1
    fi
done

exit $fail
