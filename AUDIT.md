# Audit — Battle Chip Gate / GBA Link Adapter

Two rounds. **Round one** (20 August) was a mechanical sweep of the whole tree.
**Round two** (24–25 August) was driven by the emulation lab and is in section 7;
it found the more serious defect of the two rounds, because it could observe
behaviour rather than only inspect code.

| round | method | result |
|---|---|---|
| 1 — 20 Aug | mechanical inspection | 6 defects, all fixed, all in the untested glue |
| 2 — 24–25 Aug | observation under QEMU + mGBA | 3 defects, all fixed, one of them shipping behaviour |

---

## Round one

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

Round two revised one row of this table and added another. **Chip gate protocol**
stays High and is now stronger — the rotation was traced word for word on real
Xtensa against a real ROM. But **the wiring between the database and the gate**
was never in this table at all, and that is exactly where D7 lived: two modules
each correct, joined by the wrong number. Integration between High-confidence
modules is not itself High-confidence, and nothing here said so.

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

> **Superseded.** `PROJECT_STATUS.md` §8 is the current list. Item 8 above is
> still cosmetic as written — those IDs have nothing to map to — but the
> judgement that *wire IDs in general* were a cosmetic concern was wrong, and
> round two says why. Round two also added two items this list could not have
> had: whether `--aux-uart 0` draws, and whether the ~30% of records with no
> wire ID should be mappable at all.

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

> **Partly wrong, and worth keeping visible.** "Further host-side work has
> reached diminishing returns" held for *more modules*, which is what it meant.
> It did not hold for *better observation*: the emulation lab was host-side work
> that cost a few days and found D7, a live correctness bug in shipping
> behaviour that four days of green tests had not. The distinction is between
> adding surface and adding evidence. The bench recommendation itself stands —
> see `PROJECT_STATUS.md` §10.

---

## 7. Round two

**Date:** 24–25 August 2026
**Scope:** the gate under emulation — firmware in QEMU, `broker.py`, mGBA
running MMBN6 and a custom tester ROM
**Method:** observation, not inspection. Read the broker's frame log against
`chipgate.c` by hand, then wrote checks for what the existing harness was not
asserting.

**Result: 3 defects found, all fixed. 1 environment limitation, not fixable
here.**

The distinction from round one matters. Round one could only ask *is this code
self-consistent*. Round two could ask *is the number on the wire the right
number*, and that question found a bug four days of green tests had not.

---

### D7 — the gate sent the chip table index where the wire ID belongs *(shipping behaviour)*

`gate_app.c` called `gate_send_chip(rec.index)`. `chipgate_load()` documents its
argument as the wire ID — "the binary number formed by the chip's shorted edge
pads" — while `chip_record_t.index` is the chip table's own "ID Sort" key.
`chipdb` has maintained a **separate** wire-ID map all along, and the gate
ignored it.

The two numbers agree for many chips, which is precisely what made this
survivable: slot 5 of the rotation carried a plausible chip ID that named the
wrong chip. Measured across all six databases:

| database | records | mapped | **diverged** | no wire ID |
|---|---|---|---|---|
| bn4b / bn4r | 389 | 285 | **203** (71% of mapped) | 104 |
| bn5c / bn5p | 424 | 293 | 4 | 131 |
| bn6f / bn6g | 411 | 285 | 2 | 126 |

**418 chips.** In BN4, 71% of every chip that could be sent was misnamed.

Fixed with `chipdb_id_for_slot()` — the inverse of the existing map. Safe
because `build_chipdb.py` guards both sides of each assignment with a `used`
set, so the map is injective by construction and the inverse is unambiguous.
That property is now asserted rather than assumed: `test_chipdb.c` round-trips
every mapped ID and checks `round_tripped + no_id == count`.

Three things about how it hid are worth recording:

- **The evidence was already in the docs.** `README.md`'s wire-ID table has
  carried an `identity` column since 20 August: 82, 289 and 283 of mapped, for
  BN4, BN5 and BN6. Subtract those from `mapped` and you get 203, 4 and 2 —
  exactly the divergence counts measured here. The builder verified identity
  honestly and reported it; nobody connected that column to the consumer.
