# Layout

```
gbagate/
  Makefile              host test suites: make, make strict
  README.md             design notes, decisions and their reasons
  PROJECT_STATUS.md     what works, what does not, what is next
  AUDIT.md              current audit findings
  MANIFEST.md           this file
  LICENSE               GPL-3.0 (gbagate is GPL-3.0-only; README.md, Licence)

  src/        30 modules   portable, host-tested, no hardware dependencies
  test/       28 suites    unit suites, plus gblink_sim/ and gblink_compat/
  port/esp32s3/ 22 files   ESP-IDF glue. Lint-covered, built by CI, no hardware run yet
  port/gblink/             the Zephyr shim GB-Link's firmware runs on, its
                           link layers (Multi-Play, Wireless Adapter) over
                           board drivers, glue, gbagate's GB-Link extensions
  third_party/gblink-firmware/
                           GB-Link's adapter firmware 2.2.7, vendored (GPL-3.0);
                           UPSTREAM.md, PATCHES.md say what and what changed
  backport/                patch series offered to GB-Link (README.md)
  pio/                     PlatformIO: four images, ESP-IDF framework
  qemu/       19 files     emulation lab: QEMU, mGBA, broker, tester ROM
  data/chipdb/ 6 blobs     packed chip databases, 72 KB
  tools/       10 tools    database builder, header repair, font generator,
                           mGBA-on-Android build, RAM dump script,
                           wire-ID checker, multiboot checker, compile
                           commands, build_hw.sh (ESP-IDF without
                           PlatformIO), gblink_bench.py (bring-up)
  rom/                     tester ROM sources (built by make rom)
  doc/                     rendered screens; GBLINK.md, the GB-Link adapter
```

## Start here

```
make            build and run every suite                  (no hardware)
make strict     every unit suite at -O0, -O2, -O3 -flto, ASan
make gblink     GB-Link's firmware in the host simulator: every mode,
                the wireless cable, the Wireless Adapter against librfu
make compat     GB-Link's own host software against it: netplay bridge,
                Celio, the web apps, the launcher in Chromium (needs npm)
```

Two more that need only `cc` and `python3` — no ESP-IDF, no emulator — and both
cover ground the unit suites do not:

```
python3 qemu/test_rom_selftest.py     the ROM handshake and chip injection
                                      driven against the real chipgate.c
python3 tools/check_wire_ids.py --list every database's wire-ID map
```

Then `PROJECT_STATUS.md` for what is proven and what is not, `AUDIT.md` for
where the risk sits, and `qemu/README.md` for running the firmware under
emulation.

## The split that matters

`src/` is 5458 lines with 4090 lines of tests against it and no hardware
dependencies at all — every protocol, every state machine, every piece of
arithmetic runs on the machine you are sitting at.

`port/esp32s3/` is 2170 lines that cannot be host-tested, and `pio/src/` a
further 717. **Every defect either audit round found in the firmware was in that
layer** — seven of seven, across two rounds using completely different methods.
It is thin on purpose, and the Makefile deliberately does not pretend to build
it. (The other two defects were in the lab's own shell scripts.)

The emulation lab in `qemu/` narrows that gap without closing it: the firmware
boots and links on real Xtensa, which is how the second audit round found a
correctness bug that inspection had missed. It still models no peripheral, so
the PHY, the SPI panel and USB remain unverified. See `PROJECT_STATUS.md` §3.

## Modules

### src/ — portable

| module | purpose |
|---|---|
| `chipgate` | Battle Chip Gate responder: handshake, 9-slot rotation |
| `chipdb` | flash-resident chip database reader |
| `chip_reader` | edge-pad decode, debounce, insert/remove |
| `chip_slot` | shared 11-line pad bus, both chip families |
| `ui_list` | 128×128 list model: scroll, wrap, paging, scrollbar |
| `ui_chipview` | tabs, sort, favourites, Slot-In/Move |
| `ui_shell` | mode picker, gated by detected hardware |
| `app_modes` | mode dispatch: joins a shell choice to a handler ⚠️ never compiled |
| `ui_render` | chip screen into an RGB565 buffer |
| `ui_font` | 5×7 bitmap font (generated, original) |
| `st7735_panel` | panel geometry and hardware vertical scroll |
| `input_encoder` | quadrature, acceleration, button |
| `link_proto` | bridge wire protocol, CRC-16 + SLIP |
| `link_session` | delay-based lockstep pacing |
| `link_bridge` | transport-agnostic session and routing |
| `hw_caps` | capability mask, menu gating |
| `nrf24_probe` | SPI presence and variant detection |
| `gba_multiboot` | multiboot master: encryption and CRC |
| `rom_catalog` | USB drive listing, validation, RAM staging |
| `variant` | the three images, dependency-checked |

### port/esp32s3/ — no hardware run yet

| file | purpose |
|---|---|
| `gba_link_esp32s3.c` | Multi-Play PHY, parent and child. **Timing unmeasured** |
| `st7735_esp32s3.c` | SPI, DMA flush, scroll path. Not reachable under QEMU |
| `transport_esp32s3.c` | USB CDC and ESP-NOW backends. Neither is emulated |
| `usb_msc_esp32s3.c` | FAT over USB MSC, `rom_io_t` over FATFS |
| `mb_spi_esp32s3.c` | Normal 32-bit SIO master |
| `cg_phy_esp32s3.c` | superseded; kept only to diff early assumptions. `cg_phy_slot_chip()` has no callers |
| `transport_uart.c` | the same `link_bridge` over UART instead of CDC. **Exercised under QEMU** |
| `st7735_qemu.c` | QEMU stand-in for the panel: keeps the real framebuffer, streams it out a UART |
| `input_qemu.c` | QEMU stand-in for the encoder: keystrokes on the same UART |

The last three are how the emulation lab reaches the firmware. `transport_uart.c`
is the only file in this directory with any evidence behind it.

### qemu/ — the emulation lab

| file | purpose |
|---|---|
| `build.sh`, `run.sh`, `setup.sh` | build an image, boot it, install Espressif's QEMU |
| `lab.sh` | the whole stack at once: QEMU, broker, mGBA, viewer |
| `broker.py` | joins the firmware's UART link to mGBA's socket |
| `viewer.py`, `fbgrab.py` | live framebuffer window; single-shot PPM capture |
| `mgba_tester.lua`, `mgba_link.lua` | drive the tester ROM and the link from inside mGBA |
| `gate_sim.c` | the gate without an emulator, for fast iteration |
| `test_rom_selftest.py` | the ROM handshake and chip injection against the real `chipgate.c` |
| `uart2check.sh` | which UARTs this QEMU actually wires — see `AUDIT.md` E1 |
| `rawtap.py` | raw bytes off a link or framebuffer port |

`qemu/README.md` covers running all of it, including what to do when the screen
stays blank.

## Regenerating the databases

Needs two external checkouts — the `mmbn-chip-tables` repository and mGBA's
`res/` directory, neither of which is redistributed here:

```
python3 tools/build_chipdb.py <mmbn-chip-tables> data/chipdb --names <mgba>/res
```

Builds are byte-reproducible.

## Third-party material

Nothing Capcom or Nintendo owns is included. The chip databases hold names and
statistics derived from community-documented tables; the font is an original
design; the Nintendo logo is never carried, only fingerprinted from a dump the
user supplies themselves. See the header validation section of `README.md`.
