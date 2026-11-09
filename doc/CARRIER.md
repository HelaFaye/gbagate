# DIY carrier boards: design

Two first-version boards, both built around the ESP32-S3 Supermini and
both made of breakouts on 2.54 mm headers, so they can be built by hand.
Both are powered only from USB-C, with no battery.

| | touch carrier | knob carrier |
|---|---|---|
| MCU | ESP32-S3 Supermini | the same |
| link | up to 3 GBA link ports, one Multi-Play chain | the same |
| Battle Chip | the reader board, MCP23017 on board, over I2C | the same |
| display | 2.4"–3.2" 320x240 SPI panel, ILI9341 or ST7789, with its XPT2046 touch controller | 2.0" 240x320 ST7789V module with an EC11 encoder and two keys |
| card | the panel module's own micro SD slot | a micro SD breakout |
| input | touch | encoder and keys, on an MCP23017 |
| build flag | `BOARD_CARRIER_TOUCH` | `BOARD_CARRIER_KNOB` |

The pin maps are in `port/esp32s3/board.h`, and `test/test_board.c` checks
them. They must not collide, must stay on the Supermini's header and off the
strapping and USB pins, and GPIO 43 may only be a chip select. Nothing here
has been built yet; see "Firmware still to write" at the end.

## The Supermini's pins

The header brings out **GPIO 1–13, 43 (TX) and 44 (RX)**: fifteen pins.
GPIO 48 drives the board's own RGB LED and is not on the header. (The older
`BOARD_SUPERMINI` map puts the display's DC on 48. Check your board: if 48
is not a header pin there, that map needs the same fix.)

43 and 44 are UART0. The console moves to USB on these boards. Two rules
follow from the ROM, which prints its boot banner on 43 at every reset:

- **43 is only ever a chip select.** It toggles during boot with nothing
  clocked, so no part responds, and nothing else drives it, so nothing fights
  the ROM.
- **44 is only ever an input.**

GPIO 3 is a strapping pin, but it only selects the JTAG source when an
eFuse says so, and no gbagate image burns that eFuse. The card's chip
select sits on it, with the module's pull-up holding it high at reset.

## Pin map

| GPIO | touch carrier | knob carrier |
|---|---|---|
| 1 | I2C SDA | I2C SDA |
| 2 | I2C SCL | I2C SCL |
| 3 | SD CS | SD CS |
| 4 | link SC (all ports) | link SC (all ports) |
| 5 | link SD (all ports) | link SD (all ports) |
| 6 | SI1: port 1's SO | SI1: port 1's SO |
| 7 | SO: to port 1's SI | SO: to port 1's SI |
| 8 | SI2: port 2's SO | SI2: port 2's SO |
| 9 | LCD DC | LCD DC |
| 10 | LCD CS | expander INT (0x21) |
| 11 | SPI MOSI | SPI MOSI |
| 12 | SPI SCK | SPI SCK |
| 13 | SPI MISO | SPI MISO |
| 43 | touch CS | LCD CS |
| 44 | SI3: port 3's SO | SI3: port 3's SO |
| 48 | the Supermini's own LED | the same |

All fifteen are used on both boards. The touch controller is polled over
SPI, so its IRQ pin is not needed. The panel's reset is an RC on the
carrier, and its backlight is tied on.

## The link ports: one chain

A Multi-Play cable passes a "your turn" signal along the units: each GBA's
SO feeds the next one's SI, while SC and SD are shared by all. The parent is
the unit whose SI is low. The carrier wires its three ports as that chain
with the board at both ends:

```
            SC, SD  ──────┬─────────────┬─────────────┬──── GPIO 4, 5
                          │             │             │
 GPIO 7 (SO) ──► SI ┌─────┴──┐  SI ┌───┴────┐  SI ┌───┴────┐
                    │ port 1 │ ┌──►│ port 2 │ ┌──►│ port 3 │
                    └───┬────┘ │   └───┬────┘ │   └───┬────┘
                     SO │      │    SO │      │    SO │
                        ├──────┘       ├──────┘       │
 GPIO 6 (SI1) ◄─────────┘              │              │
 GPIO 8 (SI2) ◄────────────────────────┘              │
 GPIO 44 (SI3) ◄──────────────────────────────────────┘
```

What that gives:

