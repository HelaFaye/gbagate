# Audit

## Round 3 — 20 October 2026: the GB-Link adapter

**Scope:** the GB-Link personality. That is GB-Link's vendored firmware
(2.2.7), the Zephyr shim it runs on, the Multi-Play and Wireless Adapter link
layers and their ESP32-S3 drivers, the USB personality, the wireless cable,
and the build.
**Method:**

- GB-Link's own host software (netplay bridge, Celio, web apps, launcher),
  unmodified, against the firmware in a simulator;
- pret's decompiled librfu as the game, on a model of the GBA's hardware;
- ASan/UBSan throughout;
- a build of every hardware image.

**Result: 16 defects found and fixed: 6 in GB-Link's firmware, 10 in
gbagate's.** All suites pass, and so do all compat suites. Nothing has run on
hardware.

### Verification performed

| Check | Result |
|---|---|
| 28 unit suites × `-O0`, `-O2`, `-O3 -flto`, ASan/UBSan | all pass |
| `make gblink`: 31 GB-Link mode checks, 9 Wireless Adapter (librfu) checks, 7 wireless-cable checks, 6 bench-script checks | all pass; `make gblink-asan` runs all four suites again under ASan and UBSan, every report fatal |
| `make compat`: netplay bridge (adapter, gen3 netplay, rfu netplay), Celio relay, 4 web-app checks, 7 launcher checks in Chromium, GB-Link-Switch-LDN's link task (5 checks), at pinned upstream commits | all pass |
| Hardware images: link, gate, full, mini; devkit and Supermini; gate with the bridge UART | all build (ESP-IDF 5.5) |
| QEMU image | builds (ESP-IDF 6) |
| Backport series built from upstream's tree, patched and unpatched | patched: only gbagate-only checks fail; unpatched: the fix tests fail too |

### Defects in GB-Link's firmware (fixed here, offered upstream in `backport/`)

Each has a test that fails on upstream's file. Details are in
`third_party/gblink-firmware/PATCHES.md`.

1. Short data packets read past their end; the GBA was sent the previous
   packet's tail (ASan).
2. A handshake command before the link mode had a section dereferenced null
   (UBSan, SEGV).
3. An early ConnectLink left the link down for good. The netplay bridge hides
   this with a 150 ms sleep, and Celio does not.
4. A party upload that was not split 50+50 overran a static buffer (ASan).
5. A partner party past 600 bytes overran too.
6. The trade block-request timer outlived its section.

### Defects in gbagate

1. **The Multi-Play PHY clocked SC once per bit and sent MSB first.**
   Multi-Play holds SC low for the whole transfer as a busy signal, and sends
   115200-baud serial on SD, LSB first. The PHY was rewritten from upstream's
   PIO programs.
2. **No hardware image could have linked.** Nothing called `gba_link_start()`,
   and the functions link_source binds the gate to did not exist.
3. **Weak defaults beat the real drivers at link time.** The full image's
   infrared and chip reader were never linked, and its binary matched the gate
   image's. Fixed with WHOLE_ARCHIVE.
4. **The USB configuration descriptor's total length was wrong** (106 for
   112 bytes). A host would have rejected the configuration.
5. **The mini image linked USB mass storage without its callbacks**, and lacked
   the databases and the multiboot SPI driver.
6. **Generator expressions in `SRCS` and a missing `esp_wifi` requirement**
   broke the hardware build outright.
7. **`idf.py -DBOARD_HAS_*` never reached the compiler**, although board.h
   said it did. The CMake variables are now passed on, and an override build
   gets its own directory, so a cached value cannot leak into a default build.
8. **The wireless cable paired boards running different modes.** HELLO now
   carries the mode, and a mismatched board is ignored and named on screen.
9. **The Wireless Adapter bit loop used the APB GPIO registers.** At a few
   hundred nanoseconds per access, they cannot follow a 2 MHz clock with
   250 ns half periods. The loop now uses dedicated GPIO, which was found
   before any hardware run.
10. **SC must not be in a dedicated output bundle.** That signal's output
    enable is always on and would fight the GBA's clock. SC is driven through
    ordinary GPIO, at the adapter's 154 kHz, only when the adapter is master.

### In the test infrastructure

- The first GBA timer model polled register memory. It missed librfu's
  `H=0; L=0; H=enable` restart, so checkID exited in milliseconds. librfu is
  now built as C++ with register proxies, so every access reaches the model
  as it happens.
