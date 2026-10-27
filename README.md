# GBA link adapter + Battle Chip Gate

Design notes and the reasoning behind each decision. See `MANIFEST.md` for the
layout, `PROJECT_STATUS.md` for what is proven and what is not, and `AUDIT.md`
for where the risk sits.

```
make            every host suite, no hardware
make strict     every unit suite at -O0, -O2, -O3 -flto and under ASan/UBSan
make compat     GB-Link's own host software against the firmware
```

**It is also a GB-Link adapter.** Every hardware image runs GB-Link's adapter
firmware (2.2.7, `third_party/gblink-firmware`), so the GB-Link web apps, the
netplay bridge, Celio and the Switch bridge see a GB-Link: every mode, from the
Gen 3 relay to the Wireless Adapter. Two boards can also play GB-Link's relay
to each other over ESP-NOW with no PC at all. See `doc/GBLINK.md`; fixes found
upstream along the way are in `backport/`.

Firmware building blocks for an ESP32-S3 that plugs into a GBA link port. Two
related jobs share one PHY:

- **Device mode** — be a Battle Chip Gate (or e-Reader, or anything else that
  answers for itself). No latency problem, because the answers are generated
  locally.
- **Adapter mode** — be a link partner for a remote player: mGBA over USB, or
  another adapter over ESP-NOW.

| file | what it is | verified? |
|---|---|---|
| `chipgate.{h,c}` | Battle Chip Gate responder | yes, two independent sources |
| `link_proto.{h,c}` | host/peer wire protocol, CRC + SLIP | yes, tests pass |
| `link_session.{h,c}` | delay-based lockstep pacing | yes, tests pass |
| `gba_link_esp32s3.c` | Multi-Play PHY, parent and child roles | framing from spec, timing unmeasured |
| `st7735_panel.{h,c}` | Panel geometry + hardware scroll maths | yes, tests pass |
| `input_encoder.{h,c}` | Quadrature + acceleration + button | yes, tests pass |
| `cg_phy_esp32s3.c` | earlier child-only PHY | superseded by `gba_link_esp32s3.c` |
| `test_chipgate.c`, `test_link.c` | host harnesses | pass |

```
make run-chipgate
```

## The one idea that makes networking possible

GBATEK, Multi-Player Mode: *the parent blindly sends data regardless of whether
the children have processed old data or supplied new data, so the parent may
need to insert delays between transfers.*

A child's word must already be staged when the parent starts, so **you cannot
relay a remote player into a child slot** — this is why "wireless link cable"
projects keep failing.

There is one crude child-side brake: GBATEK notes a slave may signal not-ready
by temporarily leaving multiplayer mode, which stops it driving SD high, and a
parent that checks `SIOCNT` bit 3 will wait. But it reads as a bad cable rather
than "one more frame", and nothing obliges a game to check. Test it per game;
don't build on it.

But the parent sets the pace, and the spec explicitly expects it to insert
delays. So make the adapter the parent. Remote latency becomes a pause between
transfers instead of a missed slot. Wire the adapter into the parent connector
position and the local GBA comes up as a child by itself.

`link_session.c` is that stall, with bookkeeping. `gba_link_esp32s3.c` spends
the wait in `m_transfer`'s preamble, where it is harmless.

## Wire framing

Each word on SD is **18 bits**: start bit (low), 16 data bits MSB first, stop
bit (high). Data is driven while SC is low and sampled on the rising edge. The
parent sends first into SIOMULTI0, then drops its SO to hand the token to the
first child, and so on down the chain. A child that does not assert a start bit
within the timeout ends the transfer, and its slot reads `0xFFFF`.

At 115200 baud: 8.68 us/bit, 156 us/word, under 1 ms for a full four-player
transfer. A game that transfers once per VBlank leaves ~15.7 ms of slack —
that is the budget the network has to fit inside.

## Transport budget

| transport | round trip | verdict |
|---|---|---|
| USB CDC (native OTG) | < 1 ms | best for the mGBA bridge |
| ESP-NOW / raw 802.11 | 1-3 ms | GBA-to-GBA wireless |
| BLE | >= 7.5 ms connection interval | **control only, never link frames** |
| Internet | tens of ms | don't offer it |

Use BLE for the phone app — slotting chips, editing folders, settings. Relaying
link frames over it will produce half a frame of unavoidable jitter.

## Protocol

`[type:1][seq:2 LE][payload][crc16:2 LE]`, CRC-16/CCITT-FALSE. SLIP framed on
byte streams, raw on datagrams. `LP_WORD` carries the peer's word for a
sequence; `LP_RESULT` reports a completed transfer as all four slots.

Sequence numbers are 16-bit and wrap every ~18 minutes at 60 transfers/second;
`link_session.c` handles the wrap in signed 16-bit space and there is a test
for it.

`0xFFFF` is reserved on the bus — it is what a reader sees for a slot nobody
drove. Never relay it as real payload.

## Chip IDs

For a physical Battle Chip the ID is the binary number formed by its shorted
edge pads. Confirmed across all 58 schematics in the EXE chip project:

- **Advance / Progress**, 11 pads: pad **10** is the common bus (24/24 nets).
  ID bits on pads 1-9 and 11, so 10 bits.
- **Beast Link**, 12 pads: pad **2** is the common bus (23/23). ID bits on pads
  3-11, so 9 bits. Pads 1 and 12 are never wired.

No silicon in a chip. Reader = drive the common pad low, read the rest with
pull-ups, sample twice ~20 ms apart. Put the 11 lines behind an MCP23017 or a
74HC165 rather than spending 11 GPIOs.

**This number — the pads — is the wire ID, and it is what slot 5 of the gate
rotation must carry.** It is not the chip table's index, and the two are not
interchangeable; assuming they were cost 418 chips their correct identity until
it was measured. See the wire-ID section below and `AUDIT.md` D7.

## Build order

