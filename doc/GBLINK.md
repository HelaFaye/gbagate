# gbagate as a GB-Link adapter

A gbagate board is a [GB-Link](https://gblink.io) adapter. It runs GB-Link's
own adapter firmware (2.2.7, vendored in `third_party/gblink-firmware`) on the
ESP32-S3, on a shim of the Zephyr calls that firmware makes. To every GB-Link
host it looks like an RP2040 GB-Link: the same USB IDs, the same version, and
the same commands, modes and status words. GB-Link's web apps, its netplay
bridge, Celio and the Switch bridge drive it unchanged.

On top of that it adds a few things of its own (the gbagate extensions
below). They use command and mode numbers GB-Link does not use and its hosts
never send. Its fixes for upstream defects are offered back as patches in
`backport/`.

**Nothing in this document has run on hardware yet.** `doc/BRINGUP-GBLINK.md`
is the bench procedure, step by step. Everything above the
bit level is tested in the host simulator, against GB-Link's own host code and
pret's decompiled librfu. Where the bit level matters, the code says how it
was derived and what has not been measured.

---

## What it speaks

| | |
|---|---|
| USB IDs | VID 0x2FE3, PID 0x000A, bcdDevice 0x0227 (GB-Link 2.2.7) |
| Strings | manufacturer "gbagate", product "GBLink USB", serial from the MAC |
| Interface 0 | vendor class, four bulk endpoints: EP1 OUT commands, EP1 IN status (2 bytes LE), EP2 OUT/IN data (64 bytes) |
| Interfaces 1-2 | CDC-ACM, for WebSerial (Firefox), with the `'GB'` framing: `47 42 | ch | len LE | payload`, channel 0 command, 1 data, 2 status, 3 gbagate's own |
| WebUSB | BOS with landing page launcher.gblink.io (toggled by 0x44, as upstream) |
| MS OS 2.0 | function subset on interface 0, WinUSB, GUID {FA611CC3-7057-42EE-9D82-4919639562B3}, so Windows needs no driver |

The descriptors come from `src/gblink_usbdesc.c`. The compat suite's
fake WebUSB is built from the bytes that file produces, so a test that passes
also checks the descriptors.

### Modes (SetMode `[0x00, mode, variant]`)

| mode | what | gbagate notes |
|---|---|---|
| 0x00 | Trade emulator (Pokemon Gen 3) | |
| 0x01 | GBA link relay (Celio, netplay bridge) | |
| 0x02 | GB/GBC SPI link | needs a 5 V level shifter, see Wiring |
| 0x03 | GB printer | likewise |
| 0x04 | Advance Wars 1/2 (variant 1/2) | |
| 0x05 | e-Reader (variants 1-13, including Emerald US/JP/FR/DE) | |
| 0x06 | Battle Chip Gate (upstream's, as megaman.gblink.io drives it) | |
| 0x07 | Wireless Adapter (AGB-015) emulation | see below |
| 0x80 | gbagate's own Battle Chip Gate | extension |

---

## Wireless Adapter (mode 0x07)

Upstream emulates the AGB-015 with gpsp's protocol core. Network traffic
travels as "RFU1" frames in 64-byte packets on the data channel. Three things
can carry them:

- **a PC**: the netplay bridge (`GBLinkRfuDevice` + `RfuLinkBridge`) over
  RetroArch netplay;
- **a Switch**: a GB-Link-Switch-LDN board on the bridge UART;
- **another gbagate**, over the air, with no PC at all (below).

The wire is the hard part. The GBA runs 32-bit Normal mode at up to 2 MHz
(250 ns half periods) and expects its answer to the next word staged within
about 40 µs during the ID exchange. The RP2040 does this with two PIO
programs. gbagate does it with a task on core 1 that shifts each word edge by
edge, using **dedicated GPIO**: pins bundled onto CPU registers, read and
written by single instructions. The ordinary GPIO registers sit behind the APB
and take hundreds of nanoseconds per access, which is too slow for this.
GB-Link-Switch-LDN measured about 403 ns on its boards.

The loop masks interrupts only while it is watching a transfer. It relies on
librfu's handshake: in the command phase the GBA starts a word only after SI
has followed its SO. So the loop holds SI low ("not ready") whenever it is not
watching, and the GBA waits rather than losing a word. The details are in
`port/esp32s3/rfu_link_esp32s3.c` and `port/gblink/rfu_phy.h`.

The game's soft-reset pulse on SD (about 1.1 ms) is timed from its edges, not
caught at a poll.

**Use a GBC link cable** (or plug straight into the GBA). A GBA cable grounds
the line the adapter receives on. Upstream says the same, and the netplay
bridge warns about it.

### Testing it: the game's own code

The simulator models the GBA's serial port, timers and interrupts
(`test/gblink_sim/sim_rfu.c`). It runs **pret's decompiled librfu** on that
model as the game: the STWI command layer, the SIO interrupt handler and the
Sio32ID exchange that FireRed, LeafGreen and Emerald link in. The tests fetch
librfu from pret/pokefirered at a pinned commit (`fetch_librfu.sh`) and do not
redistribute it. Without it, these tests skip.

- `test_rfu.py`:
  - checkID converges on 0x8001 against the adapter's ID dance;
  - the status REQs answer as an AGB-015 does;
  - a soft reset restarts the adapter;
  - MS_Change hands the clock to the adapter, which hands it back;
  - two adapters joined only by their RFU1 streams host a room, find it,
    connect, and pass data both ways.
- `test_peer.py`: the same, between two boards over the ESP-NOW cable.
- `test_rfu_netplay.mjs`: the same, with GB-Link's netplay bridge engine at
  both ends of a RetroArch netplay session.

---

## gbagate's extensions

### 0x4F GET_EXTENDED_INFO

A hardware-range command, so it is answered in every mode. A GB-Link has no
0x4F and stays silent, so a timeout means "a GB-Link".

Reply, 16 bytes:

| bytes | contents |
|---|---|
| 0 | 0x4F |
| 1-3 | `GBX` |
| 4 | extension version 1 |
| 5-7 | gbagate firmware version (0.4.0) |
| 8-11 | capabilities, u32 LE |
| 12 | board: 0 host simulator, 1 devkit, 2 Supermini |
| 13 | image: 0 link, 1 gate, 2 full, 3 mini |

Capability bits:

| bit | capability |
|---|---|
| 0 | chip gate (mode 0x80) |
| 1 | second SD path fitted |
| 2 | RGB LED |
| 3 | VCC switch |
| 4 | display |
| 5 | storage |
| 6 | ESP-NOW |
| 7 | standalone multiboot |
| 8 | bridge UART wired |

The layout is in `src/gbx_proto.h`.

### 0x4E GET_PHY_STATS

The link drivers' own counters, for bring-up, answered in any mode. The reply
is `[0x4E, 1]` followed by u32 counters:

- Multi-Play words completed, dropped and answered late;
- Wireless Adapter words, and words abandoned mid-way;
- words since the link was configured;
- uptime.

`tools/gblink_bench.py` reads them. `doc/BRINGUP-GBLINK.md` says what they
should do.

### Mode 0x80: gbagate's Battle Chip Gate

This is gbagate's own gate (`src/chipgate.c`), which also runs standalone from
the board's menu. Over USB it uses these commands:

| command | arguments | reply |
|---|---|---|
| 0x50 InsertChip | id lo, id hi | `[0x50, 1]` accepted, `[0x50, 0]` refused while a chip is still sending |
| 0x51 RemoveChip | | |
| 0x52 GetState | | link state, slot, chip, game, sessions, frames |
| 0x53 SetGame | 1 BN4, 2 BN5, 3 EXE6 JP, 4 BN6 US/EU | |

When a chip has finished sending, the board says so unprompted with
`[0x54, id lo, id hi]`. Sessions are reported as LinkConnected and
LinkReconnecting, and a game that stops clocking for a second ends the session.

Upstream's own gate (mode 0x06) is there as well. The two use different
variant numbers, so check which one a host expects.

### Link: Wireless, the cable with no cable

Two gbagate boards can run GB-Link's relay between themselves over ESP-NOW
(`src/gbl_peer.c`). Each board plays Celio's part for its own firmware: the
larger random nonce becomes master, it waits for both handshakes, and it
relays "connected" from one side to the other. Status and data travel on a
reliable, ordered channel (sequence numbers, cumulative acks, go-back-N
retransmission).

The **air link** setting picks what the cable carries:

- **cable**: a Gen 3 link cable (mode 0x01);
- **adapter**: a Wireless Adapter (0x07), the Union Room with no PC or Switch;
- **AW1** or **AW2**: Advance Wars.

Both boards must agree: HELLO carries the mode, and a board in another mode is
not taken for a peer. When that happens the screen says "peer: other link".

---

## Bridge UART (GB-Link-Switch-LDN)

GB-Link 2.2.7 also carries its frames on a second serial port to a companion
MCU, at 921600 baud. On a devkit this is UART2, TX on GPIO 47 and RX on GPIO 2.
Build it in with:

    BOARD_HAS_BRIDGE=1 tools/build_hw.sh gate

Wire it like the RP2040's GP8/GP9, crossed: our TX to the companion's RX. A
GB-Link-Switch-LDN board on these pins drives mode 0x07 exactly as it drives
an RP2040 GB-Link. The Supermini has no free pins for it.

---

## Using it with GB-Link's software

| GB-Link software | status | tested by |
|---|---|---|
| gblink-netplay-bridge (engine) | gen3, AW, rfu all work | `test_adapter.mjs`, `test_netplay_e2e.mjs`, `test_rfu_netplay.mjs` |
| Celio-Server + Celio-Client | relay works with the client from before b057315 (see below) | `test_celio.mjs` |
| gblink-ereader | e-Reader mode, preload, scan | `test_webapps.mjs` |
| gb-pokemon-web | GB link mode | `test_webapps.mjs` |
| gblink-distributor | GB link entry, byte exchange | `test_webapps.mjs` |
| gblink-launcher | identifies it, LED/landing/cable settings work; **do not use its firmware updater** (below) | `test_launcher.mjs` (in Chromium) |
| GB-Link-Switch-LDN | bridge UART, `BOARD_HAS_BRIDGE=1` | `test_gblink.py` bridge_uart_companion |
| megaman.gblink.io | mode 0x06 as upstream | `test_gblink.py` chip_gate_upstream_mode (the page itself could not be fetched to test) |

Run all of it with `make compat`. It needs node and npm, and network access the
first time.

### Things to know

- **The launcher's firmware updater is not for this board.** It offers
  RP2040 `.uf2` builds. Its one-click update sends 0x43, which on an ESP32-S3
  enters the ROM serial bootloader, and then waits for a PICOBOOT device that
  never appears. Power-cycle the board to leave the bootloader.
  `backport/gblink-launcher` teaches the launcher to recognise gbagate and hide
  the updater. Until that is merged, flash gbagate with `pio run -t upload` or
  esptool.
- **Celio.** The newest public Celio-Client (b057315, 3 October 2026) sends
  with Socket.IO acks and retries. The newest public Celio-Server (9f02d3e) does
  not answer acks. The two current heads therefore do not interoperate, with
  any adapter. The relay test uses the client from just before that change.
- **USB is GB-Link or a drive, not both.** The ESP32-S3 has four IN endpoints
  besides EP0, and GB-Link's interface plus CDC use them all. The `usb` setting
  chooses one, and the board reboots to apply it.

---

## Wiring

The link port's SC, SD, SI and SO lines go to the pins in
`port/esp32s3/board.h`.

GB-Link's firmware knows two SD paths: the GBA cable's, and the one a generic
GBC cable uses. The GB-Link DIY board routes both to the Pico. Wire
`PIN_LINK_SD_ALT` to the connector the same way and build with
`BOARD_HAS_SD_ALT=1` to get the cable auto-detection, the 0x49-0x4B cable
settings and the wrong-pin watchdog. A board without it reports a GBA cable
whatever is plugged in, which is the truth for that board.

**Game Boy and Game Boy Color links run at 5 V.** The ESP32-S3's pins are not
5 V tolerant. The GB SPI and printer modes (0x02, 0x03) need a bidirectional
level shifter between the board and the port, as the GB-Link DIY board has
(a SparkFun BOB-12009). The GBA modes are 3.3 V.

---

## Licences

`third_party/gblink-firmware` is GB-Link's code under GPL-3.0, and everything
built from it is too. The hardware images that include it are therefore
GPL-3.0 as a whole.

GB-Link-Switch-LDN is AGPL-3.0. gbagate does not include any of it; the bridge
UART only talks to a board running it.

pret's librfu is neither part of gbagate nor redistributed with it. The tests
fetch it.
