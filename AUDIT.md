# Audit

**Date:** 28 August 2026
**Scope:** every source file, tool and generated artefact
**Method:** mechanical checks, not a read-through

**Result: 1 defect found and fixed. 21 suites pass in 4 configurations each.**

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

## 2. The defect

### `st7735_set_bgr()` was called and never defined

The Settings screen corrects a panel with red and blue swapped. It called a
function that did not exist. **The hardware image would not have linked.**

Worth recording how it survived: QEMU builds use `st7735_qemu.c` instead, so
the QEMU lint pass never saw the call, and the hardware lint pass did not cover
`st7735_esp32s3.c`. Neither pass was wrong on its own; the gap was between them.

Fixed, and the hardware lint now covers the display driver. That required
stubbing `spi_master.h`, `gpio.h`, `esp_heap_caps.h` and `esp_rom_sys.h`, which
also brings `mb_spi_esp32s3.c` and the rest of the display path within reach of
a compiler for the first time.

---

## 3. Features finished this pass

Three gaps where the machinery existed and nothing connected it. Each looked
like a working feature from the outside.

**The settings screen adjusted one value.** Row offset only; the other five were
displayed and inert. A screen that lists six settings and responds to one
teaches you not to trust it. Now a field table -- cursor, clamping and
formatting come for free, and adding a setting is a row.

**Stored calibration was never applied.** `st7735_set_offset()` existed and
nothing called it, so a device calibrated once came back at defaults and the
save looked broken rather than never loaded. Applied at boot and again the
moment a value changes, because an offset you cannot see change is one you
cannot calibrate by eye.

**Favourites were carried and never read.** `settings.c` has had the array since
it was written; nothing loaded it into `chipview` or wrote it back. Starring a
chip survived exactly until the next power cycle.

### One design note

Editing uses one encoder for two jobs -- move the cursor, or change a value --
with the button switching between them. The selected value is bracketed while
editing, because otherwise nothing on screen says which mode you are in and the
knob does something different depending on invisible state.

Acceleration is deliberately ignored while editing. The curve that crosses 400
chips in a few turns would move a 0..8 offset the whole range on one flick.

---

## 4. Where the risk sits

```
portable + headers   ~6400 lines   21 suites, 4 configs each
tests                ~5200 lines
esp-idf glue         ~2200 lines   UNTESTED, but now lint-covered
```

The untested fraction has produced every defect in every audit, including this
one. It is now compile-checked in both configurations, which catches the
structural class -- missing declarations, absent definitions, wrong argument
counts -- but nothing behavioural.

---

## 5. Open items

| # | Item | Closed by |
|---|---|---|
| 1 | PHY edge timing at 115200 | logic analyser |
| 2 | **Pad bit order** | one chip, a multimeter |
| 3 | `ack_word` vs gate ID | log non-`0x8FFF` words |
| 4 | Multiboot bit order and inter-word gap | scope |
| 5 | ST7735 offsets and BGR/RGB | **now adjustable on the device** |
| 6 | USB MSC enumeration and FAT handover | ESP-IDF and a USB host |
| 7 | RFU emulation, infrared | not implemented, deliberately |

Item 5 has moved: it no longer needs a recompile to correct, only the Settings
screen and an eye.

---

## 6. Recommendation

Unchanged, and now sharper. Every feature that can be built and verified without
hardware has been. The remaining items are not code problems: two need a scope,
one needs a multimeter and could be done today, one needs a USB host, and two
have nothing documented to implement against.

**The next useful work is a bench session.**
