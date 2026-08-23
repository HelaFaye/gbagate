# Project State — Battle Chip Gate / GBA Link Adapter

**Date:** 20 August 2026
**Target:** ESP32-S3 (S3FH4R2, 4 MB flash / 2 MB quad PSRAM) · ST7735 128×128 ·
rotary encoder with button · MCP23017 · chip detect switches · optional
nRF24L01+PA/LNA
**Build:** PlatformIO, `framework = espidf`, three images

```
portable + headers   5412 lines    19 modules
tests                4042 lines    14 suites, 618 assertions  (74% of portable)
esp-idf glue         1909 lines     8 files   UNTESTED
tools                1046 lines     5 tools
                    ------
total               12409 lines
```

**14 suites × 4 build configurations (`-O0`, `-O2`, `-O3 -flto`, ASan/UBSan) —
56 of 56 passing.**

---

## 1. The three images

| image | for | features |
|---|---|---|
| **link** | wireless link cable and Wireless Adapter emulation | display, encoder, link parent, ESP-NOW, USB serial, adapter emulation |
| **gate** | everything except physical chips and IR | + chip gate, all six databases, link child, USB storage, multiboot |
| **full** | everything | + chip reader, infrared, nRF24 |

Feature dependencies are a table validated on a host, not scattered `#ifdef`s.
The tiers nest strictly (`full ⊃ gate ⊃ link`), and nothing mandatory depends on
optional hardware — both asserted.

**ESP32-S3 only.** The S2 was dropped: the link child role needs a core that
WiFi and USB cannot steal, and a Battle Chip Gate is a child by definition. On a
single-core part the gate is not a reduced feature, it is an impossible one.

---

## 2. Features, and how far each one is proven

### Working and host-verified

| Feature | State | What proves it |
|---|---|---|
| **Battle Chip Gate responder** | complete | Handshake, 9-slot rotation, 9-cycle chip repeat, 4 gate identities. Two independent derivations agree — the homebrew disassembly and mGBA's `battlechip.c` |
| **Chip database** | complete | 6 games, 411/424/389 chips, 72 KB total, byte-reproducible builds |
| **Wire-ID mapping** | BN4/5 ~93%, BN6 61% | Derived from mGBA's own name lists; identity verified rather than assumed |
| **Chip screen** | parity with the homebrew | 6 tabs, 4 sorts, favourites, Slot-In/Move, uses column, scrollbar |
| **Renderer** | complete | Pixel-level assertions plus a visual check (`screens.png`) |
| **Mode shell** | complete | Modes gated by detected hardware; diagnostics always reachable |
| **Rotary encoder** | complete | Quadrature with structural debounce, acceleration crossing 411 chips in 25 detents |
| **Display geometry** | complete | Panel offsets, hardware vertical scroll: one row costs 3072 B against 24576 B |
| **Chip reader** | complete | 11 shared pad lines, one MCP23017, bus discipline enforced |
| **Multiboot** | complete | Validated against a simulated GBA that decrypts and CRCs independently |
| **ROM catalogue** | complete | Validation, padding strip, RAM staging, hostile filename cases |
| **Header repair** | complete | Two implementations fuzzed against each other |
| **Transport bridge** | complete | Hostile loopback: fragmentation, drops, corruption |
| **Pacing / lockstep** | complete | Stall budget, underrun accounting, 16-bit sequence wrap |

### Written, never run

| Feature | Risk |
|---|---|
| **Multi-Play PHY** | Framing from GBATEK; **no timing measured** |
| **ST7735 driver** | SPI/DMA plumbing, DMA flush, scroll path |
| **USB MSC storage** | FAT ownership handover |
| **USB CDC / ESP-NOW transports** | Thin, but unrun |
| **Multiboot SPI master** | Bit order and inter-word gap unconfirmed |
| **FreeRTOS task layout** | Core pinning and priorities |

### Designed but not built

Wireless Adapter emulation toward the GBA, infrared accessory protocol, mGBA
SIO driver upstream, font patch flow.

### Closed, deliberately

- **OEM Wireless Adapter interop.** The RFU radio is a Freescale MC13190 doing
  ~4 Mbps ASK at 2432 MHz; the nRF24 is GFSK-only, caps at 2 Mbps, and its
  packet engine cannot be bypassed. Three independent blockers. The nRF24 stays
  as a *second radio*, which is a different and still-valid argument.
- **Virtual ROM streaming of retail games.** Requires trapping cartridge
  fetches; the ARM7TDMI has no MMU or fault mechanism.

---

## 3. What was established, and from where