1. Host harnesses. **Done.**
2. **GBE+ over TCP** — its chip gate is socket-driven, so a WiFi client proves
   your chip IDs and UI against a real game with no PHY at all. *Skipped:
   overtaken by 3.*
3. **mGBA bridge. Done, by a different route than planned.** The plan was an
   SIO driver alongside `GBASIODolphin`, upstream. What actually works is a Lua
   script inside mGBA plus `qemu/broker.py`, which needs no mGBA patch and no
   upstream cooperation — and the firmware on the other end runs in QEMU on real
   Xtensa rather than on a host. An upstream driver is still the tidier long-term
   answer (issue #2379 asks for it and has no implementation), but it is no
   longer on the critical path.
4. **Firmware under QEMU. Done, and it earned its place.** Not in the original
   plan. It is the tier that first put the gate in front of a real game, and it
   found a live correctness bug the host suites could not — see `AUDIT.md` D7.
   `qemu/README.md` covers it.
5. **Real hardware** — only now does the PHY timing matter. Nothing above
   removes a single item from that list; QEMU models no peripheral.

## Still unmeasured

Framing is from GBATEK, but nothing in `gba_link_esp32s3.c` has met a logic
analyser. Check on first bring-up:

- Setup/hold around the SC edges at 115200. `spin()` uses cycle counts, so
  verify `esp_clk_cpu_freq()` matches the actual clock after any DFS change.
- `CHILD_TIMEOUT_US` (100 us) versus what real hardware allows before ending a
  transfer.
- Whether `so(false)` needs to lead the child's first SC edge.

## Two things that will bite you

- **IRAM.** Any flash write anywhere in the system suspends the instruction
  cache. Mid-frame that corrupts 18 bits of clocking. The link task and
  everything it calls are `IRAM_ATTR`; keep it that way, and put `chipgate.c`
  and `link_session.c` in IRAM too.
- **Power.** Link port pin 1 gives 3.3 V at milliamps, nowhere near a WiFi
  radio. Separate supply, common ground.

## Optional hardware and gated menus

`hw_caps.{h,c}` turns detected hardware into a capability mask; menu entries
declare the bits they need and are omitted entirely when unsatisfied.
`hw_menu_hidden()` exists so a diagnostics screen can explain the omission —
without it, "the wireless option is missing" is an unfalsifiable bug report.

`nrf24_probe.{h,c}` detects the radio over SPI with the transfer function
injected, so it is testable against a simulated part. Presence is not "bytes
came back" — a floating bus returns those too. It is:

1. STATUS is neither `0x00` nor `0xFF` and bit 7 is clear.
2. SETUP_AW has its six hardwired zero bits clear and a low field of 1–3.
3. A five-byte pattern written to `TX_ADDR` reads back exactly, then restores.

Step 3 is the one that counts. It also reports whether the part honours
`RF_DR_LOW`, which distinguishes a genuine nRF24L01+ from clones that accept
the write and drop the bit — relevant because 250 kbps is the mode you would
reach for to buy range.

### The nRF24 will not talk to a real Wireless Adapter

Worth being blunt about: per the FCC filing, Nintendo's RFU is a 2.4 GHz device
using **ASK modulation at roughly 4 Mbps in a TDMA scheme**. The nRF24L01+ is
GFSK at 250 kbps / 1 / 2 Mbps with a fixed Nordic frame format and no raw-PHY
access. It cannot produce that waveform. Nor is the RFU air protocol documented
— all the published reverse engineering (Corwin, Rodrigo/afska, davidgf) covers
the *link-cable* side between the GBA and the adapter, not the radio side
between two adapters. Emulators exploit exactly that: they intercept the SIO
command set and carry payload over their own transport.

So the supported path for Wireless Adapter games is to emulate the RFU toward
the GBA over the link port and carry payload over ESP-NOW or WiFi between two
of these boards. That gets game compatibility, not hardware interop.

The nRF24 is still worth its footprint for one reason: it is a **second,
independent radio**. The ESP32 has one 2.4 GHz front end shared by WiFi and
BLE, so bridging to mGBA over WiFi while also linking to a peer means
coexistence stalls. Offloading the peer link to the nRF24 removes that
contention, and PA/LNA buys range.

Two practical notes on PA/LNA modules: they need bulk capacitance (10–100 µF)
right at the module or they brown out on transmit, and they desense badly when
sitting next to the ESP32 antenna — keep them apart and pick non-overlapping
channels.

## Chip reader and the 128x128 UI

`chip_reader.{h,c}` decodes a physical chip from its pads, with the pad word
supplied by a callback so it can be tested on a host and driven by an MCP23017
or 74HC165 on hardware. `ui_list.{h,c}` is the scrolling list model, free of any
display calls. `build_chipdb.py` packs the `.asm` chip tables into flash blobs.

```
make run-ui
```

Database sizes, all six US games:

| game | chips | total |
|---|---|---|
| bn4r / bn4b | 389 | 10.9 KB each |
| bn5p / bn5c | 424 | 11.7 KB each |
| bn6g / bn6f | 411 | 11.3 KB each |

**66 KB for everything.** 16-byte fixed records plus a separate string pool, so
records stay directly indexable and the whole table can sit in flash unparsed.

### Two things the schematics forced into the design

**A blank chip is invisible without a detect switch.** The EXE project's
`Advance_Base_Chip` has no shorts at all, so its pad word is identical to an
empty slot. Chip ID 0 is therefore unreadable on the pads alone. `chip_reader`
takes a `present` flag for a mechanical switch; without one it resolves the
ambiguity as *empty*, because inventing a chip puts a bogus ID on the link
while missing a blank one merely disappoints. Fit the switch.

**Bit order is a calibration knob, not a known.** That the pads form the ID is
established across 58 schematics. Which pad is the MSB is not, and guessing
wrong yields plausible but wrong IDs. `msb_is_lowest_pad` flips it; one chip
whose ID you know settles it permanently.

### Layout

Header 14px, list 8 rows x 12px, footer 18px — exactly 128. Row columns: star 8,
name 66, code 8, MB 20, element 10, leaving 16 for the scrollbar. The 11-char
maximum name (`ProtoMn[EX]`) fits the 66px name column at a 6px advance.

`ui_list` supports wrap mode, which is what a rotary encoder wants, plus paging
and absolute jump for an alphabet index. Scrollbar geometry comes out of the
model so the renderer cannot disagree with it; the thumb clamps to 4px so it
stays visible at 411 entries.

### Wire IDs

Resolved, and without needing a ROM or an SIO capture. mGBA ships the mapping in
`res/exe4|exe5|exe6/chip-names.txt`, and `BattleChipModel::reloadAssets` shows
the rule: **chip ID is the 1-based line number**, with blank lines still
consuming an ID.

```
python3 build_chipdb.py <mmbn-chip-tables-main> chipdb/ --names <mgba>/res
```

| game | chips | IDs | mapped | identity | unmapped |
|---|---|---|---|---|---|
| bn4r / bn4b | 389 | 307 | 285 | 82 | 22 |
| bn5p / bn5c | 424 | 305 | 293 | 289 | 12 |
| bn6g / bn6f | 411 | 470 | 285 | 283 | 185 |

Names differ between sources — mGBA spells them out (`MegaCannon`, `SuperVulcan`)
while the tables use the 8-character in-game forms (`M-Cannon`, `SuprVulc`) — so
matching is by abbreviation: a table name matches when its letters appear in
order within the mGBA name. Identity (`id == index`) is *preferred but verified*
rather than assumed, which is why the identity column is reported separately.
BN5 is almost entirely identity; BN4 drifts after about id 80; BN6 holds
identity below id 320 and then diverges where the Navi V-chips live.

Unmapped IDs are left unmapped rather than guessed. Most of BN6's 185 are above
id 320 or in ranges the Gregar table leaves blank, so there is nothing to map
them to.

**The identity column is load-bearing, and nothing ever read it.** Subtract
it from `mapped` and you get the number of chips whose wire ID is *not* their
table index: 203 for BN4, 4 for BN5, 2 for BN6 — 418 across the six databases.
For most of the project's life `gate_app.c` sent `chip_record_t.index` in slot 5
of the rotation regardless, so those 418 chips announced themselves to the game
as a different chip than the one on screen. In BN4 that was 71% of everything
sendable. The builder had measured the divergence honestly and printed it in
this very table; nothing connected the table to the consumer.

The lesson is not about chip IDs. It is that a fact verified in one module and
recorded in a document is not thereby true of the module that consumes it — and
that the failure was invisible precisely because the wrong value was
structurally plausible. See `AUDIT.md` D7.

`chipdb.{h,c}` reads the blob in place out of flash — no parsing, no copying,
fixed 16-byte records. `chipdb_slot_for_id()` answers the reader's question —
pads to record — and `chipdb_id_for_slot()` the gate's, which is the inverse:
given the record the user picked, the wire ID that names it. They are separate
calls because they are separate questions, and conflating them was D7.
`build_chipdb.py` guards both sides of each assignment with a `used` set, so the
map is injective and the inverse is unambiguous; `test_chipdb` round-trips every
mapped ID to keep it that way. `chipdb_open()` validates every section offset,
because a truncated flash write should fail at open rather than surface later as
garbage names.

Records with no wire ID cannot be sent at all, and the gate now refuses rather
than substituting a plausible number: roughly 30% of each database, most of them
chips no physical Battle Chip ever encoded.

```
python3 tools/check_wire_ids.py --list
```

reports the map for every database and fails only on a genuinely broken one —
two wire IDs claiming one record, or a record slot past the table.

```
make run-chipdb
```

## Rotary encoder

`input_encoder.{h,c}` decodes quadrature with acceleration, plus the shaft
button. Portable: feed it two channel levels, the button level and a millisecond
timestamp.

Debouncing is structural rather than timed. A 16-entry transition table accepts
only legal quadrature moves, so contact chatter produces a +1 immediately
followed by a −1 and nets to zero. More robust than an RC filter and free.
Illegal double transitions (00 → 11) are counted in `glitches` — a rising count
means bad contacts, long wires, or a poll rate below ~1 kHz.

Acceleration is **linear, not exponential**, and deliberately so: a slow turn
must always move exactly one row or picking a specific chip becomes a fight.
Inside a 150 ms window the step scales up to 20×, which crosses BN6's 411 chips
in **25 detents** instead of 51 full turns. Both behaviours are asserted in
`test_encoder`.

The button distinguishes click from long press, and a release after a long press
is *not* also a click — otherwise every hold would slot a chip on the way out.

```
make run-encoder
```

## ST7735: "not cropped" does not mean zero offset

Worth flagging before bring-up. The ST7735 controller has **132×162** of frame
memory, so even a native 128×128 panel is a window inside it. The 1.44" green-tab
modules almost always need `colstart = 2, rowstart = 3`; some want `2, 1`.

Treat the offset as a runtime setting with a calibration pattern — draw a 1px
border and nudge until it closes — rather than a compile-time constant. This is
the classic two-pixel-shift time sink.

## Display: ST7735 geometry and hardware scroll

`st7735_panel.{h,c}` holds panel geometry and the scroll arithmetic. No SPI
calls, so the maths is testable on a host and the driver stays thin.

Default is **128×128** (`colstart 2, rowstart 3`); `ST7735_PANEL_128X160`
switches to the 1.8" panel (`2, 1`). Both offsets are runtime-settable via
`st7735_panel_set_offset()` — calibrate with a 1px border and nudge until it
closes. `st7735_window()` refuses rectangles that fall outside the glass rather
than letting them vanish into invisible memory.

### Using the spare frame memory

The controller always has 132×162 of memory regardless of the glass, so a
128×128 panel leaves 4 spare columns and 34 spare rows.

**The columns are dead.** Nothing is behind them, and read-back is not a usable
scratchpad — RAMRD is far slower than writes and most breakouts don't wire MISO.

**The rows are worth having, via hardware vertical scroll.** Point VSCRDEF's
scrolling area at the chip list:

```
tfa=17 vsa=96 bfa=49        (17 + 96 + 49 = 162, as the controller requires)
full list repaint 24576 B   one row 3072 B   ratio 8.0x
```

Moving one row costs a single 12px write plus one VSCRSADD write instead of
repainting all eight rows — ~1.5 ms against ~12 ms at 20 MHz. That is the
difference between an encoder that feels direct and one that smears.

This works by repainting the row that just wrapped off the top, so it needs no
off-panel rows at all; a 128×160 panel with only 2 spare rows scrolls just as
well. `st7735_scroll_fill_row()` returns which memory row to repaint, and
`st7735_window_raw()` reports how many rows fit before the wrap so a straddling
strip can be written in two pieces.

The 34 spare rows on 128×128 are a bonus worth 2.8 list rows of staging — enough
to render ahead during idle so a fast encoder spin never stalls.

There is only **one** scroll region, so scrolling the list and scrolling a
banner are mutually exclusive without reprogramming VSCRDEF. Spend it on the
list.

## Hardware decisions (locked)

- **ESP32-S3 Supermini**, ESP32-S3FH4R2 — 4 MB flash, 2 MB PSRAM. R2 is *quad*
  PSRAM, sharing the flash bus, so GPIO 33–37 stay free; an R8 octal part would
  have consumed them. Chosen over the XIAO for GPIO headroom (~16 vs 11), which
  is what leaves room for the nRF24.
- **Rotary encoder with button.** `ui_list` runs in wrap mode.
- **Chip detect switch fitted.** `chip_reader` now defaults
  `has_detect_switch = true`, so a blank chip reads as ID 0 instead of being
  indistinguishable from an empty slot. Removal is signalled by the switch
  opening, not by the pads going open.

## Display driver

`st7735_esp32s3.c` — SPI plumbing around the host-tested geometry in
`st7735_panel.c`. Not yet run on hardware.

- **Framebuffer in internal SRAM, not PSRAM.** 32 KB fits easily in the
  S3FH4R2's 2 MB of PSRAM, but PSRAM reads go through the cache that a flash
  write suspends — the exact hazard the link task is built around — and DMA
  from PSRAM is slower. Spend the internal SRAM.
- **Dirt is tracked per row, not per rectangle.** Rows are contiguous in the
  framebuffer, so a run of dirty rows goes out as one DMA transfer. Rectangles
  would need one transfer per row.
- **Four pins**: SCK, MOSI, CS, DC. RST ties to EN, backlight to 3V3. MISO is
  not connected — the panel is write-only.
- `st7735_flush()` returns bytes sent. Log it during bring-up: if a cursor move
  pushes 24 KB, something is over-marking dirty.
- `st7735_scroll_rows()` handles the straddling case, writing the strip in two
  pieces when it crosses the end of the scroll region.

MADCTL is left without row/column swap on purpose. Rotating the scan would put
the scrolling axis across the list instead of along it, and the scroll region is
defined in memory rows.

## Correction: the chip reader needs 11 lines, not 21

Re-checked all 58 schematics per pad rather than per chip. No pad is shorted on
every chip, so there are no hidden ground pads — but the two families **overlap**,
which I missed:

| family | common | ID pads | bits |
|---|---|---|---|
| Advance / Progress | 10 | 1–9, 11 | 10 |
| Beast Link | 2 | 3–11 | 9 |

Union of all pads used, either family: **1–11, so 11 lines.** Pad 10 is Advance's
common and a Beast ID bit; pad 2 is Beast's common and an Advance ID bit. Since
only one chip is ever inserted at a time, both connectors can share the same 11
expander lines — each MCP23017 pin is individually input-with-pullup or
output-low, so the driver just drives whichever pad is common for the occupied
slot and reads the rest.

**One MCP23017 covers everything:** 11 shared pad lines + 2 detect switches = 13
of 16. Not two expanders, and not 21 lines.

Revised pin budget, ESP32-S3 Supermini:

| | pins |
|---|---|
| Link port (SC, SD, SI, SO) | 4 |
| ST7735 (SCK, MOSI, CS, DC) | 4 |
| I²C to the MCP23017 | 2 |
| Encoder A, B, SW | 3 |
| **subtotal** | **13** |
| nRF24 (SCK, MOSI, MISO, CE, CSN) | 5 |

The encoder stays on real GPIOs: it needs ~1 kHz sampling and an I²C round trip
per poll would drop transitions on a fast spin. The nRF24 can share SCK/MOSI
with the display if you accept that a flush blocks it, which brings it to 3
extra pins.

## Shared pad bus

`chip_slot.{h,c}` drives the eleven shared lines. Expander access is injected,
so the sequencing is host-testable including the failure paths.

Three rules the tests enforce, all of which matter on real copper:

- **Level before direction.** The output level is written before the pin becomes
  an output, so the line is never momentarily driven high against the other
  slot's pads.
- **Exactly one line driven, always released.** Every exit path — success,
  conflict, I/O failure — returns all eleven lines to input-with-pull-up. A
  common pad left driven would short against the other slot's chip the moment
  it is inserted.
- **Pads outside the family's ID set are forced high**, not left as read. A
  Beast chip leaves pads 1 and 12 unconnected, and a floating input must never
  become an ID bit.

Two chips seated at once returns `SLOT_SCAN_CONFLICT` rather than a reading.
One shared bus cannot serve both, and a blend of two chips decodes to a
plausible wrong ID — the worst possible failure. Refusing is the only safe
answer, and the UI should say "one chip at a time".

Test vectors come from real schematics — Gun_Del_Sol (pads 4, 10, 11), BugBomb
(2, 3, 4, 9), Advance_Base_Chip (no shorts) — so a decode regression surfaces as
a named chip going wrong rather than an abstract bit error.

## Chip screen and the mode shell

`ui_chipview.{h,c}` rebuilds the homebrew's chip screen behaviour on the packed
database; `ui_shell.{h,c}` is the mode picker in front of it. Neither touches
the display, so both are host-tested against the real `.chdb` blobs.

```
make run-chipview
```

Tab sizes come out matching the games:

| game | Standrd | Mega | Giga | Dark | PA |
|---|---|---|---|---|---|
| bn4r | 186 | 80 | 10 | grey | 1 |
| bn5p | 180 | 80 | 6 | grey | 30 |
| bn6g / bn6f | 200 | 54 | 5 | grey | 30 |

Dark being greyed on BN6 Gregar is correct, not a bug: the Gregar table leaves
those indices blank, which is exactly the "grey and not selectable" case the
homebrew documents.

### Row layout corrected

The earlier layout gave a column to MB and omitted **Number of Uses**. That was
backwards — uses is the field that governs play and the homebrew puts it on
every row, while MB only matters when building a folder. The row now follows the
homebrew's own arrangement, with the code box tinted by element rather than
spending a separate column:

```
> [icon] Name              [code] 3 *  |bar
  6  10   66                 10   8 8    4
```

MB moves to a detail view. `test_layout_fits` asserts the columns neither
overlap nor overrun the scrollbar.

### Behaviours worth knowing

- **Greyed tabs are skipped, not landed on.** Cycling walks past them and the
  loop is bounded by the tab count, so an all-grey mask cannot spin forever.
- **Un-starring inside the favourites tab drops the row**, and emptying the tab
  moves to a selectable one rather than stranding the user on a blank screen.
- **A custom order survives a rebuild.** Move mode sets `CHIP_SORT_CUSTOM`, and
  rebuild keeps the still-qualifying entries in their current order and appends
  anything new. Regenerating from database order would silently discard the
  user's arrangement — which the tests now catch.
- **Switching mode drops anything held**, so a picked-up chip cannot dangle
  into Slot-In mode.

### Mode shell

`Link · Send Program · Infrared · USB Storage · Self Test · (applets:
Battle Chip · Test Pattern · Chip Dice) · Settings · Diagnostics`, filtered by
`hw_caps`.

**Battle Chip** is an applet (`applets/battle_chip.c`) and includes what
used to be Read Chip. Choose a chip and send it, or put a real one in the
reader board and it goes out as a real gate would send it. It sends each
chip's wire ID (`chipdb_id_for_slot`). The built-in mode it replaces sent
the table index, which names a different chip for 203 of BN4's 285 sendable
chips.

**Link** opens a choice of interface: `USB - PC or mGBA`, `Wireless -
gbagate` or `Online - Celio`. It shows only what the board has, and a board
with only one goes straight to it. Back from a running link returns to that
choice. Online plays a partner anywhere through a Celio server with no PC:
the board joins your WiFi (set it up from its hotspot page or a `wifi.txt`
on the USB drive) and is its own Celio client. On the same WiFi it is also a
GB-Link adapter at `ws://<board>/gblink`. See `doc/ONLINE.md`.

**Applets** are modes added without touching the core: one C file in
`applets/` and a line in `applets/applets.def`. Battle Chip, Test
Pattern (panel calibration), Chip Dice and GBA Memory ship as examples;
`doc/APPLETS.md` is the walkthrough.

**GBA Memory** sends the memory agent (`rom/agent.c`) to a GBA at its boot
screen, and the board can then read, write, fill and call anything in the
GBA's memory over the link cable. Try it with Colour: the GBA's screen
changes. This is the first step of the multiboot preloader. See
`doc/AGENT.md`.

**Themes**: Settings' first row picks one of five palettes (Classic, Navi,
DMG, Contrast, Daylight). It applies at once and is saved. `test_screens`
checks every theme's contrast.

