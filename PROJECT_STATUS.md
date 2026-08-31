# Project State — Battle Chip Gate / GBA Link Adapter

**Date:** 25 August 2026
**Target:** ESP32-S3 (S3FH4R2, 4 MB flash / 2 MB quad PSRAM) · ST7735 128×128 ·
rotary encoder with button · MCP23017 · chip detect switches · optional
nRF24L01+PA/LNA
**Build:** PlatformIO, `framework = espidf`, three images

```
portable + headers   5751 lines    20 modules
tests                4455 lines    15 suites
esp-idf glue         2170 lines     9 files   no hardware run yet
pio application       717 lines     5 files   partly exercised under QEMU
emulation lab        3680 lines    19 files   QEMU + mGBA + tester ROM
tools                1238 lines     6 tools
                    ------
total               18011 lines
```

**14 suites × 4 build configurations (`-O0`, `-O2`, `-O3 -flto`, ASan/UBSan) —
all passing** as of the 24 Aug run. Plus a QEMU/emulation tier described in
section 3.

⚠️ **The 15th suite (`modes`) has never been compiled or run.** It was written
25 Aug in an environment where the build required an approval that was not
given, so `make run-modes` has not executed once. Treat `src/app_modes.c` as
unverified code and the suite as an unverified claim until someone runs
`make run-modes` — see section 2.

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
| **Wire-ID mapping** | complete, and the map is now *used* | Injective by construction; `chipdb_id_for_slot()` inverts it and round-trips every mapped ID. See D7 — the gate previously ignored the map |
| **Chip injection over the link** | complete | A chip driven through all 99 frames of the rotation against the real `chipgate.c`; slot order, both counters, 9-frame hold, single-edge consume |
| **Chip screen** | parity with the homebrew | 6 tabs, 4 sorts, favourites, Slot-In/Move, uses column, scrollbar |
| **Renderer** | complete | Pixel-level assertions plus a visual check (`screens.png`) |
| **Mode shell** | *selection* complete; dispatch written but unrun | Modes gated by detected hardware; diagnostics always reachable. `src/app_modes.c` now joins `SHELL_SCREEN_RUNNING` to a handler table, but has never been compiled — see "Written but never built" |
| **Rotary encoder** | complete | Quadrature with structural debounce, acceleration crossing 411 chips in 25 detents |
| **Display geometry** | complete | Panel offsets, hardware vertical scroll: one row costs 3072 B against 24576 B |
| **Chip reader** | complete | 11 shared pad lines, one MCP23017, bus discipline enforced |
| **Multiboot** | complete | Validated against a simulated GBA that decrypts and CRCs independently |
| **ROM catalogue** | complete | Validation, padding strip, RAM staging, hostile filename cases |
| **Header repair** | complete | Two implementations fuzzed against each other |
| **Transport bridge** | complete | Hostile loopback: fragmentation, drops, corruption |
| **Pacing / lockstep** | complete | Stall budget, underrun accounting, 16-bit sequence wrap |

### Proven under emulation (Xtensa, not hardware)

| Feature | What the QEMU lab showed |
|---|---|
| **Boot, variant resolution, task layout** | `app_main` runs, tasks pin, NVS and FAT mount on emulated flash |
| **`link_bridge` over UART** | Links to `broker.py`, and through it to mGBA running the tester ROM |
| **Gate rotation on real Xtensa** | Tester ROM reaches `ROM-TEST: PASS (gate FF00, 24 linked frames)`; broker log matches the documented rotation word for word |
| **`transport_uart.c`** | Same bridge behind a UART; the protocol under test is the real one |

### Written, never run

| Feature | Risk |
|---|---|
| **Multi-Play PHY** | Framing from GBATEK; **no timing measured** |
| **ST7735 driver** (`st7735_esp32s3.c`) | SPI/DMA plumbing, DMA flush, scroll path. QEMU cannot exercise it — there is no SPI device on the other end |
| **USB MSC storage** | FAT ownership handover. USB-OTG is not emulated on the S3 |
| **USB CDC / ESP-NOW transports** | Thin, but unrun. Neither is emulated |
| **Multiboot SPI master** | Bit order and inter-word gap unconfirmed |
| **`--aux-uart 0/1` framebuffer layout** | Written this session in response to E1, never built or booted. See section 6 |

### Written but never built

**Mode dispatch — `src/app_modes.{h,c}`, `test/test_modes.c`, 658 lines,
zero compilations.**

This was the single largest gap. It is now written, and the writing is the
easy half: the environment it was written in required approval to invoke the
compiler, and that approval was not given, so *not one line has been through a
compiler*. It may not build. The suite may not pass. Nothing here should be
read as working code until `make run-modes` has run once.

Recording that plainly matters more than usual, because this same gap was
mis-scoped once already (25 Aug) by a session that assumed rather than checked.
That session began from the premise that four app modes — Wireless, Multiboot,
Settings, Chip Scan — were unimplemented. Checking the tree first would have
shown three of the four already exist:

| Assumed missing | Actually | Evidence |
|---|---|---|
| `mb_send()` for `.mb` | implemented | `src/gba_multiboot.c:61`, suite `test/test_multiboot.c` |
| ESP-NOW integration | implemented | `port/esp32s3/transport_esp32s3.c:139` `transport_espnow_start()` |
| nRF24 detection | implemented | `src/nrf24_probe.c`, 3-step presence test |
| Chip reader logic | implemented | `src/chip_reader.c`, `src/chip_slot.c` |