- **Port 1 is wired exactly like the single port always was**: the board's
  SO goes to the GBA's SI, and its SO comes back on SI1. Every one-cable
  mode runs there unchanged, including the 2-player link, the Wireless
  Adapter, multiboot, Battle Chip Gate, infrared and the printer.
- **Three local GBAs plus the board as player 4.** The board holds its SO
  low, so port 1's GBA is the parent. The board waits for SI3 (port 3's
  turn ends) and then answers in slot 3.
- **Fewer local GBAs, remote players in the other slots.** The board
  answers each slot after the last local GBA, timed from that GBA's SI
  line. Or it takes slot 0 itself: it drives SC as the parent and releases
  port 1 with its SO.
- **Fill the ports in order.** An empty port 2 leaves port 3's SI floating.
  100 kΩ pull-downs on SI2 and SI3 make an empty port read as absent, which
  is half of how the board counts the GBAs it has (`gblink_cable_gbas`).
  The other half is the handshake words (`mp_detect` in `src/mp_relay.c`).

Per line, on the carrier: 100 Ω series resistors on everything the board
drives (SC, SD, SO), against ringing on a long cable. Leave the link port's
VCC pin unconnected, as Nintendo's cables do. Each GBA powers itself.

GBA links are 3.3 V, so no level shifting is needed. A Game Boy or Game
Boy Color is 5 V and must not be connected without a shifter
(`doc/GBLINK.md`, Wiring). Whether a GBA running a Game Boy game raises its
link port to 5 V is not established: measure it before connecting one.

Port breakouts: any GBA link socket breakout (SC, SD, SI, SO, GND), on
a 1x6 header. Optional footprint: a 4-line TVS array (SRV05-4 or similar)
on each port, since the ports are the board's exposed connectors.

## The Battle Chip board

One MCP23017 at **0x20** (A0–A2 to ground), wired as `src/mcp23017.c`
expects:

| expander | Battle Chip slot |
|---|---|
| GPA0–GPA7 | pads 1–8 |
| GPB0–GPB2 | pads 9–11 |
| GPB3 | Advance/Progress slot detect switch (to ground when seated) |
| GPB4 | Beast Link slot detect switch |
| GPB5–GPB7 | spare |

The internal pull-ups are used. Both slots share the eleven pad lines
(`src/chip_slot.h` explains why that is safe).

**Connector to the carrier:** 1x6 at 2.54 mm (or JST-PH 6 for a cable):
`3V3, GND, SDA, SCL, INT, 5V`. INT is the expanders' interrupt, open-drain
and shared. The carrier has the bus's only pull-ups (4.7 kΩ). The board's
own footprints for them stay unpopulated, because two sets in parallel are
too strong for the MCP23017's 400 kHz.