`app_mode_route()` names the screen every mode draws on, and `test_modes`
walks the whole menu, every row and every interface behind Link, to check
that none reaches a placeholder. That's how Infrared was found: listed, but
refused with "no handler".

**Diagnostics requires no capabilities at all**, on purpose: the screen that
explains why something is missing must not itself be gated on parts being
present.

Gate modes need a game before they can start, since both the gate identity and
the chip database are per-game, so selecting one advances to a game list rather
than starting. Backing out of a running mode lands on the game list rather than
the top — changing game is the usual reason to leave.

## Renderer

`make screens` renders every screen, in every state worth seeing and in
every theme, to `build/screens/gallery.png`; `doc/screens-all.png` is a
copy. `test_screens` asserts on them too: the frame, every line drawn, no
page line clipped, and a list of the titles, footers and menu rows that are.

`ui_render.{h,c}` draws the chip screen into a plain RGB565 array — no SPI, no
hardware — so `test_render` can render a screen, assert on the pixels, and dump
`.ppm` files to look at. `screens.png` shows four states side by side.

`ui_font.{h,c}` is a 5×7 bitmap font generated by `tools/make_font.py`, where
the glyphs are drawn as text and can actually be edited. An original design, not
a lifted one — the homebrew's font is Capcom's, which is what the font-patch
flow exists for.