What was genuinely absent was the layer *between* them:

```
grep -rn "shell_init" → src/ui_shell.{c,h}, test/test_chipview.c   (and nothing else)
grep -n  "APP_MODE|shell_" pio/src/gate_app.c → (no output)
```

`pio/src/gate_app.c` includes `ui_shell.h` and uses nothing from it. The
subsystems exist and are tested; the wiring from a selected `app_mode_t` to the
subsystem that serves it did not. The mis-scope is instructive: the modules are
individually well covered, which makes absence at the seam between them easy to
mistake for absence of the modules themselves.

**What `app_modes.c` does.** A handler table — `{mode, enter, exit, tick}` —
injected the same way `rom_io_t` and `mb_xfer_fn` are, so the decisions are
host-testable. `app_dispatch_from_shell()` is the missing seam: it reads the
shell every UI iteration and makes dispatch follow it, entering on arrival at
`SHELL_SCREEN_RUNNING` and exiting on departure. Declarative rather than
paired enter/exit calls, because an obligation to pair them by hand is exactly
what gets missed on one of several exit paths.

Four guards, of which one is a real latent bug rather than defence in depth:

| Guard | Why |
|---|---|
| `NO_HANDLER` | during bring-up most modes are unbound; should read as "not wired yet", not hang |
| `MISSING_CAPS` | the shell's filter runs once at `shell_init()`; anything reaching dispatch by another route — saved last-mode, diagnostics jump, resume-after-reset — bypasses it |
| `NEEDS_GAME` | **the real one.** `ui_shell_t` keeps `chosen_game` and `has_game` as separate fields, so a per-game mode can be reached holding a stale `cg_game_t`. That starts a gate announcing the wrong identity, and the console's only symptom is silence |
| `BUSY` | refuses rather than silently swapping, so a leaked mode is loud |

**Still not built:** the handlers themselves. `app_modes.c` is the table and the
rules; every `enter`/`exit`/`tick` for the real modes remains to be written, and
`gate_app.c` is not yet wired to it — it still enters the chip screen directly.
That wiring is deliberately *not* done here: doing it blind, in a session that
cannot compile, would put unbuilt code on the only path the firmware boots
through.

One dependency worth noting before building the Settings handler: its first
calibration target is `msb_is_lowest_pad` (`src/chip_reader.h:66`), currently
hardcoded `true` and commented *"provisional until calibrated"*. That is Open
Question 2, which needs a bench. Building the UI first means shipping a control
whose correct default is unknown.

**Multiboot link target — `rom/multiboot.ld`, `rom/crt0_mb.s`,
`tools/check_multiboot.py`, plus edits to `rom/tester.c`, `rom/Makefile`,
`Makefile` and `.gitignore`. Also zero compilations.**

Written 25 August, in the same session and under the same constraint: `cc`,
`make` and even `python3` all required an approval that was not granted, so
none of it has been assembled, linked or run. `arm-none-eabi-gcc` could not
even be probed for. Read this section as a design that has not been executed.

The gap it addresses is the one the ROM audit found: `rom/tester.gba` links
`.text` at `0x08000000` (`rom/tester.ld:17`), so it is a cartridge image, and
a cartridge image cannot be the payload of a multiboot transfer no matter what
it is renamed to. `mb_send()` is well covered against the simulated GBA in
`test/test_multiboot.c`, but nothing had ever handed it a real, linked,
bootable payload. `make mb` is meant to produce one.

| File | What it does |
|---|---|
| `rom/multiboot.ld` | links every section into EWRAM at `0x02000000`; three `ASSERT`s (below) |
| `rom/crt0_mb.s` | multiboot header entries at `0xC0`/`0xC4`/`0xC5`/`0xE0`, no `.data` copy, 4-byte footer |
| `rom/Makefile` | `make mb`; size bounds checked against `MB_MIN_SIZE`/`MB_MAX_SIZE`; `fixheader-mb` |
| `tools/check_multiboot.py` | structural check on the built image; needs only `python3` |

Three decisions worth stating, because each encodes a failure that is silent
at the far end:

1. **`tester.o` is shared between the two links, not recompiled.** Only the
   startup file and the linker script differ. That makes it structurally
   impossible for the cartridge and multiboot images to drift into running
   different handshakes, so any discrepancy between them is a delivery problem
   and never a logic one.

2. **The image ends in four non-zero bytes (`GBMB`).**
   `rom_effective_size()` (`src/rom_catalog.c:114`) strips trailing `0x00`/`0xFF`
   before transfer, then rounds the payload back up to 16. An image that
   legitimately ended in alignment padding would therefore be sent *short*, and
   the symptom would be a program that boots and misbehaves rather than a
   transfer error. The footer makes that unrepresentable;
   `check_multiboot.py` reimplements `rom_effective_size()` and asserts it does
   not shorten the file.

3. **`make mb` is not part of `make`.** The cartridge image is what the QEMU
   harness uses and it is known to build. Keeping an unbuilt link off the
   default target means a mistake in it cannot break the working path — which
   matters more than usual when the author could not run either one.

The linker script asserts what `crt0_mb.s` assumes: `.data` LMA == VMA (there
is no copy loop), and neither the image nor `.bss` may reach `0x0203FF00`,
where `tester.c` publishes its verdict. An image that overran it would
overwrite its own result and report nonsense.

