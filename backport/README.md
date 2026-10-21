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
| 7 | Bound the slot index of a trade request | ASan, `trade_emu_party_index_bounded` (code review) |

The tests are in `test/gblink_sim/test_gblink.py`. To check the series, the
simulator was built from upstream's tree with the series applied, and from the
same tree without it. Only gbagate's own simulator files and its two
reimplemented transport headers came from gbagate. Under ASan and UBSan:

- **patched 2.2.7**: every test passes except the three that exercise
  gbagate-only commands (0x4F, mode 0x80);
- **unpatched 2.2.7**: the four tests named above fail as well;
- **the Wireless Adapter mode**: all of `test_rfu.py`'s librfu checks pass on
  unpatched upstream code.

Patch 7 came later, from a code review. Its test runs upstream's
`payloads/pokemon.cpp` on its own under ASan: unpatched it reports a
global-buffer-overflow on the first index past the party, and patched it
passes.

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

## Celio-Server

Base: `main` at 9f02d3e (its head).

    git am backport/Celio-Server/*.patch

| # | patch |
|---|---|
| 1 | Forget a client's state when it leaves the session |
| 2 | Speak Celio-Client's acked protocol, and keep serving earlier clients |

The current Celio-Client and the public Celio-Server could not link at all.
Since b057315 the client acks and retries what it sends, and the commits after
it expect the server to do the same (deviceData as arrays both ways, acked
commands, acked session events with a uuid). The server did none of it, and
rxjs' `fromEvent` turned every acked emit into `[payload, ack]`. Patch 1 is a
separate bug the client's tests also catch: a partner who left before the link
started kept blocking the next pair's handshake. The client's tests start a
server from `/Users/nilskirchhof/Repo/linkServer`, the package's own name, so
its author's copy has probably moved on the same way; these patches make the
public one match.

The server learns what each client speaks from its traffic, so an earlier
client is served as before. `backport/verify-celio.sh` checks the series:

- **Celio-Client's own suite** (its `tests/`, pointed at the patched server):
  25 of 25 pass, against 13 unpatched;
- **gbagate's relay test** (`test_celio.mjs`, two adapters linked through the
  server) with the current client (be562a9) at both ends, the client from
  before b057315 at both ends, and one of each, in both orders: all four pass
  patched; unpatched only the two earlier clients link.

## Not patches: things worth knowing upstream
- **The soft-reset pulse.** `linkLayer_rfu.c` catches AgbRFU_SoftReset by
  polling SD about once a millisecond against a 1.1 ms pulse, so a late poll
  misses it. gbagate times the pulse from its edges instead, and qualifies it
  by width. The same idea would work with the RP2040's edge latches and a
  timer.
- **Printer replies are shifted out LSB first** (`gb.cpp`), where a Game Boy
  reads MSB first. This works today only because the bytes sent (0x81, 0x00)
  read the same either way.