```
make run-render
```

### Things the renderer gets right on purpose

- **Text clips by whole glyphs.** A half-drawn letter at a column edge reads as
  a rendering bug; a truncated word reads as truncation.
- **The tab strip is windowed.** Six tab names side by side need 132px and we
  have 128, so the active tab is shown by name with arrows plus six pips.
  Unavailable libraries get a dim pip rather than being omitted, so the strip
  does not change length as favourites appear and disappear.
- **A long footer status cannot overwrite the mode label.** It right-aligns
  when it fits and clips from the left edge of the free space when it does not.
- **Blank rows are still painted.** The framebuffer is not cleared between
  frames on hardware, so an unpainted band would show as a stale stripe.

### A testing note worth keeping

The first version of these tests sampled pixels at arbitrary coordinates and
four assertions failed — every one because the sample landed on glyph ink
rather than row background. The fix was a `row_bg()` helper that samples column
5, between the cursor glyph and the element block, below the font baseline.
Sampling anywhere else tests the wrong thing and passes or fails for the wrong
reason.

The element-tint test had the same shape of problem: it counted distinct icon
colours, which passes trivially on an all-grey render. It now asserts each
row's icon against `ui_element_colour()` of that row's actual record.

## Multiboot

`gba_multiboot.{h,c}` puts a program into a GBA's EWRAM over Normal 32-bit SIO,
using the BIOS download routine that runs at power-on with no cartridge. The
32-bit word exchange is injected, so the protocol is host-testable and only the
SPI plumbing is left to hardware.