`tester.c` gained three appended fields — `run_base`, `boot_mode`, `slave_id`
— placed strictly after the mailbox, because `qemu/mgba_tester.lua` reads the
block at hard-coded offsets (0, 4, 8, 12, 16, and 44..50 at
`qemu/mgba_tester.lua:52-55`). `boot_mode`/`slave_id` are the header bytes the
BIOS overwrites during a download, so a non-zero pair is the one thing this
image can prove that `tester.gba` cannot. The evidence is one-directional and
the code says so: zero does *not* prove failure, since a high-level-BIOS
emulator may load into EWRAM and jump without emulating the transfer.

**Not done, deliberately:** no handler wires `APP_MODE_MULTIBOOT` to this, and
no host suite consumes `tester.mb` yet. The obvious next step — feeding the
built image through `mb_send()` against the `sim_gba_t` already in
`test/test_multiboot.c:57` — is a genuine end-to-end proof and should skip
cleanly when the ARM toolchain is absent. It was not written here because
adding a third unbuildable artifact to two existing ones is not progress.

Also designed but not built: Wireless Adapter emulation toward the GBA,
infrared accessory protocol, mGBA SIO driver upstream, font patch flow.

### Closed, deliberately

- **OEM Wireless Adapter interop.** The RFU radio is a Freescale MC13190 doing
  ~4 Mbps ASK at 2432 MHz; the nRF24 is GFSK-only, caps at 2 Mbps, and its
  packet engine cannot be bypassed. Three independent blockers. The nRF24 stays
  as a *second radio*, which is a different and still-valid argument.
- **Virtual ROM streaming of retail games.** Requires trapping cartridge
  fetches; the ARM7TDMI has no MMU or fault mechanism.

---

## 3. The emulation lab

Added since the 20 August status. `qemu/README.md` is the working document; this
is what it changes about *confidence*.

```
[ firmware in QEMU ] --UART1--> :5556 <--> broker.py <--> :7000 [ mGBA + tester ROM ]
                     --UART2--> :5557 <--> viewer.py        (see E1)
```

**What it added.** The firmware now boots and links on real Xtensa against a
real MMBN6 ROM, and a custom tester ROM turns "does the gate work" into a
machine-checkable verdict written to `build/lab/rom_test.result`. That is a tier
that did not exist before: between host unit tests and a bench session.

**What it does not do.** It removes **nothing** from the bench list in section 6.
No peripheral is wired to anything: the GBA link PHY, the ST7735 SPI bus,
USB-OTG, WiFi and ESP-NOW are all outside what QEMU models. The lab exercises
*logic in place*, not electrical behaviour.

**A caveat worth keeping in view.** `hs_passed()` in the tester ROM checks that
the link came up, that ≥24 frames flowed, and that it saw an identity, a
continue and one varied slot. It does **not** check slot ordering or counter
arithmetic — it would pass a gate whose rotation came round scrambled. The
`test_chip_injection` check in `qemu/test_rom_selftest.py` is what actually pins
the rotation down, and it runs on `cc` + `python3` alone.

| harness | needs | covers |
|---|---|---|
| `make` / `make strict` | `cc` | the 14 portable suites |
| `python3 qemu/test_rom_selftest.py` | `cc`, `python3` | ROM handshake against the real `chipgate.c`, chip injection, teardown/relink, framebuffer conversion |
| `python3 tools/check_wire_ids.py --list` | `python3` | every database's wire-ID map: injectivity, and the divergence D7 fixed |
| `bash qemu/uart2check.sh` | QEMU, a built image | which UARTs this QEMU actually wires |
| `./qemu/lab.sh` | QEMU, mGBA, a ROM | the whole stack end to end |

---

## 4. What was established, and from where

| Finding | Source |
|---|---|
| Gate IDs `0xFFC6/C7/C4/FF00`, 9-slot rotation | Homebrew disassembly + mGBA `battlechip.c` |
| Wire ID = 1-based line number in `chip-names.txt` | mGBA `BattleChipModel.cpp` |
| Wire ID ≠ chip table index for 418 chips | Measured across all six databases; see D7 |
| The gate rotation is correct on real Xtensa | Broker log traced word by word against `chipgate.c` |
| 18-bit Multi-Play framing; parent inserts delays | GBATEK |
| Multiboot constants `0x6F646573`, `0xC37B`, `0x43202F2F` | `stlink_gba_multiboot`, cross-read against GBATEK |
| Chips are passive shorts; **11 shared pad lines** | All 58 EXE project schematics |
| Chip Gate Use Limit is a real table field | mmbn-chip-tables |
| RFU is MC13190, `SIOCNT 0x5003`, 2432 MHz | GBATEK + MC13190 datasheet |
| GBA verifies the logo during multiboot | Confirmed by you |

---

## 5. Corrections made along the way

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
6. **"Wire-ID identity is verified rather than assumed" was true of the builder
   and false of the gate.** `build_chipdb.py` did verify it and recorded the
   divergence honestly — the identity column in `README.md` has said
   82/289/283-of-mapped since 20 August. `gate_app.c` then sent `rec.index`
   anyway. Verifying a fact in one place and ignoring it in another reads as
   proven when it is not. See D7.
7. **"Verified in `--sim` mode" did not mean what it looked like.** `--sim`
   skips the viewer entirely, so the claim in `qemu/README.md` covered a path
   that excluded the thing later found broken (E1). Corrected in place.
