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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbagate)