```
make run-multiboot
```

### The constants

Taken from `shinyquagsire23/stlink_gba_multiboot`, a working implementation,
cross-read against GBATEK:

```
session key    m = 0x6F646573 * m + 1
word mask      w ^ (~(0x02000000 + offset) + 1) ^ m ^ 0x43202F2F
CRC            poly 0xC37B, seed 0xC387
```

The mask includes the **destination address**, so a word encrypted for the
wrong offset decrypts to garbage even with the correct key. Any implementation
missing that term will complete a transfer and then fail the final checksum
with no clue which step was wrong. `test_primitives` asserts each constant
literally so a typo cannot survive a refactor.

### The simulated GBA is not a stub

`test_multiboot.c` implements the *receiving* half: it picks a client byte,
derives the same session key from it, decrypts every word into a simulated
EWRAM image and computes its own CRC. A test passes only when the bytes landing
in EWRAM are byte-identical to the ROM **and** both sides independently arrive
at the same checksum.

```
result: ok, 572 words, client byte 0xD1
crc 0x398C
```

That is the property worth testing. A stub that answers "yes" would pass with
any constants at all.

### Failure paths covered

- **No console attached** → `MB_ERR_NO_GBA` after the configured attempt limit.
- **Console switched on late** → still succeeds; the sync is patient, not
  one-shot, which is the normal case since the user powers the GBA after
  choosing the ROM.
