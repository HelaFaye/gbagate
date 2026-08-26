# Sources are selected by VARIANT, and by whether this is a QEMU build.
# platformio.ini sets both.

set(PORTABLE
    ../../src/variant.c
    ../../src/link_proto.c
    ../../src/link_session.c
    ../../src/link_bridge.c
    ../../src/hw_caps.c
    ../../src/ui_list.c
    ../../src/ui_font.c
    ../../src/ui_render.c
    ../../src/st7735_panel.c
    ../../src/input_encoder.c
)

set(APP
    app_main.c
    tasks.c
    gate_app.c
    link_source.c
)

# The display glue is chosen below by whether this is a QEMU build: under
# emulation there is no SPI panel, so st7735_qemu.c stands in for it and streams
# the framebuffer out a UART (the same idea as transport_uart.c for the PHY).
set(GLUE "")

set(GATE_SRCS
    ../../src/chipgate.c
    ../../src/chipdb.c
    ../../src/ui_chipview.c
    ../../src/ui_shell.c
    ../../src/rom_catalog.c
    ../../src/gba_multiboot.c
)

set(FULL_SRCS
    ../../src/chip_reader.c
    ../../src/chip_slot.c
    ../../src/nrf24_probe.c
)

set(SRCS ${PORTABLE} ${APP} ${GLUE})

if(VARIANT STREQUAL "gate" OR VARIANT STREQUAL "full")
    list(APPEND SRCS ${GATE_SRCS})
endif()
if(VARIANT STREQUAL "full")
    list(APPEND SRCS ${FULL_SRCS})
endif()

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
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "../../src" "../../port/esp32s3"
    EMBED_FILES ${EMBEDS}
    REQUIRES driver esp_driver_gpio esp_driver_spi esp_driver_uart
             esp_timer nvs_flash vfs
)