8. **"Four modes remain to be implemented" was wrong on three of four, and the
   work reported against it never happened.** Multiboot, ESP-NOW and the nRF24
   probe were already written and tested; the real gap was dispatch. Worse, the
   session then reported creating `wireless_mode.{h,c}` and others that were
   never written — the heredocs failed, first blocked by plan mode and then on
   a shell quoting error, and the failures were not read before success was
   claimed. Two lessons, and the second is the load-bearing one: check the tree
   before accepting a task premise, and check that a write *succeeded* before
   reporting it. A tool error that goes unread is indistinguishable from
   fabrication in the transcript.
9. **`src/app_modes.c` and `test/test_modes.c` were written without a compiler.**
   The same session that recorded correction 8 then wrote 658 lines it could not
   build, because `cc` and `make` both required an approval that was not granted.
   The code is committed and the docs describe it, but *no claim is made that it
   compiles*. This is recorded here rather than left implicit precisely because
   correction 8 is one item above it: the failure mode being guarded against is
   writing plausible code and letting the surrounding prose imply it works. See
   Open Question 13 — running `make run-modes` is the first task next session.
10. **"Is the test ROM ready to test all features of the firmware?" — no, and
    the multiboot link written in answer is also unbuilt.** The ROM covers one
    of seven app modes, and covers it as protocol logic rather than at the wire:
    `rom/tester.c` drives an EWRAM mailbox because mGBA refuses a script's
    writes to `SIOMULTI0`. `rom/multiboot.ld` and `rom/crt0_mb.s` were then
    written to close the most structural of those gaps — a cartridge-linked
    image can never be a multiboot payload — and they inherit correction 9's
    limitation exactly: no compiler was available, so `make rom-mb` has never
    run. The pattern is now three sessions old and worth naming: *this
    environment can write firmware but cannot execute anything, so every
    artifact leaves it unverified by construction.* The mitigation is not more
    care while writing; it is keeping unbuilt work off default build targets
    (`make mb` is not in `make`, `app_modes` is not wired into `gate_app.c`) so
    that a mistake in it cannot damage a path that does work. See Open
    Questions 15 and 16.
11. **The multiboot sender had a real protocol bug, and the tests were
    structurally incapable of finding it.** Checking `gba_multiboot.c` against
    GBATEK rather than against the test suite turned up D10 and D11. The lesson
    is not "write more tests" — there were fourteen suites and they were green.
    It is that `test_multiboot.c`'s simulated GBA was built from the
    implementation instead of from the specification, so it could only ever
    prove the two halves agreed with each other. **A test oracle derived from
    the code under test cannot falsify that code.** That is now fixed for
    multiboot; it is worth auditing whether `link_session`, `chipgate` and the
    QEMU broker share the shape.

    **Update (26 Aug): confirmed by falsification, not just by a green run.**
    `make run-multiboot` passes with the fix. More importantly, reverting
    `src/gba_multiboot.c:142` to the unmasked `client + 0x0Fu` and re-running
    produced `multiboot FAILURES`, and restoring the mask returned it to
    `all checks passed`. Red on the bug, green on the fix, same binary and same
    command — so D10 was a real defect and `test_client_byte_range()` actually
    constrains it. This is the first test in this project demonstrated to fail
    on the defect it was written for; every other suite is green but unfalsified,
    and green-but-unfalsified is exactly what the old round-trip assertion was.

    One honest limit on the observation: `Makefile:46` pipes each suite through
    `tail -1`, so only the verdict line was visible. The predicted diagnostic
    (`first failing client byte: 0xF1`, three failures) was **not** seen — what
    was confirmed is the *direction* of the prediction, not the specific byte.
    Confirming the byte needs `./build/test_multiboot data/chipdb/bn6g.chdb`
    run directly, without the `tail`. This is a minor point for D10, whose
    arithmetic is settled, but the `tail -1` habit means **no suite in this repo
    has ever shown its own diagnostics** — worth knowing before relying on one.

---

## 6. Defects found and fixed

The first round (20 August, six defects) is in `AUDIT.md` §2. Second round,
24–25 August, driven by the emulation lab:

| # | Defect | Would have |
|---|---|---|
| D7 | `gate_app.c` sent the chip **table index** where the **wire ID** belongs | told the game a different chip than the one on screen, silently, for 418 chips |
| D8 | `lab.sh` trapped `EXIT INT TERM` but not `HUP` | stranded a QEMU holding `:5556`/`:5557` whenever a terminal closed — observed |
| D9 | `build.sh` forwarded only `-D VARIANT` and `-D IDF_TARGET` to `idf.py` | made the E1 remedy unbuildable; a documented fix that silently could not take effect |
| D10 | `mb_send()` added the handshake byte into the checksum word `f` instead of packing it, without masking to 8 bits | failed the final CRC on ~5% of real transfers, at random, with nothing to point at |
| D11 | `MB_MIN_SIZE` was `0x1A0`, below the BIOS's documented floor | accepted images the download routine is not specified for |
| D12 | `Makefile:46` ran each suite as `./$< $(DB) \| tail -1`, so the pipeline's status was `tail`'s and every suite's exit code was discarded | made `make` exit 0 on failing tests — any CI or script gating on it would report green on red |
| D13 | `Makefile`'s `strict` rule printed one `FAIL` column for both "did not compile" and "compiled and failed", with compiler errors sent to `/dev/null` | a suite that stopped compiling under `-O3 -flto` would look like an ordinary assertion failure, with the diagnostic destroyed |

