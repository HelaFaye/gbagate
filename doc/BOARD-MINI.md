# The mini board

Everything the project needs, from parts already on the bench: an
ESP32-S3-WROOM-1 devkit, a 1.44" ST7735, a five-way switch, some tactiles, a
micro SD module and two GBA SP link sockets.

No USB storage, no chip reader, no infrared, no second radio. What it does is
be a Battle Chip Gate, be a link partner, and send multiboot programs — which
is the part of this project that works.

Build it with `pio run -e mini`.

---

## Two boards, one firmware

| | 44-pin devkit | Supermini |
|---|---|---|
| usable GPIO | ~44 | **14** |
| flash / PSRAM | 16 MB / 8 MB | 4 MB / 2 MB |
| battery | no | **yes, BAT pad** |
| switches | direct GPIO | **I2C expander** |
| build | `pio run -e mini` | `pio run -e mini-supermini` |

The Supermini cannot carry this layout directly. Link (4) + display (4) + SD (2)
leaves two pins, and the GBA arrangement needs ten:

```
  encoder      13 pins  fits
  5-way + 2    17 pins  does not
  5-way + 4    19 pins  does not
  5-way + 5    20 pins  does not
```

An **MCP23017** closes it: two pins for sixteen. Total becomes 12 of 14, with
two spare on the board and six on the expander. The cost is a bus hop per poll
-- about 60 us for a two-byte read at 400 kHz, against a UI polling at 60 Hz.
Not close to mattering.

`src/mcp23017.c` was already written and tested for the chip reader; reading a
button bank is the same part used the other way round, so this was one function
and a driver, not a new subsystem.

**Which to build.** The Supermini for the finished thing: it is small enough to
hold and that BAT pad is a battery connector, which the devkit has no
equivalent of -- a handheld tethered to USB is not really a handheld. The
devkit for bring-up: everything direct-wired, nothing behind an I2C hop while
you are trying to scope a link pin, and flash for OTA.

One asymmetry worth knowing: a missing chip reader hides a menu entry, but a
missing button expander leaves nobody able to press anything. `buttons_mcp.c`
treats it as fatal and says so, rather than booting to a menu that cannot be
moved.

## Bringing it up in stages

What is *fitted* is declared separately from what is *wired*, so a part can be
left off with a build flag rather than by commenting out an init call — which
is how that normally gets done and how it normally gets forgotten.

```
pio run -e mini-bringup          display and link, no card
pio run -e mini                  everything
```

or by hand, for any combination:

```
idf.py -DBOARD_HAS_SD=0 -DBOARD_HAS_BUTTONS=0 build
```

The order worth following:

1. **Display alone.** `mini-bringup` with nothing else populated. You are
   looking for the menu, and for the right colours and offsets — Settings
   corrects both, on the device, and the values persist.
2. **Buttons.** Set `Settings → input` to `5-way + 5` and check the footer
   names the controls you actually fitted.
3. **The link port.** Self Test drives it and reports six checks.
4. **The card, last.** It shares SPI2 with the display, so adding it first
   means a bus problem and a driver problem look identical. With the display
   already proven, a glitch that appears when the card arrives is the card.

Each step boots to a menu that offers only what is present — a mode whose
hardware is absent is hidden rather than shown and broken, and the console log
says which parts were found.

## What the parts change

The devkit is an **N16R8**: 16 MB flash and 8 MB PSRAM, against the 4 MB and
2 MB of the Supermini this project first targeted, and 44 pins against about
16. Two consequences worth taking:

- **OTA fits.** `partitions-16mb.csv` gives two 4 MB app slots. That matters
  more here than on the other boards, because with no USB storage a cable and
  a reflash is otherwise the only way in.
- **The pin budget stops being the constraint.** The earlier design put eleven
  chip-reader lines behind an I²C expander to save pins. This board has pins
  to spare, and no reader.

Programs live on the **SD card** rather than a USB volume.
`rom_catalog.c` takes its filesystem as callbacks, so the catalogue, the
validation and the staging are the same code — only `sd_esp32s3.c` differs
from `usb_msc_esp32s3.c`. That seam was designed in early and this is the first
time it has paid.

---

## Layout