| Finding | Source |
|---|---|
| Gate IDs `0xFFC6/C7/C4/FF00`, 9-slot rotation | Homebrew disassembly + mGBA `battlechip.c` |
| Wire ID = 1-based line number in `chip-names.txt` | mGBA `BattleChipModel.cpp` |
| 18-bit Multi-Play framing; parent inserts delays | GBATEK |
| Multiboot constants `0x6F646573`, `0xC37B`, `0x43202F2F` | `stlink_gba_multiboot`, cross-read against GBATEK |
| Chips are passive shorts; **11 shared pad lines** | All 58 EXE project schematics |
| Chip Gate Use Limit is a real table field | mmbn-chip-tables |
| RFU is MC13190, `SIOCNT 0x5003`, 2432 MHz | GBATEK + MC13190 datasheet |
| GBA verifies the logo during multiboot | Confirmed by you |

---

## 4. Corrections made along the way

Recording these because each one was wrong in a way that would have cost bench
time.

1. **Chip reader needs 11 lines, not 21.** The two chip families share pads —
   pad 10 is Advance's common *and* a Beast ID bit. One MCP23017 covers both.
2. **A child *can* signal not-ready** by briefly leaving multiplayer mode. "No
   backpressure exists" was too strong; it is crude and optional, but real.
3. **The staging rule's justification was overstated.** During multiboot we are
   the SPI master, so a cache stall stretches a clock rather than corrupting a
   frame. The rule stands; the reason is bus contention and error-path
   isolation.
4. **The row layout had MB where uses belongs.** Uses governs play and the
   homebrew puts it on every row; MB only matters when building a folder.
5. **The logo check is right to skip on-device** — but a CRC32 fingerprint is
   not the artwork, and once you confirmed the console verifies it, that became
   worth adding.

---

## 5. Audit findings, all fixed

Six defects, **all six in the untested 24%**:

| # | Defect | Would have |
|---|---|---|
| D1 | `msc_read_all()` refused a 256 KB program | shipped silently |
| D2 | Mount config built then discarded | shipped silently |
| D3 | Missing `esp_partition.h` | failed to compile |
| D4 | Nonsense SPI flag expression | compiled, meant nothing |
| D5 | Renderer trusted an unenforced invariant | drawn wrong chips plausibly |
| D6 | Doc correction applied to delivery, not source | drifted |

Inspection alone found four defects in 1,909 lines of glue, while the last three
portable modules produced none. That ratio is the most useful number here.

---

## 6. Open questions

| # | Question | How to close |
|---|---|---|
| 1 | PHY edge timing at 115200 | Logic analyser |
| 2 | **Pad bit order — which pad is MSB** | **One known chip + multimeter** |
| 3 | Does `ack_word` equal the gate ID | Log non-`0x8FFF` words in SYNCED |
| 4 | Multiboot bit order (MSB vs LSB first) | Fails as "sync never completes" |
| 5 | Multiboot inter-word gap (36 µs assumed) | Scope |
| 6 | ST7735 offsets and BGR/RGB | Calibration pattern |
| 7 | ESP-IDF version PlatformIO pins | `pio run -e gate -v` |
| 8 | Per-game stall budget | Counters on the diagnostics screen |
| 9 | BN6 wire IDs above 320 | Cosmetic; compare against bn6f |

Question 2 needs one physical chip and a multimeter and could be done today.
1, 3, 4, 5 and 6 all resolve in a single bench session. Question 7 needs only a
verbose build.

---

## 7. Risks

**High — the PHY has never been measured.** Everything on hardware depends on
it. Mitigated structurally: the layers above are transport-agnostic, so a PHY
rewrite costs nothing above it.

**High — 2 MHz for adapter emulation.** `SIOCNT 0x5003` is 0.5 µs/bit, ~120 CPU
cycles at 240 MHz, with clock direction switching mid-session. Seventeen times
tighter than Multi-Play. It is in all three images; if it proves unworkable it
comes out of the table, not out of every source file.

**Medium — IRAM discipline.** Nothing enforces it at compile time.
`CONFIG_SPI_FLASH_AUTO_SUSPEND=y` covers the cache side; the code side is
convention.

**Low — power.** Link port pin 1 gives 3.3 V at milliamps. Separate supply,
common ground, pin 1 disconnected in accessory mode.

---

## 8. Recommendation

Host-side work has reached diminishing returns. The last audit found defects
only where hardware is required to verify, which is the signal to stop writing
and start measuring.

1. **Chip reader with one known chip** — closes question 2, needs no scope.
2. **Breadboard the link port and scope it** — closes 1, 3, 4, 5.
3. **Display calibration pattern** — closes 6.
4. **`pio run -e gate -v`** — closes 7, costs a minute.

Adding further modes before that session multiplies untested surface against an
unverified foundation.