**D7 in detail.** `chipdb.h` maintains a wire-ID map because the reader needs
it; `chip_record_t.index` is the chip table's own "ID Sort" key. They agree for
most chips and not all, so sending the index looked plausible while naming the
wrong chip. Measured:

| database | records | mapped | **diverged** | no wire ID |
|---|---|---|---|---|
| bn4b / bn4r | 389 | 285 | **203** (71% of mapped) | 104 |
| bn5c / bn5p | 424 | 293 | 4 | 131 |
| bn6f / bn6g | 411 | 285 | 2 | 126 |

**418 chips across the six databases.** Fixed with `chipdb_id_for_slot()`, the
inverse accessor — safe because `build_chipdb.py` guards both sides of the
assignment with a `used` set, so the map is injective by construction.
`test_chipdb.c` round-trips every mapped ID and asserts
`round_tripped + no_id == count`, which fails loudly if injectivity is ever
lost. Confirmed by two independently written implementations agreeing exactly
on bn6g (C walking the live blob, Python parsing the file format).

**Why it survived four days of green tests.** The lab harness runs `bn6g`,
where only 2 chips diverge — and neither is one a test would happen to slot. It
is the single luckiest configuration for hiding this.

**An accepted behaviour change.** ~104 records in BN4 and 126–131 in BN5/BN6
have no wire ID at all. Selecting one now shows `NO WIRE ID` and sends nothing,
where before it sent a wrong-but-plausible number. Refusing is correct, but it
is visible: roughly 30% of the list becomes unsendable. If that ratio is wrong,
`MATCH_THRESHOLD` in `build_chipdb.py`'s `build_id_map` is the knob — not the
gate.

**D10 in detail — and why fourteen suites of green tests could not see it.**
GBATEK (`.reference/gbatek.htm:95956`) builds the two multiboot magic words as
four packed bytes, LSB first: `m=dword(pp,cc,cc,cc)` and `f=dword(hh,rr,rr,rr)`.
`hh` is `11h + client_data[1..3]`; with one slave on a Normal-mode link the two
absent slaves read back `FFh` each (`:95940`, `:95947`), so `hh = cc + 0x20F` —
a value that does not fit the byte-wide field it occupies. `mb_send()` assembled
`f` with `+` and never masked, so for any client byte above `F0h` the carry out
of byte 0 landed in the `rr` byte:

| client byte | `f` computed | `f` per GBATEK |
|---|---|---|
| `0x42` | `0xFFFF4251` | `0xFFFF4251` ✓ |
| `0xF5` | `0xFFFFF604` | `0xFFFFF504` ✗ |

`f` is folded into the final checksum, so the symptom is a transfer that runs to
completion and then fails CRC — precisely the failure `gba_multiboot.h`'s header
comment says the quoted constants exist to prevent. **The client byte is chosen
at random by the GBA**, and 14 of the 254 legal values trigger it, so on hardware
this is an intermittent ~5% failure with no diagnosis.

The reason no test caught it is structural and worth keeping: `sim_gba_t` in
`test_multiboot.c` was not an independent implementation of the spec. It called
the master's own `mb_key_next`, `mb_word_mask` and `mb_crc_step`, and re-derived
`f` by copying the master's expression — carry bug included. **A slave that
mirrors the master agrees with it even when the master is wrong**, which made the
whole round-trip assertion vacuous for exactly the class of bug it advertised
catching. The test's comment claiming "a wrong multiplier, xor or address term
all show up right here" was false; only the literal pins in `test_primitives()`
constrained those.

Fixed on both sides: the master packs with `|` after masking, and the simulated
slave now derives `m`, `hh` and `f` from GBATEK's definitions rather than from
the master's code. `test_client_byte_range()` sweeps all 254 legal client bytes,
which turns this from a bug caught by luck into one that cannot regress silently.

---

## 7. Environment findings

Not defects in this project, but they bound what the lab can prove.

**E1 — this QEMU build does not wire UART2.** Measured by `qemu/uart2check.sh`,
which boots the image for 6 s with every UART going to a file, no sockets and no
clients:

```
UART0 console         2153 bytes
UART1 link_proto       220 bytes   (the control: known to work)
UART2 framebuffer        0 bytes
```

UART1 filled in the same run, so serial chardevs work and the ordering is right;
the third simply is not attached to a UART peripheral in this build's `esp32s3`
machine. The console confirms the firmware *did* bring the tap up
(`st7735_qemu: framebuffer tap on UART2`), so the bytes were written and
discarded.

This is why the ST7735 panel stayed blank under emulation, and it is **not
fixable in the firmware or in `viewer.py`**. It is also not only about the
screen: `input_qemu.c` reads the same UART, so the encoder is dead under
emulation too, and no chip can be slotted by hand there.

Two routes out, neither yet confirmed:

1. A QEMU that models three UARTs (`idf_tools.py install qemu-xtensa`).
2. `--aux-uart 0`, which moves the framebuffer onto UART0 alongside the console.
   Written, **not yet built or booted.** Sharing is safe in principle because
   the stream is self-delimiting and each frame goes out in one
   `uart_write_bytes()`, but that reasoning is untested.

The silent-failure shape is worth remembering: a chardev QEMU creates but never
attaches still opens its listening socket and still accepts a client. The
connect succeeds and nothing ever arrives, so the symptom is a blank window with
no error anywhere.

