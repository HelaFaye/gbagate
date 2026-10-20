# Patches for GB-Link

gbagate runs GB-Link's adapter firmware, and runs GB-Link's host software
against it in its test suites. Doing that found defects and gaps upstream.
This directory holds the fixes as `git format-patch` series, written in each
project's own style, for GB-Link's maintainers to take or leave.

Every patch was made in a clone of the upstream repository. Each one says in
its message what was wrong, how it shows on hardware, and how it was found.

## GBLink-Firmware

Base: `Wireless-Adapter` at 1b49b76 (2.2.7, the source of the launcher's beta).
Patches 1-5 also apply unchanged to `master` (4aef46a, 2.2.3).

    git am backport/GBLink-Firmware/*.patch

| # | patch | found by |
|---|---|---|
| 1 | Zero-pad short link data packets instead of reading past them | ASan, `relay_short_packet` |
| 2 | Ignore StartHandshake/ConnectLink while the link mode has no section | UBSan + SEGV, `relay_role_command_before_mode_role` |
| 3 | Hold an early ConnectLink until the child GBA has seen the handshake | hang, `relay_connect_without_grace` |
| 4 | Bound the trade emulator's party uploads | ASan, `trade_emu_pk3_upload_any_split` |
| 5 | Stop the trade block-request timer when the section ends | inspection |
| 6 | Initialise the client RECV_DATA packet pointer | GCC 14 `-Werror` (false positive) |

The tests are in `test/gblink_sim/test_gblink.py`. To check the series, the
simulator was built from upstream's tree with the series applied, and from the
same tree without it. Only gbagate's own simulator files and its two
reimplemented transport headers came from gbagate. Under ASan and UBSan:

- **patched 2.2.7**: every test passes except the three that exercise
  gbagate-only commands (0x4F, mode 0x80);
- **unpatched 2.2.7**: the four tests named above fail as well;
- **the Wireless Adapter mode**: all eight librfu checks in `test_rfu.py` pass
  on unpatched upstream code.

gbagate's own Battle Chip Gate (mode 0x80) is not offered here. Upstream
shipped its own gate in 2.2.6 (mode 0x06), and gbagate implements that mode
too, so a second gate would only duplicate it.

## gblink-launcher

Base: `main` at bac443a.

    git am backport/gblink-launcher/*.patch

| # | patch |
|---|---|
| 1 | Label the Battle Chip Gate LED slot (2.2.6+ reports seven; the seventh read "Mode 6") |
| 2 | Recognise gbagate boards and do not offer them RP2040 firmware |

Patch 2 matters for anyone who plugs a gbagate into the launcher. The board
enumerates as a GBLink adapter, so the launcher offers it the bundled `.uf2`.
The launcher's one-click update then sends 0x43, which puts an ESP32-S3 into its
ROM serial bootloader. The launcher then waits for a PICOBOOT device that never
appears. The patch reads the USB manufacturer string, which sends the adapter
nothing, so a genuine GBLink is never probed.

`test/gblink_compat/test_launcher.mjs` runs both versions of the page in
Chromium. Its `navigator.usb` is backed by the gbagate firmware simulator, with
the firmware's real descriptors. Upstream identifies the board as "GBLink
v2.2.7" and shows the updater. Patched, it shows "gbagate (GBLink v2.2.7
protocol)", no updater, and seven labelled LED slots.

## Not patches: things worth knowing upstream

- **Celio-Client and Celio-Server do not interoperate at their current heads.**
  Since b057315 the client sends status and data with Socket.IO acks and
  retries. The public server (9f02d3e) neither acks them nor unwraps the ack
  argument. gbagate's relay test pins the client to the commit before.
- **The soft-reset pulse.** `linkLayer_rfu.c` catches AgbRFU_SoftReset by
  polling SD about once a millisecond against a 1.1 ms pulse, so a late poll
  misses it. gbagate times the pulse from its edges instead, and qualifies it
  by width. The same idea would work with the RP2040's edge latches and a
  timer.
- **Printer replies are shifted out LSB first** (`gb.cpp`), where a Game Boy
  reads MSB first. This works today only because the bytes sent (0x81, 0x00)
  read the same either way.