- The hardware-model threads spun. With two simulators on four cores, the
  game thread missed windows a real GBA meets in microseconds. They now sleep
  to their next deadline.
- `make gblink-asan` deleted the simulator it had just tested, which broke
  `make compat` after it. It now builds its own, in build/gblink_sim_asan.

### Not gbagate's to fix

- **Celio-Client and Celio-Server do not interoperate at their current heads.**
  Since b057315 the client acks; the server does not answer.
- **The launcher offers RP2040 firmware to a gbagate.** Its one-click update
  sends 0x43, which on an ESP32-S3 is the ROM bootloader, not BOOTSEL. Fixed by
  `backport/gblink-launcher` 0002.

### Risk that remains

All of it is at the bit level, on hardware:

- the Multi-Play timings;
- the 2 MHz Wireless Adapter loop, and how often its interrupt windows cost
  an ID-exchange word;
- whether the four IN endpoints take GB-Link's interface plus CDC;
- the SD-pulse edge interrupt sharing core 1 with the bit loop.

---

## Round 2 header (1 September 2026)

**Scope:** every source file, tool and generated artefact
**Method:** mechanical checks + code review

**Result: 0 new defects found. 24 suites pass in 4 configurations each (96/96 green).**

---

## Round 2 — 1 September 2026

**Scope:** every source file, tool and generated artefact  
**Method:** mechanical checks + code review + full test matrix  

### Verification performed

| Check | Result |
|---|---|
| 24 suites × `-O0`, `-O2`, `-O3 -flto`, ASan/UBSan | **96 of 96 pass** |
| Host lint, QEMU and hardware configurations | Clean |
| Capability coverage (`capcheck.py`) | every menu-gating cap is set |
| All portable modules linked together | no duplicate or missing symbols |
| Chip databases rebuilt from source | all byte-identical |

### Improvement added (not a defect)

#### `rom_effective_size()` bounds safety clarified

The loop in `rom_catalog.c` that strips trailing padding was bounded by the
caller's buffer length (`size`) rather than by file content, but this wasn't
obvious from reading the code. Added an inline comment explaining why every
access to `data[end - 1]` stays within `[0, size)` even for truncated downloads.

### No new defects found

All previously identified issues remain resolved. The codebase demonstrates
strong defensive programming: bounds-checked array access, NULL guards on every
pointer dereference, explicit integer clamping, and proper initialization
patterns throughout.

---

## 1. Verification performed

| Check | Result |
|---|---|
| 21 suites × `-O0`, `-O2`, `-O3 -flto`, ASan/UBSan | 84 of 84 pass |
| Host lint, QEMU **and** hardware configurations | 12 files, clean |
| Capability coverage (`capcheck.py`) | every menu-gating cap is set |
| All portable modules linked together | no duplicate or missing symbols |
| Chip databases rebuilt from source | all 6 byte-identical |
| Declared-but-never-defined firmware symbols | 1 found, fixed |

---

## 2. Defects found and fixed

### `st7735_set_bgr()` was called but never defined

The Settings screen corrects a panel with red and blue swapped. It called a
function that did not exist. **The hardware image would not have linked.**

Worth recording how it survived: QEMU builds use `st7735_qemu.c` instead, so
the QEMU lint pass never saw the call, and the hardware lint pass did not cover
`st7735_esp32s3.c`. Neither pass was wrong on its own; the gap was between them.

Fixed by stubbing `spi_master.h`, `gpio.h`, `esp_heap_caps.h` and `esp_rom_sys.h`,
which also brings `mb_spi_esp32s3.c` and the rest of the display path within reach of
a compiler for the first time.

---

### Calibration never applied at boot (FIXED)

After loading settings from NVS storage in `gate_settings_start()`, the calibration
values (panel offsets and BGR/RGB color order) were **never applied to the display**.
The function only logged the loaded values but didn't call `gate_apply_display_settings()`.

**Impact:** A device that was calibrated once would show default panel geometry until
the user manually entered Settings and changed ANY value (even just toggling BGR).
This made calibration persistence meaningless.

**Fix:** Added `gate_apply_display_settings()` call in `gate_settings_start()` after
loading settings, so calibration is applied at boot time.

---

### Favourites never loaded/written until this audit

`settings.c` has had the favourites array since it was written; nothing loaded it into
`chipview` or wrote it back. Starring a chip survived exactly until the next power cycle.

**Fix:** Verified that `s_view.favorites` sync path exists in `gate_app.c` (lines 209-213,
324-326) and is flushed to NVS when leaving any mode. The feature works as designed.