---

## 8. Open questions

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
| 10 | Does `--aux-uart 0` actually draw | `./qemu/build.sh gate --aux-uart 0` then `./qemu/lab.sh --aux-uart 0` |
| 11 | Should the ~30% of records with no wire ID be mappable | Review `MATCH_THRESHOLD`; likely most are chips no physical Battle Chip ever encoded |
| ~~12~~ | ~~Where does mode dispatch belong — extend `gate_app.c`, or a new `app_modes.c`~~ | **Closed 25 Aug: `src/app_modes.{h,c}`.** `gate_app.c` is ESP-IDF-only and cannot be built by the host suites, so dispatch decisions placed there would be untestable — the exact property that let this gap survive. Portable module, injected handlers, ESP-IDF glue stays in `gate_app.c` |
| 13 | **Does `make run-modes` actually build and pass** | Run it. The suite was written without a compiler available; this is the first thing to check next session, before anything is built on top of it |
| 14 | Should `NO_HANDLER` outrank `MISSING_CAPS` in `app_dispatch_enter()` | Judgment call, currently NO_HANDLER first. During bring-up "not wired yet" is the common truth; once every mode is bound, a caps failure is the more serious signal and arguably should win. Revisit when the handlers exist |
| 15 | **Does `make rom-mb` build, and does the image pass `make rom-mb-check`** | Run both. Same situation as 13: written with no compiler and no way to probe for `arm-none-eabi-gcc`. The check needs only `python3`, so it is the cheaper half and can run the moment an image exists |
| 16 | Should a host suite consume `rom/tester.mb` | The pieces are already there: `sim_gba_t` at `test/test_multiboot.c:57` is a working simulated GBA, and `mb_send()` would take the real image unmodified. That turns "the linker produced a file" into "the protocol accepts this payload". Must skip cleanly when the ARM toolchain is absent, the way `qemu/test_rom.sh:77-82` already does |
| ~~17~~ | ~~Has `test_client_byte_range` been seen to FAIL~~ | **Closed 26 Aug.** Reverting `src/gba_multiboot.c:142` to unmasked `client + 0x0Fu` gave `multiboot FAILURES`; restoring the mask gave `all checks passed`. D10 confirmed real, sweep confirmed to have teeth. Caveat: `Makefile:46` pipes through `tail -1`, so the predicted `first failing client byte: 0xF1` line was never displayed — the direction was verified, the specific byte was not. See Open Question 19 |
| 19 | **Does the repaired `Makefile` reporting actually work** | Raised and fixed 26 Aug as D12/D13 — but, like everything else written here, **not compiled or run.** Verify with: `make run-multiboot` (expect one green line, unchanged); then break `src/gba_multiboot.c:142` again and re-run (expect `FAILURES -- full output follows`, the full suite output including `first failing client byte: 0xF1`, and a `make: *** [Makefile] Error 1` line that was **absent** before the fix). The `Error 1` line is the actual thing being tested; the rest is presentation |
| 18 | Do the other protocol suites share `test_multiboot.c`'s oracle problem | **Partly answered 26 Aug.** `test_link.c` round-trips `lp_encode`→`lp_decode` and pins no literal wire bytes — same flaw. Mitigated for the *encode* path only: `qemu/test_lp.py` diffs compiled C against `qemu/lp.py`, a separate implementation, byte for byte. So encode has a real independent oracle; **decode is still validated only against C's own encode.** `chipgate` and `link_session` not yet checked. Also worth confirming `lp.py` was written from the spec and not transcribed from `link_proto.c` — if transcribed, even the encode oracle is circular |

Question 2 needs one physical chip and a multimeter and could be done today.
1, 3, 4, 5 and 6 all resolve in a single bench session. Question 7 needs only a
verbose build. 10 needs only a rebuild and costs minutes.

**17 is closed, and it is the one worth learning from.** It was the only open
question guarding a change to code that already worked, and it was closed not by
a passing run but by making the test fail on demand. That distinction is the
whole point: 13 and 15 are still open, and running them will produce green or
red, but *green from them will mean less than green from multiboot does* — the
multiboot suite has been shown to distinguish a correct implementation from a
broken one, and `test_modes` and `rom-mb` have not. 13 and 15 need nothing but
permission to run a compiler.

19 is new and follows directly from closing 17: the harness discards suite
output, so a red result cannot say *what* was wrong. `Makefile:56-61` is worse
than line 46 — `strict` sends compiler errors to `/dev/null` as well, so a suite
that fails to *compile* under `-O3 -flto` is reported identically to one that
compiles and fails. That is a plausible way for a real defect to be seen and
dismissed.

Note that 4 — multiboot bit order — becomes cheap to answer once 15 is closed:
a real payload plus the simulated GBA in `test/test_multiboot.c` is most of a
test rig, and the failure signature is already known ("sync never completes").

---

## 9. Risks

**High — the PHY has never been measured.** Everything on hardware depends on
it. Mitigated structurally: the layers above are transport-agnostic, so a PHY
rewrite costs nothing above it. The QEMU lab does *not* touch this.

**High — 2 MHz for adapter emulation.** `SIOCNT 0x5003` is 0.5 µs/bit, ~120 CPU
cycles at 240 MHz, with clock direction switching mid-session. Seventeen times
tighter than Multi-Play. It is in all three images; if it proves unworkable it
comes out of the table, not out of every source file.

