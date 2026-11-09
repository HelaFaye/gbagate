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
| input | touch | the module's encoder and two keys, direct |
| build flag | `BOARD_CARRIER_TOUCH` | `BOARD_CARRIER_KNOB` |

The pin maps are in `port/esp32s3/board.h`, and `test/test_board.c` checks
them. They must not collide, must use only pins the Supermini brings out,
must stay off the strapping and USB pins, and must leave UART0 (43, 44) as
UART0. Nothing here has been built yet; see "Firmware still to write" at
the end.

## The Supermini's pins

The Supermini has two rows of pins:

- **The edge rows** (the two pin headers): GPIO 1–13, 43 (TX) and 44 (RX),
  plus 5V, GND and 3V3.
- **The inner rows** (through-holes on the underside): GPIO 14–18, 21, 33,
  34, 37 and 38 on one side, and 35, 36, 39–42 and 45–48 on the other.

Pins left alone:
- **45 and 46** are strapping pins.
- **48** is the board's own RGB LED. Its pad also sits at the top edge, but
  using it for anything else drives the LED's data input too.
- **0** is BOOT, and **19 and 20** are USB.
- **33–37** are free only because the Supermini's ESP32-S3FH4R2 has quad
  PSRAM. A module with octal PSRAM uses them, so check the chip marking if a
  different Supermini turns up.

The core signals (link port 1, the SPI bus, the panel and I2C) stay on the
edge rows, where they were. The inner rows take the rest. That leaves 43 and
44 as UART0, the console or the bridge UART: the ROM prints its boot banner
on 43 at every reset, so nothing else should sit there. It also leaves
GPIO 3, a strapping pin, unused.

**For the carrier's footprint:** fit headers to the inner rows the same way
as the edge rows, and plug the Supermini into sockets. Measure the inner
row's spacing from the edge rows on your board before laying out the PCB:
if it is not on the 2.54 mm grid, give it its own pads and route them.

## Pin map

| GPIO | row | touch carrier | knob carrier |
|---|---|---|---|
| 1 | edge | I2C SDA | I2C SDA |
| 2 | edge | I2C SCL | I2C SCL |
| 4 | edge | link SC (all ports) | link SC (all ports) |
| 5 | edge | link SD (all ports) | link SD (all ports) |
| 6 | edge | SI1: port 1's SO | SI1: port 1's SO |
| 7 | edge | SO: to port 1's SI | SO: to port 1's SI |
| 8 | edge | SI2: port 2's SO | SI2: port 2's SO |
| 9 | edge | LCD DC | LCD DC |
| 10 | edge | LCD CS | LCD CS |
| 11 | edge | SPI MOSI | SPI MOSI |
| 12 | edge | SPI SCK | SPI SCK |
| 13 | edge | SPI MISO | SPI MISO |
| 43, 44 | edge | UART0 (console, or the bridge UART) | UART0 |
| 14 | inner | SI3: port 3's SO | SI3: port 3's SO |
| 15 | inner | SD CS | SD CS |
| 16 | inner | touch CS | encoder A |
| 17 | inner | touch IRQ | encoder B |
| 18 | inner | LCD reset | LCD reset |
| 21 | inner | backlight (PWM) | backlight (PWM) |
| 33 | inner | spare | encoder push |
| 34 | inner | spare | confirm key |
| 35 | inner | spare | back key |
| 36 | inner | Battle Chip board INT | Battle Chip board INT |
| 48 | LED | the Supermini's own LED | the same |

Spare on both boards: GPIO 3 (strapping, so outputs only and pulled up),
37–42 and 47. That leaves room for a second SD path for GB-Link's GBC
cables, an infrared emitter, or the sleek version's extra parts.

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
 GPIO 14 (SI3) ◄──────────────────────────────────────┘
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
and shared, on GPIO 36 with a 10 kΩ pull-up on the carrier. It tells the
firmware a chip went in or came out without polling the bus. The carrier
has the bus's only pull-ups (4.7 kΩ). The board's own footprints for them
stay unpopulated, because two sets in parallel are too strong for the
MCP23017's 400 kHz.

**Later, as a controller:** face and shoulder buttons on the same
connector, through a second expander on this board at **0x21**. (That is
where `buttons_mcp.c` already looks for buttons.) Its INT joins the shared
line, so the connector does not change.

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
| RESET | GPIO 18, with 10 kΩ to 3V3 so the panel stays out of reset while the GPIO floats at boot |
| SDI, SCK | MOSI, SCK |
| LED | through a P-channel MOSFET (AO3401 or similar) from 3V3, with its gate on GPIO 21 and 10 kΩ to 3V3, so the backlight starts off and dims by PWM. The backlight draws 30–60 mA, more than a GPIO may source |
| **SDO** | **not connected.** On many of these modules the panel's SDO does not let go of MISO when its CS is high, which corrupts the touch and card reads. The firmware never reads the panel |
| T_CLK, T_DIN, T_DO | SCK, MOSI, MISO |
| T_CS | GPIO 16 |
| T_IRQ | GPIO 17: low while touched, so the controller is read only then |
| SD_CS / MOSI / MISO / SCK | GPIO 15 / MOSI / MISO / SCK |

ILI9341 or ST7789 cannot be told apart without reading the panel, and the
panel is not read. So it is a setting, with a test pattern to confirm it.

## The knob carrier

**Panel module (2.0" ST7789V, EC11, two keys).** The panel's SPI pins
(`SCL/SDA`, which here are SCK/MOSI) go to the shared bus. Its other pins:

| module pin | carrier |
|---|---|
| CS | GPIO 10 |
| DC | GPIO 9 |
| RES | GPIO 18, with 10 kΩ to 3V3 |
| BLK | GPIO 21, through the same MOSFET as on the touch carrier, or direct if the module switches its own backlight |
| encoder A, B | GPIO 16, 17. The encoder's common goes to ground and the internal pull-ups are used. The ESP32-S3's pulse counter decodes them, as on the devkit |
| encoder push | GPIO 33 |
| confirm, back keys | GPIO 34, 35 (to ground) |

The panel and the input are direct, so this board needs no expander of its
own.

**Card:** a micro SD breakout on the shared bus, with CS on GPIO 15. Use a
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
- The encoder's and keys' pins from `board.h` in the input driver (it
  reads the devkit's today), and the chip board's INT as a "chip changed"
  event.
- The three-port PHY is written (`src/mp_bus.c`, and the four-player mode
  in `port/esp32s3/gba_link_esp32s3.c`) and tested against modelled GBAs.
  It still has to be scoped on the carrier, and Link: Online has to start
  it (`doc/MULTIPLAY.md`, "What has to be built").
- Build environments and CI images for `carrier-touch` and `carrier-knob`.
- Then the PCB: KiCad, two layers, all headers through-hole.