**Later, as a controller:** face and shoulder buttons on the same
connector, through a second expander on this board at **0x22**. (0x21 is
the knob carrier's.) Its INT joins the shared line, so the connector does
not change.

## The touch carrier

**Panels.** The red 2.8"/3.2" ILI9341 modules and the DIYables 2.4"/2.8"
ST7789 touch modules carry the same 14 signals in the same order:
`VCC GND CS RESET DC SDI SCK LED SDO T_CLK T_CS T_DIN T_DO T_IRQ`. The
DIYables module lists them bottom to top. Their card slot has its own
4-pin header (`SD_CS SD_MOSI SD_MISO SD_SCK`). One 1x14 socket and one
1x4 fit all of them.

| module pin | carrier |
|---|---|
| VCC | **5V** (the module's regulator; leave its J1 3.3 V jumper open) |
| CS / DC | GPIO 10 / 9 |
| RESET | 10 kΩ to 3V3 and 100 nF to ground (power-on reset; the driver also sends a software reset) |
| SDI, SCK | MOSI, SCK |
| LED | 3V3 (the module switches its backlight) |
| **SDO** | **not connected.** On many of these modules the panel's SDO does not let go of MISO when its CS is high, which corrupts the touch and card reads. The firmware never reads the panel |
| T_CLK, T_DIN, T_DO | SCK, MOSI, MISO |
| T_CS | GPIO 43 |
| T_IRQ | not connected (polled), or to a test pad |
| SD_CS / MOSI / MISO / SCK | GPIO 3 / MOSI / MISO / SCK |

ILI9341 or ST7789 cannot be told apart without reading the panel, and the
panel is not read. So it is a setting, with a test pattern to confirm it.

## The knob carrier

**Panel module (2.0" ST7789V, EC11, two keys).** The panel's SPI
(`SCL/SDA`, which here are SCK/MOSI) goes to the shared bus, CS to GPIO 43
and DC to GPIO 9. RES gets the same RC as above, and BLK is tied on. The
module's encoder and keys go to the carrier's MCP23017 at **0x21**:

| expander | |
|---|---|
| GPA0, GPA1 | encoder A, B (common to ground) |
| GPA2 | encoder push |
| GPA3 | the module's confirm key |
| GPA4 | its back key |
| GPA5–GPB7 | spare: room for a D-pad and face buttons on a later revision |

Its INT goes to GPIO 10. The expander captures the pins' state on each
change (INTCAP), so a step is read as it was even if the read comes a
little later. A fast spin is about 400 edges a second, and one I2C read
takes about 60 µs.

**Card:** a micro SD breakout on the shared bus with CS on GPIO 3. Use a
3.3 V breakout, or a 5 V one with its own regulator and buffer on the 5V
rail.

## Power

USB-C on the Supermini is the only input. Its 3.3 V regulator (500 mA
class) feeds the ESP32-S3, the expanders and a 3.3 V card. The panel module
is fed from 5 V through its own regulator, which keeps the backlight off
the Supermini's regulator.

| | typical | peak |
|---|---|---|
| ESP32-S3, WiFi on | 100 mA | 350 mA (transmit) |
| card, writing | 30 mA | 100 mA |
| expanders | 2 mA | 2 mA |
| **3.3 V rail** | **~130 mA** | **~450 mA** |
| panel and backlight (from 5 V) | 60–90 mA | 100 mA |

Bulk capacitance: 47 µF on 5 V at the panel header, and 10 µF plus 100 nF
at each breakout's 3V3 pin.

## USB-C

The ESP32-S3 has one USB port. On these boards it is the Supermini's own
USB-C, which carries the gbagate interfaces (GB-Link vendor and CDC, USB
storage) and the power. On the sleek version, the "two clearly labelled
USB-C ports" can be one data-and-power port wired to the S3's USB pins, and
one power-only port. Both feed 5 V through diodes or an ideal-diode OR, and
the power-only port's data pins stay unconnected. A second data port would
need a USB hub chip.

## The UI on 320x240

The UI is written for a 128x128 panel. Its sizes are compile-time
constants (`src/ui_list.h`: 12-pixel rows, a 14-pixel header and an
18-pixel footer), and its input is buttons and an encoder.

**Not touch-friendly yet.** On a 2.8" 320x240 panel a pixel is about
0.18 mm, so a 12-pixel row is about 2 mm. A comfortable touch target is
7–9 mm, which is 40–48 pixels. Scaling the current screens up does not
work either: 2x is 256x256, which does not fit 240 lines.

What it needs:

1. The layout's sizes come from the board, and every screen is drawn from
   them. `test_screens` renders the whole gallery at both 128x128 and
   320x240, and checks each screen at both.
2. A 2x font for the large panels: the current font, doubled.
3. Touch events in `ui_event.h`: tap at (x, y), drag and release. Lists
   map a tap to a row, and a drag to a scroll.
4. On-screen Back and OK buttons in the footer, because touch has no keys.
5. The Battle Chip gate screens and the chip list shown as touch targets.

The knob carrier's 240x320 panel needs steps 1 and 2. Its input is the
encoder, which the UI already supports.

## Firmware still to write

- ILI9341 and ST7789 drivers at 320x240, with DMA and dirty-rectangle
  updates. A full frame is 150 KB, so it lives in PSRAM; the Supermini's
  S3FH4R2 has 2 MB.
- XPT2046 touch: polling, filtering and a calibration screen, with the
  calibration saved.
- The encoder and keys through the expander's INT, as input events.
- The three-port PHY: answering in the slots after the last local GBA, timed
  from SI1–SI3, and taking slot 0 as the parent. This is the hardware layer
  that `doc/MULTIPLAY.md`'s "What has to be built" table lists.
- The console on USB for these boards.
- Build environments and CI images for `carrier-touch` and `carrier-knob`.
- Then the PCB: KiCad, two layers, all headers through-hole.