**Medium — a passing harness can be a loose harness.** D7 sat behind green tests
for four days because the one database the lab exercises is the one where the
bug is nearly invisible, and because `hs_passed()` accepts a rotation it never
actually checks. The lesson is not "add more tests" but "check what the passing
test is *not* asserting". Both gaps are now closed by
`qemu/test_rom_selftest.py`.

**Medium — IRAM discipline.** Nothing enforces it at compile time.
`CONFIG_SPI_FLASH_AUTO_SUSPEND=y` covers the cache side; the code side is
convention.

**Low — power.** Link port pin 1 gives 3.3 V at milliamps. Separate supply,
common ground, pin 1 disconnected in accessory mode.

---

## 10. Recommendation

Unchanged in direction, and now better supported: **the next useful work is a
bench session, not more code.** The emulation lab was worth building — it found
D7, which is a real correctness bug in shipping behaviour rather than a
build-time slip — but it has now given up most of what it can. Everything left
on the list needs a scope, a chip or a panel.

Before the bench:

1. **Rebuild with `--aux-uart 0` and confirm the screen draws** — closes 10,
   costs minutes, and restores the encoder under emulation, which is the only
   way to slot a chip by hand without hardware.
2. **`pio run -e gate -v`** — closes 7, costs a minute.

At the bench:

3. **Chip reader with one known chip** — closes 2, needs no scope.
4. **Breadboard the link port and scope it** — closes 1, 3, 4, 5.
5. **Display calibration pattern** — closes 6.

Adding further modes before that session multiplies untested surface against an
unverified foundation.

---

## Remaining features: what was built, and what was deliberately not

**Built and host-tested (this pass):**

- **MCP23017 driver** (`src/mcp23017.{h,c}`) -- the last piece between a fitted
  reader board and a working Read Chip mode. I2C injected, so register
  sequencing, the presence probe, BANK=1 recovery and bus discipline are all
  tested against a simulated part that models the pad-bus physics.
- **`port/esp32s3/i2c_esp32s3.c`** -- the real transfer behind it, and a
  `chip_reader_probe()` that reports honestly when no board is fitted.
- Wired into the full image only; the gate and link images keep the weak
  default and hide Read Chip.

The expander test found a bug immediately: the detect switches were on bits 8
and 9, which are **pads 9 and 10**, not spare pins. A direction change for a
pad silently became a change for a switch, the mask came out all-ones, the
write was skipped as redundant, and every chip decoded as empty. Moved to bits
11 and 12.

**Deliberately not implemented:**

- **RFU (Wireless Adapter) emulation.** The link-cable side is documented, but
  it runs at `SIOCNT 0x5003` -- 0.5 us per bit, with the clock direction
  switching mid-session. Nothing in this project can verify that timing: QEMU
  models no pins, and the host suites model no clock. Writing it now would
  produce a few hundred lines of plausible code whose only test would be a
  bench session that has not happened. That is exactly the pattern that
  produced every defect the audit found.
- **Infrared.** The GBA side is GPIO bit-banging, which is the easy part; the
  accessory protocol is undocumented beyond a single third-party device. There
  is nothing to implement *against*.
- **USB MSC.** The glue exists and is unrun. Finishing it means testing it, and
  testing it needs ESP-IDF and a USB host.

The honest position is unchanged from the audit: what remains needs hardware or
a toolchain, not more code. Adding unverifiable modes multiplies the untested
surface against a foundation that still has not met a logic analyser.

---

## Modes and persistence (this pass)

**All nine modes now have a screen.** Four had been falling through to a
placeholder: `Link: USB`, `Link: Wireless`, `Read Chip` and `Settings`. A fifth,
`USB Storage`, is new.

| mode | shows |
|---|---|
| Link: USB / Wireless | bridge state, words sent and received, round-trip time |
| Read Chip | slot occupancy, and the decoded wire ID |
| USB Storage | who owns the volume, and a button to hand it over |
| Settings | panel offsets, colour order, saved/unsaved |

The link screens report **round-trip time** because that is the number that says
whether a wireless link is usable: under a millisecond is fine, tens are not.

`USB Storage` exists because handing the volume to a host has to be a deliberate
action. Inferring it from what the host appears to be doing is what corrupts a
FAT volume, and a device that cannot be told to release its storage is one you
end up power-cycling. Leaving the mode reclaims the volume automatically, so the
catalogue never reads a drive the device does not own.

### Persistence

`src/settings.{h,c}` holds two kinds of state, and they are worth telling apart:

- **calibration** -- panel offsets and BGR/RGB. Found once with a test pattern.
  Losing these means recalibrating by eye.
- **preference** -- default game, encoder acceleration, favourites.

The blob is `magic | version | length | payload | CRC`, with storage injected,
so a corrupt or foreign save is handled by tested code rather than by whatever
NVS returns. A bad blob falls back to defaults: **a device that boots to
defaults is recoverable, one that boots to garbage calibration is a puzzle.**

An intact blob holding an impossible value -- a game index from a build with
more games, an offset past the panel -- is **clamped, not rejected**, because
rejecting would throw the calibration away along with it.

**Saves are deferred.** Every favourite toggle would otherwise be a flash write,
and flash writes suspend the instruction cache -- the one thing the link task
cannot tolerate. State is marked dirty and flushed when a mode is left, keeping
writes to moments when nothing is being clocked.

### Still not done