- **A corrupted word** → `MB_ERR_CRC`, and the two checksums differ.
- **A bogus offset echo** → `MB_ERR_STALLED`, aborting in under 200 words
  rather than sending the whole ROM and failing at the end. The GBA echoes the
  offset it has accepted in the high half of every reply; checking it converts
  a silent corruption into an immediately located failure.
- **Unaligned ROM** → padded to 16 bytes with zeros, without reading past the
  caller's buffer.

## ROM catalogue and USB staging

`rom_catalog.{h,c}` turns the USB drive's directory into a list `ui_list` can
show, validates each file before the user powers on a console, and stages a
chosen one into RAM. Filesystem access is injected, so scanning, validation,
truncation and staging are all host-testable.

```
make run-catalog
```

A simulated volume holding what a real drive actually accumulates:

```
aaa_first.gba         4096 B  ok
garbage.mb            4096 B  not a GBA image
huge_retail.gba    8388608 B  wrong size
MiXeDcAsE.MB          4096 B  ok
padded.mb            16384 B  ok
truncated.mb            64 B  wrong size
unreadable.mb         4096 B  unreadable
zelda_demo.mb         8192 B  ok
```

`notes.txt`, `README`, `.hidden.mb` and `._zelda_demo.mb` are filtered out
before listing — the last is a macOS resource fork, which any drive written
from a Mac will be full of and which would otherwise pass the extension test.

### Choices worth knowing

- **Bad files are listed with a reason, not hidden.** Someone who dragged a
  file across needs to know why it will not run. An 8 MB retail dump says
  "wrong size" rather than vanishing.
- **Size is checked before the header read**, so a 64-byte truncated download
  reports "wrong size" rather than "unreadable" — the header read would fail on
  it anyway, and the size is the useful diagnosis.
- **No Nintendo logo check.** That would mean embedding Nintendo's bitmap. The
  fixed byte at `0xB2` plus an ARM branch at the entry point already reject
  everything that is not a GBA image.
- **Trailing `0x00`/`0xFF` padding is stripped** and rounded back up to 16
  bytes, so a 16 KB padded file transfers as 2.3 KB. The round-up means
  stripping can never eat real trailing data.
- **A short buffer is refused, not partially filled.** `mb_send()` reads whole
  words to the padded end, so the staging buffer must hold `mb_padded_size()`,
  not just the file.

### The staging rule this exists to enforce

Reading from FATFS triggers SPI flash sector lookups, and flash access suspends
the instruction cache — the same hazard the link task is built around. A stall
midway through clocking multiboot words corrupts the transfer.

So `rom_stage()` reads the whole file into RAM and closes the handle, and
`mb_send()` never sees a file handle. Keep it that way.

## Header validation and repair

The GBA **does** verify the Nintendo logo during multiboot. That makes a wrong
logo the worst failure shape available: the transfer completes, the console
rejects it, and there is no diagnostic. Slow, silent, and easy to blame on the
adapter.

### The fingerprint

We cannot ship Nintendo's bitmap. We do not need to — a **CRC32 of bytes
`0x04..0x9F` is a 4-byte fingerprint, not a reproduction of the artwork**. The
reference comes from a dump the user already owns:

```
python3 tools/fix_header.py --fingerprint mygame.gba > logo_crc.h
```

`rom_catalog_scan()` takes it as a parameter and flags mismatches as
`ROM_BAD_LOGO`, so the user is told *before* powering on a console.
`rom_stage()` refuses those files outright.

**Zero means "unknown" and disables the check.** A device with no reference must
not reject every file: a false rejection is worse than the failure it would have
prevented.

The fingerprint covers the logo area and nothing else, so editing a title or
rebuilding a ROM does not move it — otherwise every rebuild would look like a
bad logo. There is a test for exactly that.

