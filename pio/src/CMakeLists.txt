# Sources are selected by VARIANT, and by whether this is a QEMU build.
# platformio.ini sets both.

# Portable sources: everything in src/, globbed.
#
# This used to be a hand-written list, and three separate builds failed because
# a file was added to src/ and not to the list. The linker's message names the
# missing symbols, not the missing file, so each time it read as a code fault
# rather than a build-config one.
#
# Globbing is usually poor practice because it hides what is being built. Here
# it is the safer choice, and specifically because of what src/ is: portable C
# with no hardware dependencies, no variant-specific behaviour, and nothing
# that is wrong to compile. Anything unreferenced is dropped by --gc-sections,
# so the cost of compiling a file this image does not use is build time and
# nothing else.
#
# CONFIGURE_DEPENDS makes CMake re-run the glob when the directory changes, so
# adding a file does not need a manual reconfigure.
file(GLOB PORTABLE DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../../src/*.c)

if(PORTABLE STREQUAL "")
    message(FATAL_ERROR "no sources found in src/ -- wrong path?")
endif()

# The application layer. Not globbed: pio/src holds the CMakeLists and the
# generated files too, so a glob here would sweep up things that are not
# sources.
set(APP
    ../../port/esp32s3/nvs_store.c
    app_main.c
    tasks.c
    gate_app.c
    link_source.c
)

# The variant-gated sources stay explicit, because those genuinely must not be
# compiled everywhere: they need hardware that may be absent.
set(SRCS ${PORTABLE} ${APP})

# QEMU has no pins and no USB-OTG, so the PHY and the CDC/MSC glue are left out
# entirely rather than compiled and left to fail at runtime. UART stands in.
if(GBAGATE_QEMU)
    list(APPEND SRCS ../../port/esp32s3/transport_uart.c
                     ../../port/esp32s3/st7735_qemu.c
                     ../../port/esp32s3/input_qemu.c)
else()
    list(APPEND SRCS
        ../../port/esp32s3/st7735_esp32s3.c
        ../../port/esp32s3/gba_link_esp32s3.c
        ../../port/esp32s3/transport_esp32s3.c)
    if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full")
        list(APPEND SRCS
            ../../port/esp32s3/usb_msc_esp32s3.c
            ../../port/esp32s3/mb_spi_esp32s3.c)
    endif()
    # The expander only exists on hardware; QEMU has no I2C at all.
    if(VARIANT STREQUAL "full")
        list(APPEND SRCS ../../port/esp32s3/i2c_esp32s3.c)
    endif()
endif()

set(EMBEDS "")
if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full")
    set(EMBEDS
        ../../data/chipdb/bn4r.chdb
        ../../data/chipdb/bn4b.chdb
        ../../data/chipdb/bn5p.chdb
        ../../data/chipdb/bn5c.chdb
        ../../data/chipdb/bn6g.chdb
        ../../data/chipdb/bn6f.chdb)
endif()

# IDF 6.0 split gpio / spi_master / uart out of the `driver` meta-component into
# esp_driver_gpio / esp_driver_spi / esp_driver_uart, and `driver` no longer
# re-exports their headers. st7735_esp32s3.c needs driver/gpio.h + driver/
# spi_master.h and transport_uart.c needs driver/uart.h, so require them
# explicitly. These components also exist on IDF 5.x, so the PlatformIO
# hardware build is unaffected.
set(REQS driver esp_driver_gpio esp_driver_spi esp_driver_uart
         esp_timer nvs_flash vfs)

# Storage. usb_msc_esp32s3.c includes esp_vfs_fat.h, wear_levelling.h,
# esp_partition.h and tinyusb.h, and none of those come from the components
# above -- without these the file does not compile at all, which is why the
# mode had never been built rather than merely never run.
#
# esp_tinyusb is a managed component, fetched via idf_component.yml. Under QEMU
# there is no USB peripheral, so none of this is built and none of it is
# required.
if(NOT GBAGATE_QEMU AND (VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full"))
    list(APPEND REQS fatfs wear_levelling esp_partition espressif__esp_tinyusb)
endif()

# The chip reader's expander sits on I2C, and IDF 6.0 split that out too.
if(NOT GBAGATE_QEMU AND VARIANT STREQUAL "full")
    list(APPEND REQS esp_driver_i2c)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "../../src" "../../port/esp32s3"
    EMBED_FILES ${EMBEDS}
    REQUIRES ${REQS}
)