Unchanged: RFU emulation and infrared, for the reasons above; and the bench
list, which no further code touches.

---

## Infrared: no longer undocumented

I twice declined to implement IR on the grounds that there was nothing to
implement against. That was true until you supplied Shonumi's article, which is
a complete description of the AGB-006 and the Cyber Drive Zoids protocol. The
objection is answered and the work is done.

`src/ir_cdz.{h,c}` -- classification, decoding, encoding, 22nd test suite.

### What the article establishes

The adapter runs in **General Purpose mode** via `RCNT`, not SIO. The game
writes `0x80BE` and `0x80B6`; decoding those against the GPIO bit layout shows
they differ in exactly one bit, bit 3 = **SO**, with a direction nibble of `B`
making SO an output and SI an input. That is internally consistent with the
article's claim that SO high turns the LED on, and it is the one part of the
spec I could check independently.

Presence is detected by a **serial interrupt**, not by pin values: the game
sends a pulse and expects an IRQ. The adapter interrupts on both sending and
receiving, and on receive only for no-light to light -- never the reverse.

### The two mistakes this code exists to avoid

**Pulses are classified by the gap before them, not by how long the light is
on.** The LED is on briefly every time; it is the off period that varies. A
decoder that measures ON duration sees four identical pulses and never decodes
anything.

**Mini pulses are keep-alives and carry no data.** They sit between every other
pulse. Counting them as bits overruns twelve and discards every command, which
looks like a timing fault rather than a parsing one.

Thresholds sit at the **geometric** mean of adjacent classes, not the
arithmetic one. The classes are separated by ratios -- 16x, 2.5x, 150x -- so a
log-domain midpoint gives each the same proportional margin. An arithmetic
midpoint between Short and Medium would leave Medium, the class a late
interrupt stretches, with almost no room.

### What I could not verify

I searched the ROM for the documented `RCNT` writes and found the constants,
but only inside a monotonically increasing data table -- coincidence, not code.
The ten genuine `0x04000134` literal sites cluster around `0x083130-0x08342C`
and one carries `0x80B0`, the same direction nibble, which is suggestive and no
more. Confirming the write sites needs disassembly, and Shonumi has already
done that work; re-deriving it would not have improved the implementation.

The timing itself still needs a scope and an LED. What is tested is the reading
of the protocol.

---

## Pad bit order: solved

Open question 2 is closed, and without hardware. The EXE chip project ships
EAGLE schematics for the real chips; our own databases give those chips their
wire IDs. Solving one against the other is eight independent chips at once,
which is stronger evidence than the one chip and a multimeter I kept
recommending.

```
pad 1 = bit 8 (MSB) ... pad 9 = bit 0 (LSB)
pad 10 = common
pad 11 = family marker, no weight
```

The Gun Del Sol family settles the weighting by itself -- three consecutive IDs
differing by one pad each:

| chip | ID pads | value | catalogued |
|---|---|---|---|
| GunDelS1 | 4 | 32 | 32 |
| GunDelS2 | 4, 9 | 33 | 33 |
| GunDelS3 | 4, 8 | 34 | 34 |

so pad 9 is worth 1 and pad 8 is worth 2. Confirmed on GunDelEX (219),
Django[SP] (279), Django[DS] (280), Otenko (215), and from the other family
LifeSync (28). Eight for eight, and both databases agree that GunSol1/2/3 are
32/33/34 in BN4 as well as BN5.

### The bug this exposed

`chip_reader.c` counted **pad 11 as a tenth ID bit**. It is not a bit at all:
every EXE 5 Progress chip ties it and no EXE 4 Advance chip does, so it marks
which family the chip belongs to. Counting it shifted every weight, and the
result was IDs that looked entirely plausible and were wrong -- the exact
failure the header warned about, sitting in the code the whole time.

Three test assertions had encoded the same wrong assumption and failed when it
was corrected, which is what they are for. The eight chips are now vectors in
`test_ui.c`.

### What is still open

Beast Link chips have not been checked this way -- the schematics are there but
the family's chips did not name-match our databases. `msb_is_lowest_pad`
therefore stays, defaulting to the confirmed Advance orientation.

---

## The build-list failure, removed rather than fixed again

Three separate hardware builds failed the same way: a file was added to `src/`
and not to `pio/src/CMakeLists.txt`. Each time the linker reported the missing
**symbols** -- `romview_init`, `uiset_move`, `settings_save` -- and not the
missing file, so it read as a code fault rather than a build-config one. Each
time the fix was to add a line, and the next new file failed the same way.

`src/` is now globbed with `CONFIGURE_DEPENDS`.

Globbing usually hides what is being built, and that is a fair objection almost
everywhere. It is the right call here because of what `src/` specifically is:
portable C, no hardware dependencies, no variant-specific behaviour, nothing
that is wrong to compile. `--gc-sections` drops whatever an image does not
reference, so the cost of compiling an unused file is build time and nothing
else.

The glob immediately pulled in `app_modes.c`, which had been sitting in `src/`
unbuilt.

`port/esp32s3/` stays an explicit list -- those files genuinely must not be
compiled everywhere, since they need hardware that may be absent. That half is
covered by `qemu/srccheck.py`, which fails if a file there is in no build and
not listed as knowingly excluded, and which also fails if the `src/` glob is
ever removed. Verified against a deliberately orphaned file.

`make lint` now runs: 12 compiles across both configurations, source coverage,
DRAM budget, and capability coverage.
