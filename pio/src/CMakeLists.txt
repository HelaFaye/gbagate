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

# Applets: applets/*.c, globbed for the same reason as src/. Which of them are
# in the menu is applets/applets.def's business, not the build's: an applet
# that is compiled but not listed is dropped by --gc-sections.
file(GLOB APPLETS ${CMAKE_CURRENT_LIST_DIR}/../../applets/*.c)
list(APPEND PORTABLE ${APPLETS})

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
        ../../port/esp32s3/sd_esp32s3.c
        ../../port/esp32s3/gba_link_esp32s3.c
        ../../port/esp32s3/transport_esp32s3.c)
    # Direct GPIO or an I2C expander, decided by the board rather than the
    # variant: a Supermini has too few pins for ten switches, a 44-pin devkit
    # has plenty. Only one is built, because both define input_active().
    #
    # This was a pair of generator expressions inside SRCS. ESP-IDF resolves
    # SRCS to absolute paths at configure time, before generator expressions
    # are evaluated, so it looked for a file literally named
    # "port/esp32s3/buttons_esp32s3.c>" and no hardware image could configure.
    # A plain if() is evaluated when the list is built, which is what was meant.
    if(BUTTONS_ON_EXPANDER)
        list(APPEND SRCS ../../port/esp32s3/buttons_mcp.c)
    else()
        list(APPEND SRCS ../../port/esp32s3/buttons_esp32s3.c)
    endif()
    # The GB-Link personality, in every hardware image (FEAT_GBLINK): GB-Link's
    # firmware from third_party/, the Zephyr shim it runs on, and this board's
    # USB, PHY and hardware glue. Listed rather than globbed for the same reason
    # as APP: third_party/ also holds files that must not be built here (their
    # main.cpp is compiled via gblink_glue.cpp, renamed).
    set(GBL ../../third_party/gblink-firmware/src)
    list(APPEND SRCS
        ${GBL}/layers/packetLayer.cpp
        ${GBL}/layers/transport.cpp
        ${GBL}/callbacks/blockCommand.cpp
        ${GBL}/callbacks/blockCommandRequestCommand.cpp
        ${GBL}/callbacks/blockRequestCommand.cpp
        ${GBL}/callbacks/emptyCommand.cpp
        ${GBL}/callbacks/moveCommand.cpp
        ${GBL}/callbacks/readyCloseLinkCommand.cpp
        ${GBL}/callbacks/readyExitStandbyCommand.cpp
        ${GBL}/callbacks/sendDummyCommand.cpp
        ${GBL}/callbacks/sendLinkTypeCommand.cpp
        ${GBL}/callbacks/usbLinkCommand.cpp
        ${GBL}/payloads/linkPlayer.c
        ${GBL}/payloads/trainerCard.c
        ${GBL}/payloads/pokemon.cpp
        ${GBL}/payloads/mail.cpp
        ${GBL}/sections/tradeSetup.cpp
        ${GBL}/sections/tradeConnection.cpp
        ${GBL}/sections/tradeDisconnected.cpp
        ${GBL}/sections/tradeLounge.cpp
        ${GBL}/sections/usbSection.cpp
        ${GBL}/sections/awProtocolSection.cpp
        ${GBL}/sections/eReaderProtocolSection.cpp
        ${GBL}/sections/battleChipGateSection.cpp
        ${GBL}/sections/rfuProtocolSection.cpp
        ${GBL}/module/link.cpp
        ${GBL}/module/emu.cpp
        ${GBL}/module/gb.cpp
        ${GBL}/module/advanceWars.cpp
        ${GBL}/module/eReader.cpp
        ${GBL}/module/battleChipGate.cpp
        ${GBL}/module/rfuWireless.cpp
        ${GBL}/persist.cpp
        ${GBL}/linkWrongPinWatchdog.c
        ../../port/gblink/zshim_common.c
        ../../port/gblink/zshim_freertos.c
        ../../port/gblink/link_layer_core.c
        ../../port/gblink/rfu_link_core.c
        ../../port/gblink/serial_layer.cpp
        ../../port/gblink/hardware_port.cpp
        ../../port/gblink/gblink_glue.cpp
        ../../port/gblink/gbagate_ext.cpp
        ../../port/gblink/peer_glue.cpp
        ../../port/esp32s3/peer_espnow_esp32s3.c
        ../../port/esp32s3/usb_gblink_esp32s3.c
        ../../port/esp32s3/gblink_hw_esp32s3.c
        ../../port/esp32s3/gblink_gbspi_esp32s3.c
        ../../port/esp32s3/rfu_link_esp32s3.c
        ../../port/esp32s3/bridge_uart_esp32s3.c)
    if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full")
        list(APPEND SRCS ../../port/esp32s3/usb_msc_esp32s3.c)
    endif()
    # Multiboot's SPI master. The mini image has Send Program too (from its SD
    # card) but was left out here, so its Send Program reported "unavailable".
    if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full" OR VARIANT STREQUAL "mini")
        list(APPEND SRCS ../../port/esp32s3/mb_spi_esp32s3.c)
    endif()
    # The expander only exists on hardware; QEMU has no I2C at all.
    if(VARIANT STREQUAL "full")
        list(APPEND SRCS ../../port/esp32s3/i2c_esp32s3.c
                         ../../port/esp32s3/ir_esp32s3.c)
    endif()
endif()

set(EMBEDS "")
# mini has the gate and all six databases too (GBAGATE_HAS_CHIPDB=1); it was
# missing here, so its image referenced databases it never embedded.
if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full" OR VARIANT STREQUAL "mini")
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
    list(APPEND REQS fatfs wear_levelling esp_partition)
endif()
# Every hardware image is a GB-Link adapter, so every one needs TinyUSB, and
# the RMT driver for the status LED.
if(NOT GBAGATE_QEMU)
    list(APPEND REQS espressif__esp_tinyusb esp_driver_rmt)
    # transport_esp32s3.c's ESP-NOW backend. Missing from the list since the
    # file was written, so it had never compiled.
    list(APPEND REQS esp_wifi esp_netif esp_event)
endif()

# The chip reader's expander sits on I2C, and IDF 6.0 split that out too.
if(NOT GBAGATE_QEMU AND VARIANT STREQUAL "full")
    list(APPEND REQS esp_driver_i2c)
endif()

set(INCS "." "../../src" "../../port/esp32s3")
if(NOT GBAGATE_QEMU)
    # port/gblink first: its zephyr/ and hardware/ shims must win over any
    # same-named header elsewhere on the path.
    list(APPEND INCS "../../port/gblink" "../../third_party/gblink-firmware/src")
endif()

# WHOLE_ARCHIVE, because gate_app.c and app_main.c give weak defaults for
# drivers only some images build (chip_reader_probe, ir_*, msc_storage_init,
# mb_spi_transfer, ...). ESP-IDF links a component as a static archive, and a
# linker never extracts an archive member to satisfy a symbol that already
# has a definition -- weak or not. So the real driver was silently dropped
# whenever the weak default was found first: the full image's infrared and
# chip reader were never linked, and its binary matched the gate image's.
# Linking the whole archive lets the strong definitions win, which is what
# the weak defaults were written to rely on.
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCS}
    EMBED_FILES ${EMBEDS}
    REQUIRES ${REQS}
    LDFRAGMENTS linker.lf
    WHOLE_ARCHIVE
)

if(NOT GBAGATE_QEMU)
    # GB-Link's firmware is C++20 (std::span, ranges) and builds with
    # CONFIG_SECTIONS_USE_MASTER_MODE=y upstream (prj.conf).
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_SECTIONS_USE_MASTER_MODE=1)
    # GCC 14 reads RfuCore's RECV_DATA path (rfuProtocol.hpp) as using a
    # pointer the packet queue's read() leaves unset; it is unset only when
    # read() returns length 0, and then the loop over it does not run.
    # Upstream's file, so the warning goes rather than the code changing.
    set_source_files_properties(../../third_party/gblink-firmware/src/sections/rfuProtocolSection.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()