### Other header checks

- **Complement byte at `0xBD`**, `0x00 - (0x19 + sum of 0xA0..0xBC)`. Derived
  from the header's own contents, so it costs nothing and catches the most
  common breakage: a title edited by hand without recomputing it.
- **Fixed byte `0xB2 == 0x96`** and an **ARM branch at the entry point**.
- Size within `MB_MIN_SIZE`..`MB_MAX_SIZE`, checked *before* the header read so
  a 64-byte truncated download reports "wrong size" rather than "unreadable".

### tools/fix_header.py

```
python3 fix_header.py --check *.mb
python3 fix_header.py broken.mb                      # checksum, no donor needed
python3 fix_header.py --donor mygame.gba broken.mb   # logo too
python3 fix_header.py --fingerprint mygame.gba > logo_crc.h
```

```
FIX   no_logo.mb [DONORGAME]
        - fixed byte 0xB2 is 0x00, want 0x96
        - logo area is blank
        + copied logo from donor
        + set fixed byte 0xB2
        + recomputed complement byte to 0xB4
        -> no_logo.fixed.mb
```

- A donor is needed only for the logo; checksum repair stands alone.
- The donor is validated first, so a broken donor cannot propagate its breakage.
- The checksum is written last, because it is computed from the bytes above it.
- Repairs are re-checked after writing, so a partial fix cannot look like
  success.

### Both implementations are cross-checked

Checksum and fingerprint each exist twice, in C on the device and Python in the
tool. Disagreement would be worse than no check: the tool would "repair" files
the device then rejects. So both are fuzzed against each other over thousands of
random headers, and the tool's output is fed back through the device's own
`rom_validate()`.

```
tool:   #define ROM_LOGO_REFERENCE_CRC 0xFBF64CCCu
device: rom_logo_fingerprint()      -> 0xFBF64CCCu
```

### A bug worth recording

The first version of the logo check stored the reference in `rom_catalog_t` and
had `rom_catalog_scan()` read it *before* zeroing the struct. Sanitiser builds
passed because the uninitialised memory happened to be zero; at `-O2` it was
not, and **every ROM silently failed the logo check**.

The fix was to make the reference an explicit parameter rather than a field the
caller pre-sets, so it cannot be read uninitialised. The catalogue tests now run
at `-O0`, `-O2`, `-O3 -flto` and under ASan/UBSan, because this class of bug
only appears at some of them.

## Transport bridge

`link_bridge.{h,c}` is the piece between the wire protocol and a transport. It
owns the receive state, runs the HELLO negotiation, routes each packet type,
measures round-trip time, and recovers from a peer that disappears and returns.

The transport is a single "send these bytes" callback plus one flag, so the same
object runs over USB CDC (byte stream, SLIP framed) and ESP-NOW (datagrams,
unframed).

```
make run-bridge
```

### The loopback is deliberately hostile

Two bridges wired back to back through a wire that can fragment a stream at any
boundary, drop datagrams, and corrupt bytes — because USB fragments, ESP-NOW
drops, and radios corrupt. A bridge that only works on a clean wire is not
finished. The handshake and a word transfer are re-run at fragment sizes 1
through 5; one byte at a time is the worst case a USB stack can hand you.

### Decisions worth knowing

- **A HELLO is answered with an ACK, not another HELLO.** Otherwise two devices
  powering on together trade HELLOs forever. The responder adopts the
  initiator's sequence, so both agree from the first transfer rather than
  discovering a mismatch later.
- **A version mismatch is refused.** A peer speaking a different version is
  worse than no peer, because it would half-work. The bridge stays down and
  keeps announcing.
- **PONG echoes the original stamp** instead of restamping, so the measurement
  is a true round trip and needs no clock agreement between the two ends. The
  subtraction is unsigned, so a wrapped microsecond counter still gives the
  right interval — there is a test for that.
- **Silence past the timeout drops the session.** A stale session would let
  stale words look current.
- **No retransmission.** A word that does not arrive becomes an underrun and
  the slot reads as absent — a state the GBA hardware already produces.
  Retransmitting would add latency to fix something already handled.
- **Words and reports are refused while the link is down**, rather than
  silently vanishing into a dead transport.

## ESP-IDF glue

Three files that cannot be host-tested, kept deliberately thin so the untestable
surface stays small. **None has been run on hardware.**

| file | what it does |
|---|---|
| `usb_msc_esp32s3.c` | FAT volume over USB MSC, plus `rom_io_t` over FATFS |
| `transport_esp32s3.c` | USB CDC and ESP-NOW backends for `link_bridge` |
| `mb_spi_esp32s3.c` | Normal 32-bit SIO master for `gba_multiboot` |
| `partitions.csv` | 2 MB app, 1.5 MB FAT volume |

### The FAT ownership rule

A FAT volume has exactly one owner. If the host is mounted and the firmware
writes a byte — or the reverse — the host's cached allocation table and the real
one diverge, and the volume is corrupt, often invisibly and often much later.

So the partition is handed back and forth explicitly on TinyUSB's mount-changed
callback, and `msc_storage_available()` is the single place that answers "may I
read a file right now". Everything touching the filesystem goes through it. A
rescan while the host owns the volume returns false, which the UI should show as
"connected to PC" rather than an empty list.

Handing over on *mount* rather than on cable insertion matters: a host can
enumerate without ever mounting, and can keep a volume mounted long after the
user thinks they are done.

### Multiboot is the easy one

During multiboot the adapter is the SPI **master**, so a CPU stall stretches a
clock period and the GBA waits. That is the opposite of the Multi-Play PHY,
where we may be the child and a stall corrupts a frame outright — and it is why
multiboot can use the ordinary SPI driver instead of a pinned bit-bang loop.

This also corrects something stated earlier: the reason `rom_stage()` reads the
whole file before the handshake is *not* that a cache stall would corrupt the
transfer. It is SPI bus contention if storage and link share a host, keeping
filesystem error paths out of the transfer, and paying a read cost once instead
of 65,000 times. The header comment in `rom_catalog.h` has been corrected.

