cmake_minimum_required(VERSION 3.16)
#
# Native ESP-IDF entry point for the QEMU builds.
#
# Hardware images are built through pio/platformio.ini (`pio run -e gate`).
# PlatformIO synthesizes a project file internally, so the repo never needed a
# top-level CMakeLists for that path. qemu/build.sh, however, drives raw
# `idf.py` from the repo root, and idf.py requires a project-level CMakeLists
# here. This file is that entry point; it mirrors the -D flags the qemu-* envs
# in platformio.ini set, so build.sh only has to pass -D VARIANT.

if(NOT DEFINED VARIANT)
  set(VARIANT gate)
endif()

# The single firmware component lives in pio/src (component name "src").
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/pio")

# Include path for headers in pio/include and pio/src that ESP-IDF doesn't add automatically.
add_compile_options(-I"${CMAKE_CURRENT_LIST_DIR}/pio/include" -I"${CMAKE_CURRENT_LIST_DIR}/pio/src")
set(COMPONENTS src)

# These images are always for the emulator: no pins, no USB-OTG. The component
# CMakeLists reads GBAGATE_QEMU to swap the PHY/USB glue for the UART transport.
set(GBAGATE_QEMU 1)

# Translate the variant into the compile-time macros the C sources expect,
# mirroring the -D build_flags each qemu-<variant> env sets in platformio.ini.
# app_main.c #errors without VARIANT/GBAGATE_VARIANT_NAME, and the gate/full
# code paths need GBAGATE_HAS_CHIPDB.
if(VARIANT STREQUAL "link")
  add_compile_definitions(VARIANT=VARIANT_LINK GBAGATE_VARIANT_NAME="link")
elseif(VARIANT STREQUAL "gate")
  add_compile_definitions(VARIANT=VARIANT_GATE GBAGATE_VARIANT_NAME="gate" GBAGATE_HAS_CHIPDB=1)
elseif(VARIANT STREQUAL "full")
  add_compile_definitions(VARIANT=VARIANT_FULL GBAGATE_VARIANT_NAME="full" GBAGATE_HAS_CHIPDB=1)
else()
  message(FATAL_ERROR "VARIANT must be link, gate or full (got '${VARIANT}')")
endif()
add_compile_definitions(GBAGATE_QEMU=1)

# Which UART carries the framebuffer out and the keystrokes back
# (port/esp32s3/st7735_qemu.c and input_qemu.c share one port).
#
# 2 is the right answer and the default: UART0 stays the console and UART1 stays
# link_proto, so nothing is multiplexed. But not every build of Espressif's QEMU
# attaches a third UART to the esp32s3 machine, and one that does not silently
# discards everything written to UART2 -- the chardev's socket still opens and
# still accepts a client, so the symptom is a viewer that connects, reports no
# error and never draws. qemu/uart2check.sh decides which QEMU you have.
#
# When the answer is "only two", this moves the tap onto a UART that does exist:
#
#   0  share the console. Keeps link_proto on its own UART, so the link is
#      still tested; the cost is that log lines land in the framebuffer stream.
#      That is survivable because the stream is self-delimiting -- viewer.py
#      resynchronises on the FBUF magic and validates the geometry -- and
#      because a whole frame goes out in one uart_write_bytes() call, so a log
#      line can only ever land *between* frames, not inside one.
#   1  take link_proto's UART. A clean, uncontaminated stream, but there is then
#      no link -- only useful with lab.sh --no-mgba, to look at the screen.
#
# Must match the -serial ordering qemu/run.sh and qemu/lab.sh set up; both take
# --aux-uart for that, and the value is logged at boot so a mismatch is visible.
if(NOT DEFINED QEMU_AUX_UART)
  set(QEMU_AUX_UART 2)
endif()
if(NOT "${QEMU_AUX_UART}" MATCHES "^[012]$")
  message(FATAL_ERROR "QEMU_AUX_UART must be 0, 1 or 2 (got '${QEMU_AUX_UART}')")
endif()
add_compile_definitions(QEMU_AUX_UART=${QEMU_AUX_UART})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbagate)