- **The one database the lab exercises is the one where it is nearly
  invisible.** `bn6g` diverges on 2 chips out of 285.
- **Nothing put a chip on the wire.** `broker.py`, `gate_sim.c` and the tester
  ROM all left slot 5 as `0000`, so the entire chip-injection path — the purpose
  of a Battle Chip Gate — had never been exercised in any harness. A test cannot
  catch a wrong value in a field no test ever fills.

An accepted behaviour change came with it: records with no wire ID (~30% of each
database) now show `NO WIRE ID` and send nothing, where before they sent a
wrong-but-plausible number. Refusing is correct; the ratio is worth reviewing
against `MATCH_THRESHOLD`, which is a database question, not a gate one.

### D8 — `lab.sh` leaked a QEMU whenever a terminal closed

The trap was `cleanup EXIT INT TERM`. No `HUP`. Ctrl-C was handled, but closing
a terminal window or dropping an ssh session sends SIGHUP, and **bash does not
run an EXIT trap when it dies on an untrapped fatal signal**. The result was an
orphaned `qemu-system-xtensa` holding `:5556` and `:5557`, which is how this was
found — a subsequent run failed with `port 5556 is already in use`.

The script's own header claimed "a failed run never leaves a QEMU or a bound
port behind". That claim had a hole in it.

Fixed by adding `HUP` to the trap. The failure message also now names the
culprit via a `port_holder()` helper, so the dead end becomes
`already in use by pid NNNN … Clear it with: kill NNNN`.

### D9 — `build.sh` silently dropped build-time options

`build.sh` forwarded only `-D VARIANT` and `-D IDF_TARGET` to `idf.py`. Any
other `-D` never reached CMake.

This was found while documenting the E1 remedy, and it would have wasted a bench
hour: the documented fix (`--aux-uart 0`) could not have taken effect no matter
what the README said, and the symptom — an unchanged blank screen — looks
exactly like the diagnosis being wrong.

Fixed by forwarding `QEMU_AUX_UART` explicitly, and **on every build, not only
when the flag is passed**. CMake caches `-D` values in the build directory, so
conditional forwarding would make the value sticky: one `--aux-uart 0` build
followed by a plain rebuild would silently keep `0`, producing an image whose
UART disagrees with QEMU's `-serial` layout. That mismatch reproduces the exact
blank screen the option exists to cure.

### E1 — this QEMU does not wire UART2 *(environment, not a defect here)*

Documented in `PROJECT_STATUS.md` §7 with the measurement. Summarised: the
framebuffer and the encoder both live on UART2, and this QEMU build's `esp32s3`
machine never attaches a third `-serial` chardev to a UART peripheral. The
firmware writes; QEMU discards.

The reason it presented as a mystery rather than an error is worth keeping: a
chardev QEMU creates but never attaches **still opens its listening socket and
still accepts a client**. Every observable downstream signal says "connected".
`qemu/uart2check.sh` exists to answer this in one step, using UART1 as a
built-in positive control in the same run.

---

## 8. What round two says about the method

Round one's headline number was *the untested quarter produced 100% of the
defects*. Round two adds a second, less comfortable one.

D7 was in `pio/src/gate_app.c` — glue, again, and still the riskiest code in the
tree. But it was not found by inspection, and round one had already inspected
that area. It was found by **reading a log of what actually went out on the
wire** and comparing it against the specification by hand. The value that was
wrong was structurally plausible, correctly typed, in range, and produced by a
function that returns exactly what its name promises.

Two practices follow, and both are now in place:

1. **Check what a passing test is not asserting.** `hs_passed()` verifies the
   link came up and that ≥24 frames flowed. It does not verify slot ordering or
   counter arithmetic, so it would pass a scrambled rotation. That gap is now
   covered by `test_chip_injection` in `qemu/test_rom_selftest.py`, which checks
   all 99 frames against an independently written model.
2. **A field no test ever fills is not covered, however green the suite.** Slot
   5 was `0000` everywhere. The fix was not more assertions about the frames
   being sent, but sending a frame that had never been sent before.