```
            ┌──────────────────────────────┐
            │   ▄▄▄▄  link (top)  ▄▄▄▄     │      GBA SP socket
            │                              │
            │      ┌────────────────┐      │
            │      │                │      │
            │      │   128 x 128    │      │      ST7735, centred
            │      │    ST7735      │      │
            │      │                │      │
            │      └────────────────┘      │
            │                              │
            │    ▲                    ⬤ A  │      five-way left,
            │  ◀ ⬤ ▶      ⬤   ⬤       ⬤ B  │      A/B right,
            │    ▼      SEL  START         │      Start/Select centre
            │                              │
            │   ▄▄▄▄  link (bottom) ▄▄▄▄   │      GBA SP socket
            └──────────────────────────────┘

   back:      ⬤ L                    ⬤ R          shoulder buttons,
                                                   reachable by index fingers
```

The five-way's centre click is **A**, and the two buttons on the right face are
**A** and **B** — so A exists twice, deliberately: the click is where your
thumb already is when scrolling, and the discrete A is where a GBA player
expects it. Both produce `BTN_A`; nothing above the driver can tell them apart.

L and R on the back rather than the top edge, as asked. The five-switch input
mode (`5-way + 5`) is the one this board wants: B, Select, Start, L, R.

---

## The two link sockets

**Wired in parallel — they are one bus, not two links.**

Multi-Play is a shared bus: SC, SD, SI and SO are common to every participant,
and the cable daisy-chains. Two sockets let this board sit *inline* in that
chain rather than at the end of it, which is what makes it useful next to a
real accessory:

```
GBA ──── link (top) [ mini ] link (bottom) ──── another GBA or a real gate
```

Both sockets connect to the same four ESP32 pins. What they do **not** give you
is two independent links — nothing in the hardware or the protocol supports
that, and wiring them to separate pins would produce a board that looks like it
should work and cannot.

**Pin 1 (VCC) is left unconnected on both.** The port supplies 3.3 V at
milliamps, which is nowhere near enough to run this board, and back-feeding a
console is a good way to damage one. Power comes from the devkit's USB-C.
Ground is common — that part is essential, and a link with no common ground
reads as noise.

---

## Pins

Against the devkit's 44-pin header. Nothing here is fixed by hardware except
the strapping pins to avoid; adjust to suit your layout and change the
`#define`s.

| Function | Pin | Note |
|---|---|---|
| **Link SC** | 4 | both sockets |
| **Link SD** | 5 | |
| **Link SI** | 6 | |
| **Link SO** | 7 | |
| **Display SCK** | 12 | SPI2, shared with the card |
| **Display MOSI** | 11 | |
| **Display CS** | 13 | |
| **Display DC** | 14 | |
| **Display RST** | — | tie to the devkit's 3V3 |
| **Display BLK** | — | tie to 3V3 |
| **SD CS** | 10 | same SPI2 bus |
| **SD MISO** | 9 | the display never reads |
| Five-way up | 15 | |
| Five-way down | 16 | |
| Five-way left | 17 | |
| Five-way right | 18 | |
| Five-way click (A) | 21 | |
| B | 38 | |
| Select | 39 | |
| Start | 40 | |
| L | 41 | on the back |
| R | 42 | on the back |

Every switch shorts to ground; the internal pull-ups hold them high at rest.
No external resistors, no debounce hardware — `ui_input.c` does that, and it is
tested.

**Avoid GPIO 0, 3, 45 and 46** — strapping pins, which decide boot mode. A
switch on one of those is a board that sometimes boots into the ROM
bootloader instead of your firmware, intermittently, depending on whether a
finger was resting on it.

**GPIO 19 and 20 are USB D-/D+.** Leave them alone if you want the USB-C port
to keep working for flashing and the console.

---

## The card and the display share SPI2

Fine by construction rather than by luck: `rom_stage()` reads a whole program
into RAM and closes the file *before* any transfer begins, and the display is
only ever written from the UI task. The two are never busy at once.

If the display glitches while browsing programs, that is the first thing to
suspect, and the fix is a separate host rather than more careful sharing.

The SD driver runs the bus at 20 MHz rather than the usual 40. On a devkit with
jumper wires, 40 gives intermittent read errors that look like a corrupt card
rather than a signal problem.

---

## What the firmware does differently

`pio run -e mini` builds `VARIANT_MINI`:

```
display, encoder, chip gate, all databases, link parent, link child,
multiboot, self test, SD card
```

The menu shows **Battle Chip, Send Program, Self Test, Settings, Diagnostics**.
Read Chip, USB Storage, Infrared and the link modes are absent because the
hardware is — `hw_caps` hides them rather than offering something that cannot
work.

Set **Settings → input** to `5-way + 5` once; it is stored.

One thing to know about the SD path: it **does not format a card that fails to
mount**. The USB volume is ours and is formatted if empty; a card is the user's,
with their programs on it, and reformatting because a read went wrong would be
the worst available response.
