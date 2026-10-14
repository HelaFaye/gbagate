#!/usr/bin/env bash
#
# lint.sh -- run a compiler over the firmware sources, on the host.
#
# pio/src and port/esp32s3 are the quarter of the project no test suite
# compiles, and every defect the audit found lived there. A missing forward
# declaration in gate_app.c reached an ESP-IDF build with -Werror before anyone
# noticed, because no compiler had ever looked at the file.
#
# qemu/stubs/ is just enough ESP-IDF for a host compiler to parse these. It
# emulates nothing and links nothing; it catches implicit declarations,
# conflicting prototypes, wrong argument counts and missing includes -- pure
# compiler work that needs no hardware and takes a second.
#
# The first run found four real errors that the ESP-IDF build would have found
# one at a time, several minutes apart.
cd "$(dirname "$0")/.." || exit 1
COMMON="-std=c11 -Wall -Wextra -Wshadow -Werror -fsyntax-only
        -Iqemu/stubs -Isrc -Iport/esp32s3 -Ipio/src
        -DVARIANT=VARIANT_GATE -DGBAGATE_VARIANT_NAME=\"gate\"
        -DGBAGATE_HAS_CHIPDB=1"

# Files the stubs cover. The rest of port/esp32s3 pulls in SPI, TinyUSB and
# WiFi headers; stubbing those faithfully enough to be worth anything is a
# bigger job than it sounds, and they are on the bench list anyway.
QEMU_FILES="pio/src/gate_app.c
            pio/src/link_source.c
            pio/src/tasks.c
            port/esp32s3/transport_uart.c
            port/esp32s3/input_qemu.c"

# The same sources with GBAGATE_QEMU off take different #ifdef branches, so a
# QEMU-only pass leaves half the code unchecked -- and hardware images are the
# ones that eventually have to work.
HW_FILES="pio/src/gate_app.c
          pio/src/link_source.c
          pio/src/tasks.c
          port/esp32s3/usb_msc_esp32s3.c
          port/esp32s3/nvs_store.c
          port/esp32s3/st7735_esp32s3.c
          port/esp32s3/ir_esp32s3.c
          port/esp32s3/buttons_esp32s3.c
          port/esp32s3/sd_esp32s3.c
          port/esp32s3/buttons_mcp.c
          port/esp32s3/gba_link_esp32s3.c
          port/esp32s3/transport_esp32s3.c
          port/esp32s3/i2c_esp32s3.c
          port/esp32s3/mb_spi_esp32s3.c"

fail=0

check() {
    local label="$1" extra="$2"; shift 2
    for f in "$@"; do
        printf '%-30s %-8s ' "$f" "$label"
        if cc $COMMON $extra "$f" 2>/tmp/lint_err; then
            echo ok
        else
            echo FAIL
            sed 's/^/    /' /tmp/lint_err | head -12
            fail=1
        fi
    done
}

check qemu "-DGBAGATE_QEMU=1" $QEMU_FILES
check hardware "" $HW_FILES

# The component CMakeLists as ESP-IDF evaluates it, including the script-mode
# pass that forbids things a normal configure allows.
if ./qemu/cmakecheck.sh > /tmp/cm.out 2>&1; then
    printf '%-39s ok\n' "cmake script mode"
else
    printf '%-39s FAIL\n' "cmake script mode"
    sed 's/^/    /' /tmp/cm.out
    fail=1
fi

# Sources that are in no build at all. The linker names the missing symbols,
# not the missing file.
if python3 qemu/srccheck.py > /tmp/src.out 2>&1; then
    printf '%-39s %s\n' "source coverage" "$(tail -1 /tmp/src.out | sed 's/^ *//')"
else
    printf '%-39s FAIL\n' "source coverage"
    sed 's/^/    /' /tmp/src.out
    fail=1
fi

# Static allocations that will not fit in DRAM. A 256 KB array in a function
# overflowed dram0_0_seg by 3 KB and the linker's message named the segment,
# not the array.
if python3 qemu/bsscheck.py > /tmp/bss.out 2>&1; then
    printf '%-39s %s\n' "DRAM budget" "$(tail -1 /tmp/bss.out | sed 's/^ *//')"
else
    printf '%-39s FAIL\n' "DRAM budget"
    sed 's/^/    /' /tmp/bss.out
    fail=1
fi

# Capabilities the menu gates on that nothing sets: the mode silently vanishes.
if python3 qemu/capcheck.py > /tmp/capcheck.out 2>&1; then
    printf '%-39s ok\n' "capability coverage"
else
    printf '%-39s FAIL\n' "capability coverage"
    sed 's/^/    /' /tmp/capcheck.out
    fail=1
fi

exit $fail
