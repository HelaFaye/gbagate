# Vendored: GB-Link/GBLink-Firmware

Upstream:  https://github.com/GB-Link/GBLink-Firmware
Commit:    4aef46a1aff688cc0a68df666858eccb9b5ae291 ("Persist cable selection & add SMA4 EUR support")
Version:   2.2.3 (src/firmware_version.hpp)
Licence:   GPL-3.0 -- see LICENSE in this directory. Everything under this
           directory stays under that licence.

This is the application layer of the GB-Link adapter firmware: the command
dispatcher (`control.hpp`), every mode (trade emulator, GBA link relay, GB SPI
link, GB printer, Advance Wars proxy, e-Reader), the Gen 3 packet layer and the
payload builders. It is compiled into gbagate unchanged wherever possible, on
top of a Zephyr-API shim (`port/gblink/zephyr/`), so the gbagate hardware speaks
exactly the protocol the GB-Link web apps, the netplay bridge and Celio expect.

## Not vendored (hardware-specific; gbagate supplies its own)

| upstream file | replaced by |
|---|---|
| `layers/linkLayer_pio.c`, `linkLayer_bitbang.c`, `masterClock.hpp` | `port/esp32s3/gblink_phy_esp32s3.c` (hardware), `test/gblink_sim/sim_phy.c` (host) |
| `layers/gbLinkLayer.c` | `port/esp32s3/gblink_gbspi_esp32s3.c`, host sim |
| `layers/usbLayer.cpp` (descriptors) | `port/esp32s3/usb_gblink_esp32s3.c` |
| `layers/serialLayer.cpp` | `port/gblink/serial_layer.cpp` (portable, over `src/gblink_frame.c`) |
| `hardware.cpp` | `port/esp32s3/gblink_hw_esp32s3.c`, host sim |
| `rawRelay*`, `pio_firmware/` | not built upstream either |

Files that are present here but rewritten for gbagate are listed, with the
reason for each change, in `PATCHES.md`. The first commit that added this
directory is a byte-for-byte copy of upstream, so `git log -p` on this
directory is the complete record of every change made to it.
