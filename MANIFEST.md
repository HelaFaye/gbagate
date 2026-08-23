# Layout

```
gbagate/
  Makefile              host test suites: make, make strict
  README.md             design notes, decisions and their reasons
  PROJECT_STATUS.md     what works, what does not, what is next
  AUDIT.md              mechanical audit: 6 defects found and fixed
  MANIFEST.md           this file

  src/        19 modules   portable, host-tested, no hardware dependencies
  test/       14 suites    618 assertions
  port/esp32s3/ 6 files    ESP-IDF glue. UNTESTED -- see AUDIT.md
  pio/                     PlatformIO: three images, ESP-IDF framework
  data/chipdb/ 6 blobs     packed chip databases, 72 KB
  tools/       5 tools     database builder, header repair, font generator,
                           mGBA-on-Android build, RAM dump script
  doc/                     rendered screens
```

## Start here

```
make            build and run all fourteen suites          (no hardware)
make strict     every suite at -O0, -O2, -O3 -flto, ASan   (56 combinations)
```

Then `PROJECT_STATUS.md` for what is proven and what is not, and `AUDIT.md` for
where the risk sits.

## The split that matters

`src/` is 5412 lines with 4042 lines of tests against it and no hardware
dependencies at all — every protocol, every state machine, every piece of
arithmetic runs on the machine you are sitting at.

`port/esp32s3/` is 1909 lines that cannot be host-tested. Every defect the audit
found was in that quarter. It is thin on purpose, and the Makefile deliberately
does not pretend to build it.

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

### port/esp32s3/ — untested

| file | purpose |
|---|---|
| `gba_link_esp32s3.c` | Multi-Play PHY, parent and child. **Timing unmeasured** |
| `st7735_esp32s3.c` | SPI, DMA flush, scroll path |
| `transport_esp32s3.c` | USB CDC and ESP-NOW backends |
| `usb_msc_esp32s3.c` | FAT over USB MSC, `rom_io_t` over FATFS |
| `mb_spi_esp32s3.c` | Normal 32-bit SIO master |
| `cg_phy_esp32s3.c` | superseded; kept only to diff early assumptions |

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
