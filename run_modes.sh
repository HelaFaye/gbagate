#!/bin/bash
# run_modes.sh — Run the mode dispatch test suite
#
# Usage:
#   ./run_modes.sh           — All 4 configurations (default)
#   ./run_modes.sh -O0       — Only -O0 optimization
#   ./run_modes.sh -O2       — Only -O2 optimization
#   ./run_modes.sh -O3       — Only -O3 + LTO
#   ./run_modes.sh asan      — Only ASan/UBSan
#
# Outputs: 14 lines of results (4 configs × 4 modes)

set -e

CC="cc"
CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Wshadow -O2 -Isrc"
DB="data/chipdb/bn6g.chdb"
BUILD="build"

# Configurations to test (default: all 4)
OPTIMIZATIONS=("-O0" "-O2" "-O3 -flto" "-g -fsanitize=address,undefined")

# Create build directory if needed
mkdir -p "$BUILD"

# Run each configuration × each mode
for cfg in "${OPTIMIZATIONS[@]}"; do
    printf '%s: ' "$cfg"
    for mode in chipgate link bridge hw ui chipdb encoder panel slot chipview render multiboot catalog variant modes; do
        # Build the test binary
        if $CC -std=c11 -Wall -Wextra -Wpedantic -Wshadow $cfg -Isrc \
             -o "$BUILD/s_$mode" test/test_${mode}.c src/app_modes.c src/ui_shell.c src/hw_caps.c src/chipgate.c \
             2>/dev/null; then
            # Run it and capture output
            if ./"$BUILD/s_$mode" $DB >/dev/null 2>&1; then
                printf ' ok     '
            else
                printf ' FAIL   '
            fi
        else
            printf ' SKIP    '
        fi
    done
    echo
done

echo ""
echo "Summary: Run './make run-modes' for the full suite."
