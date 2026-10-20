# Vendored: GB-Link/GBLink-Firmware

Upstream:  https://github.com/GB-Link/GBLink-Firmware
Branch:    Wireless-Adapter (master + beta; the source of the launcher's 2.2.7 beta)
Commit:    1b49b765c350d2ecd7dd4ed79e5428d9848c7443 ("Add Pokemon Emerald French and German profiles to e-Reader")
Version:   2.2.7 (src/firmware_version.hpp)
Earlier:   4aef46a (master, 2.2.3) until gbagate 0.4
Licence:   GPL-3.0 -- see LICENSE in this directory. Everything under this
           directory stays under that licence.

This is the application layer of the GB-Link adapter firmware: the command
dispatcher (`control.hpp`), every mode (trade emulator, GBA link relay, GB SPI
link, GB printer, Advance Wars proxy, e-Reader, Battle Chip Gate, Wireless
Adapter), the Gen 3 packet layer, the RFU protocol core and the payload
builders. It is compiled into gbagate unchanged wherever possible, on
top of a Zephyr-API shim (`port/gblink/zephyr/`), so the gbagate hardware speaks
exactly the protocol the GB-Link web apps, the netplay bridge and Celio expect.

## Not vendored (hardware-specific; gbagate supplies its own)

| upstream file | replaced by |
|---|---|
| `layers/linkLayer_pio.c`, `linkLayer_bitbang.c`, `masterClock.hpp` | `port/gblink/link_layer_core.c` over a `gbl_phy_driver_t`: `port/esp32s3/gba_link_esp32s3.c` (hardware), `test/gblink_sim/sim_gba.c` (host) |
| `layers/linkLayer_rfu.c` (RP2040 PIO1) | `port/gblink/rfu_link_core.c` over a `gbl_rfu_phy_t`: `port/esp32s3/rfu_link_esp32s3.c` (hardware), `test/gblink_sim/sim_rfu.c` (host) |
| `layers/gbLinkLayer.c` | `port/esp32s3/gblink_gbspi_esp32s3.c`, host sim |
| `layers/usbLayer.cpp` (descriptors) | `port/esp32s3/usb_gblink_esp32s3.c` |
| `layers/serialLayer.cpp` | `port/gblink/serial_layer.cpp` (portable, over `src/gblink_frame.c`) |
| `hardware.cpp` | `port/esp32s3/gblink_hw_esp32s3.c`, host sim |
| `rawRelay*`, `pio_firmware/` | not built upstream either |

Files that are present here but rewritten for gbagate are listed, with the
reason for each change, in `PATCHES.md`. Each import of upstream is its own
commit, a byte-for-byte copy of the pruned tree (f607017 for 2.2.3, d33f50c for
2.2.7), with gbagate's changes re-applied in the commit after it; so
`git diff d33f50c -- third_party/gblink-firmware` is the complete list of what
gbagate changes in the current version.

## Updating

Prune the new upstream tree as above, commit it alone over this directory,
then three-way merge gbagate's changes back (`git merge-file` per file against
the previous import) and run `make gblink compat`. P1-P6 have tests that fail
without them, so a fix upstream takes is visible as a patch that no longer
applies and a test that still passes.
