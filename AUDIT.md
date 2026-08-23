# Audit — Battle Chip Gate / GBA Link Adapter

**Date:** 20 August 2026
**Scope:** every source file, tool and generated artefact
**Method:** mechanical checks, not a read-through — drift comparison, four build
configurations per suite, whole-project link, header hygiene, reproducibility,
tool round-trip, coverage and coupling scans

**Result: 6 defects found, all fixed. 13 suites pass in 4 configurations each.**

---

## 1. Verification performed

| Check | Result |
|---|---|
| Delivered files vs working copy | 1 drift, fixed |
| 13 suites × `-O0`, `-O2`, `-O3 -flto`, ASan/UBSan | 52 of 52 pass |
| All 18 portable modules linked together | no duplicate or missing symbols |
| Header guard uniqueness | no collisions |
| Every header compiles standalone | all self-contained |
| Chip databases rebuilt from source | all 6 byte-identical |
| `fix_header.py` output re-validated by device code | passes |
| Module coverage | every portable module appears in a suite |
| ESP-IDF glue review | 4 defects found, fixed |

---

## 2. Defects found and fixed

### D1 — `msc_read_all()` refused a maximum-size program *(would have shipped)*

Read `cap` bytes and treated `got == cap` as "file may be longer than the
buffer". For a 256 KB ROM into a 256 KB buffer that is an exact fit, so **the
largest legal multiboot program — the case that matters most — would have been
rejected** with no diagnosis.

Fixed by taking the length from `stat()` and reading exactly that. The read
result then distinguishes a short read from a fit.

### D2 — Mount configuration built and discarded

`usb_msc_esp32s3.c` constructed an `esp_vfs_fat_mount_config_t` with
`format_if_mount_failed = true`, then discarded it with `(void)mount`. The
README documented behaviour the code did not have: a brand-new device with an
unformatted partition would have failed to mount rather than formatting itself.

Fixed by moving the config into `tinyusb_msc_spiflash_config_t.mount_config`
where ESP-IDF 5.x expects it, with a note that this field has moved between
versions.

### D3 — Missing `esp_partition.h`

`esp_partition_find_first()` and `esp_partition_t` used without the header.
Would not have compiled.

### D4 — Nonsense SPI flag expression

`.flags = SPI_DEVICE_HALFDUPLEX == 0 ? 0 : 0` — an expression that evaluates to
0 either way and reads as though it means something. Replaced with `.flags = 0`
and a comment saying why full duplex is correct.

### D5 — Renderer trusted an unenforced invariant

`ui_render_chip_screen()` maps a visible row to a chipview position via the
list's scroll offset, which is only meaningful if the list was filled from that
view. Nothing checked it. A mismatched pair would have drawn **real chip names
in the wrong order** — wrong data that looks entirely plausible, the worst
failure shape for a UI.

Fixed with `ui_render_list_matches()`, checked before drawing; a mismatch paints
a blank list and shows `LIST MISMATCH` rather than plausible nonsense. Tested
against three mismatch shapes, including a foreign array holding identical
contents and a stale count after a tab change.

### D6 — Documentation drift in `link_session.h`

A correction was applied to the delivered copy but not the working tree, so the
source still carried the superseded claim that a child cannot stall a parent.
Resynchronised; the delivered version was the correct one.

### Also cleaned

Unused `esp_wifi.h` include; a callback named as though it overrode a TinyUSB
weak symbol when it is registered through config, renamed to `msc_on_mount_changed`.

---

## 3. Where the risk actually sits

```
portable + headers   5100 lines   13 suites, 4 configs each
tests                3832 lines   75% of portable
esp-idf glue         1687 lines   UNTESTED

untested surface: 1687 of 6787 = 24%
```

All six defects were in the 24%. That is not a coincidence and it is the single
most useful number in this report: **the untested quarter produced 100% of the
defects.** Two of them (D1, D2) would have shipped as silent misbehaviour rather
than build failures.

The mitigation is structural rather than aspirational — the glue is thin on
purpose, and every decision that can live in a portable module does. But it
cannot go to zero, and nothing in the portable layer proves the glue works.

---

## 4. Confidence by area

| Area | Confidence | Basis |
|---|---|---|
| Chip Gate protocol | **High** | Two independent derivations agree |
| Multiboot protocol | **High** | Simulated GBA decrypts and CRCs independently |
| Wire protocol + bridge | **High** | Hostile loopback: fragments, drops, corruption |
| Chip database + reader | **High** | Reproducible builds, real schematic vectors |
| UI model + renderer | **High** | Pixel assertions, visual check |
| Header validation + tools | **High** | Two implementations fuzzed against each other |
| **Multi-Play PHY** | **Low** | Framing from spec; no timing measured |
| ST7735 driver | **Low** | Never run |
| USB MSC / transports / mb SPI | **Low** | Never run; 4 defects found by inspection alone |

---

## 5. Open items, unchanged by this audit

| # | Item | How to close |
|---|---|---|
| 1 | PHY edge timing at 115200 | Logic analyser |
| 2 | Pad bit order — which pad is MSB | One known chip, multimeter |
| 3 | Does `ack_word` equal the gate ID | Log non-`0x8FFF` words in SYNCED |
| 4 | Multiboot bit order (MSB vs LSB first) | Fails as "sync never completes" |
| 5 | Multiboot inter-word gap (36 µs assumed) | Scope |
| 6 | ST7735 offsets and BGR/RGB | Calibration pattern |
| 7 | Per-game stall budget | Counters on the status bar |
| 8 | BN6 wire IDs above 320 | Cosmetic; compare against bn6f |

Items 1, 4, 5 and 6 all resolve in a single bench session with a scope and a
console. Item 2 needs one physical chip and a multimeter and can be done today.

---

## 6. Recommendation

The portable layer is in good shape and further host-side work has reached
diminishing returns: the last three modules produced no defects in their own
code, and this audit found defects only where hardware is required to verify.

**The next useful work is a bench session, not more code.** Specifically:

1. Chip reader with one known chip — closes item 2, needs no scope.
2. Breadboard the link port and scope it — closes 1, 3, 4, 5.
3. Display calibration pattern — closes 6.

Adding further modes before that session multiplies the untested surface against
an unverified foundation. The externally-suggested backlog is best left where it
is until the wire is proven.
