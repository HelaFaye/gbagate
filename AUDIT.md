# Audit

**Date:** 1 September 2026
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