### Marked for the scope

- **Bit order in `mb_spi_esp32s3.c`.** MSB-first matching the reference
  implementations, but a wrong guess fails as "sync never completes" rather than
  anything diagnostic. Try the byte swap before suspecting the constants.
- **The 36 µs inter-word gap.** The BIOS needs a gap; without it sync appears to
  work and the data phase drifts.
- **2 MHz stays off.** GBATEK notes it is intended only for hardware attached
  directly to the port with no cable in between, so a cable-connected adapter
  should stay at 115200 until measured.

### Core placement

Transports run on core 0 with WiFi, BLE, USB and the display. Core 1 belongs to
link timing and never blocks on a transport: a radio stalling 3 ms is normal, a
link frame stalling 3 ms is a dropped slot. `link_session`'s ring is the only
thing crossing between them, and `transport_tick()` is called from the UI loop
rather than the link task because it can emit a packet.

## Build variants

Three images, one target: **ESP32-S3 only**. Features and their dependencies are
a table checked on a host by `variant_resolve()`, not scattered `#ifdef`s —
because as `#ifdef`s the question "does this combination actually work" has
nowhere to live.

```
make run-variant
```

```
link  wireless link cable and Wireless Adapter emulation
      display, encoder, link parent, ESP-NOW, USB serial, adapter emulation
gate  everything except physical chips and IR
      ...plus chip gate, all databases, link child, USB storage, multiboot
full  every feature
      ...plus chip reader, infrared, nRF24
```

| image | modules | lines | chipdb |
|---|---|---|---|
| link | 11 | ~2450 | — |
| gate | 20 | ~4243 | 72 KB |
| full | 23 | ~4593 | 72 KB |

### Why the S2 went, and why it is worth recording

An earlier revision targeted the S2 as well, with a platform dimension and
capability pruning to go with it. Dropping the S2 removed the only thing that
dimension ever expressed, so it is gone rather than left as scaffolding holding
nothing up.

The reason it went is the constraint the whole architecture rests on. The link
roles divide by who controls the clock:

- **parent** — we choose when a transfer starts, so a stall stretches a gap and
  the GBA waits.
- **child** — our word must be staged before the parent clocks it, so a stall
  corrupts the frame outright.

The child role needs a core that WiFi and USB cannot steal, and a Battle Chip
Gate is a child by definition. On a single-core part the gate is not a reduced
feature — it is an impossible one.

### What the tests prove

- Every feature in every image has its prerequisites. A table edit made in a
  hurry fails here rather than on a bench.
- The tiers nest strictly: `full ⊃ gate ⊃ link`. Moving up cannot silently lose
  a feature.
- **Nothing required depends on optional hardware.** The nRF24 is compiled in
  and gated at boot by `hw_caps`, so an unpopulated footprint runs the same
  image — but if some mandatory feature depended on it, an unpopulated board
  would break something that looks mandatory. That is checked.

## Building: PlatformIO + ESP-IDF

```
pio run -e gate                build
pio run -e gate -t upload      flash
pio run -e full -t monitor
make                           the host suites (from the repo root), no hardware
```

`framework = espidf`, not Arduino. The Arduino core would fight us on exactly
the thing this project cares about: it owns task creation and core affinity,
and the link task must have core 1 to itself.

### The three settings that matter

```
CONFIG_FREERTOS_UNICORE=n        core 1 for the link, core 0 for everything else
CONFIG_SPI_FLASH_AUTO_SUSPEND=y  a flash write on core 0 must not stall core 1
CONFIG_FREERTOS_HZ=1000          the link task yields one tick between transfers
```

`SPI_FLASH_AUTO_SUSPEND` is the single most important one. Without it, saving a
folder corrupts whatever frame is in flight — the IRAM discipline in the source
handles the code side, but a suspended cache still stalls anything not resident.

`CONFIG_FATFS_LFN_HEAP` matters more than it looks: FAT volumes written by
desktops use long names, and without LFN the drive shows mangled 8.3 names and
`rom_catalog` cannot match extensions reliably.

### FreeRTOS layout

| task | core | priority | notes |
|---|---|---|---|
| `link` | **1** | max | IRAM resident, busy-waits on clock edges |
| `ui` | 0 | 5 | display, encoder, menus |
| `transport` | 0 | 4 | `bridge_tick`, which can emit a packet |
| storage | 0 | 3 | FATFS, only while USB is detached |

Nothing crosses between the cores except `link_session`'s ring and one queue of
UI events. Every additional shared structure is another chance for core 0 to
make core 1 wait, and core 1 waiting is a corrupt frame.

Posting from core 1 uses `xQueueSendFromISR` and **drops rather than blocks**: a
full UI queue must never stall a transfer. The drop count is surfaced on the
diagnostics screen so a persistently full queue is visible rather than felt.

### One thing to check before trusting the build

USB MSC via `tusb_msc_storage` needs **ESP-IDF 5.x**. The official
`espressif32` platform has historically pinned older IDF releases, so confirm
what `pio run -e gate -v` reports. If it is 4.x, point `platform` at a fork
shipping 5.x rather than trying to backport TinyUSB.

## Licence

gbagate is free software under the GNU General Public License, version 3
only (`GPL-3.0-only`); the full text is in [LICENSE](LICENSE).

- `third_party/gblink-firmware/` is GB-Link's adapter firmware, under its own
  GPL-3.0 licence (the same text, in that directory). The shim files derived
  from it (`port/gblink/link_layer_core.c`, `rfu_link_core.c`,
  `serial_layer.cpp`) say so. The hardware images include it, and are
  GPL-3.0 as a whole.
- pret's decompiled librfu and GB-Link-Switch-LDN (AGPL-3.0) are not part of
  gbagate: the tests fetch them into `build/` and nothing of them is
  committed or shipped.
