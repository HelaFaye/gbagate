# Patches for GB-Link

gbagate runs GB-Link's adapter firmware, and runs GB-Link's host software
against it in its test suites. Doing that found defects and gaps upstream.
This directory holds the fixes as `git format-patch` series, written in each
project's own style, for GB-Link's maintainers to take or leave.

Every patch was made in a clone of the upstream repository. Each one says in
its message what was wrong, how it shows on hardware, and how it was found.

## GBLink-Firmware

Base: `Wireless-Adapter` at 1b49b76 (2.2.7, the source of the launcher's beta,
and still its head). Every patch but 11 also applies on its own to that base,
in any order; 11 needs 2 under it (it locks the null check 2 adds). Patches 1-5 also apply unchanged to `master` (4aef46a, 2.2.3).

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
| 8 | Shift printer replies out MSB first | `backport/verify-printer.sh` (code review) |
| 9 | Keep a mode's traffic with the host that started it | `bridge_companion_keeps_its_mode_while_usb_queries`; `test_ldn.py` with LDN's own link code |
| 10 | Silence two compiler warnings | GCC 14 `-Wextra` (no change in behaviour) |
| 11 | Stop cancel() reaching a section that has gone | ASan, `link_cancel_while_the_section_ends` (intermittent) |

The tests are in `test/gblink_sim/test_gblink.py`. To check the series, the
simulator was built from upstream's tree with the series applied, and from the
same tree without it. Only gbagate's own simulator files and its two
reimplemented transport headers came from gbagate. Under ASan and UBSan:

- **patched 2.2.7**: every test passes except the three that exercise
  gbagate-only commands (0x4F, mode 0x80);
- **unpatched 2.2.7**: the six tests named for patches 1-4, 7 and 9 fail as
  well;
- **patch 11's race** shows only now and then: 12 runs of
  `link_cancel_while_the_section_ends` on each tree, under ASan, failed 2
  times unpatched (stack-use-after-scope in `LinkModule::cancel` and in
  `PacketLayer::cancel`, both through the cleared-too-late section pointer)
  and 0 times patched;
- **upstream's own build** (`build.yml`: Zephyr v3.7-branch, SDK 0.16.8,
  `west build -b rpi_pico`): the base and every step of the series build with
  no warnings; the whole series adds 1.4 KB of flash and 8 bytes of RAM. Not
  run on RP2040 hardware;
- **the Wireless Adapter mode**: all of `test_rfu.py`'s librfu checks pass on
  unpatched upstream code.

`backport/verify.sh` repeats that check: it builds the simulator from
upstream's tree at the base, with the series or `--unpatched`. Patch 9
changes `layers/transport.*`, which the wireless-cable tests extend with a
local route (PATCHES.md P8); `backport/harness/` adds exactly that route to
whichever tree is built, so the cable tests run against upstream's own
transport code too.

Patch 9 also shows end to end: `test/gblink_compat/test_ldn.py` runs
GB-Link-Switch-LDN's own `pico_link.c` on the bridge UART while a USB host
polls GetFirmwareInfo for 4 s. Before the patch LDN loses the telemetry,
times out and sends SetMode again (resetting the adapter under the game);
after it, LDN's one SetMode stands and every query is answered on USB.

Patch 8 changes nothing a test of the shipped firmware can see: the only
replies sent, 0x81 and 0x00, read the same in either bit order.
`backport/verify-printer.sh` sets the status to 0x02 in both trees for the
experiment: the simulated Game Boy, which samples MSB first, reads 0x40
without the patch and 0x02 with it.

Patch 7 came later, from a code review. Its test runs upstream's
`payloads/pokemon.cpp` on its own under ASan: unpatched it reports a
global-buffer-overflow on the first index past the party, and patched it
passes.

gbagate's own Battle Chip Gate (mode 0x80) is not offered here. Upstream
shipped its own gate in 2.2.6 (mode 0x06), and gbagate implements that mode
too, so a second gate would only duplicate it.

## Checking all of it

    backport/verify.sh              the firmware series (and --unpatched)
    backport/verify-printer.sh      patch 8's evidence
    backport/verify-celio.sh        the Celio-Server series (--client-suite
                                    adds Celio-Client's own tests)

All three need the checkouts `make compat` makes, and build into
`build/backport/`. Nothing is sent anywhere: the patches stay here until
someone submits them.

## gblink-launcher

Base: `main` at 94b9a95 (was bac443a; both patches apply unchanged). The
upstream repository is no longer publicly clonable, so the compat suite
tests a private mirror of 94b9a95 (`test/gblink_compat/run.sh`).

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

## Celio-Server -- superseded

These two patches were written against `GB-Link/Celio-Server` at 9f02d3e,
taken at the time for the current server. It is a fork that stopped there.
`Celio-Link/Celio-Server`, the original, already has both changes:

| # | patch | upstream |
|---|---|---|
| 1 | Forget a client's state when it leaves the session | a9e6c00 "Fixed bug where leaving could cause a stale handshake" |
| 2 | Speak Celio-Client's acked protocol | 5ead974, 87923bf, 4299ffa, d85fbcc (3-4 October 2026) |

How upstream did it, against ours:

- **Stale handshake (patch 1).** The same line: delete the leaver's
  `clientState` in `removeClient`. Upstream also deletes it from
  `SessionManager.clientToSession`, which we missed: without that a client
  that left could not join or create another session ("AlreadyExists").
- **Acks (patch 2).** Same wire result, different design:
  - *Old clients.* Ours detected per client whether it acks and kept
    serving pre-ack clients plainly. Upstream requires the new protocol:
    `deviceData` must be a batch array, and create/join without an ack
    callback are ignored.
  - *Sending.* Ours queued every acked send of a session -- data,
    commands, both directions -- through one `concatMap`, so each packet
    waited a full round trip for the one before it. Upstream gives each
    client a `PipelinedSender` with 64 data batches in flight, and a
    separate ordered queue for its commands. Under latency that is the
    difference between one packet per round trip and 64.
  - *Validation.* Upstream checks every payload (`validation.ts`) and acks
    `false` for a malformed one; ours acked `true` before looking.
  - *Duplicates.* Upstream remembers sequence numbers only and dropped
    `requestData` (never used); ours kept every packet.
  - *Disconnects.* Upstream resets the 5 s disconnect timer when a client
    comes back first (2edc83a), and a retry that fails because the client
    is gone does not evict or log (`ClientDisconnectedError`).
  - *Leaving.* Both spare the leaver the `sessionClose`: ours by skipping
    it in `evict`, upstream by removing it first.

Nothing here needs sending upstream. What would help is GB-Link syncing its
fork from Celio-Link. The pull request opened against the fork (#1) can be
closed. The patches stay in the tree as a record; `verify-celio.sh` no
longer runs.

## Celio-Link/Celio-Server -- the ack timeout

`Celio-Link-Server/0001-ack-timeout-follows-the-line.patch`, against
Celio-Link/Celio-Server 8f509bd: not sent; it is the user's to send.

**The problem.** `Client.emitWithRetry` waits a fixed 1 s for each ack, sends
again 5 times 100-500 ms apart, then ends the session. Every server-to-client
message goes through it: session events, commands, data. On a line whose
round trip nears 1 s, every copy's wait runs out before its ack can come
back. socket.io throws away an ack that arrives after its own timeout, so
even the first copy's late ack is lost. And `pingTimeout: 2000` drops the
socket of any client whose round trip nears 2 s. A satellite link or a bad
mobile connection cannot hold a session; the client can do nothing about it,
since it already acks at once.

**The fix.**
- Each copy waits twice as long as the last: 1, 2, 4, 8, 8, 8 s, as TCP backs
  off its retransmit timer.
- Every copy listens for its ack until the last one's time is up, so a late
  ack for an earlier copy counts. Duplicates are already harmless: clients
  drop them by uuid and sequence.
- Once a round trip is known, the first wait is twice the smoothed round
  trip (first sends only, Karn's rule), 1-8 s.
- `pingTimeout` goes from 2 s to 5 s.

gbagate's own client does the same in the other direction (`src/celio.c`).

**Measured** with `test_celio_latency.mjs`: two board clients, delay and
jitter on each one's socket, a whole session per row.

| round trip | 8f509bd | with the patch |
|---|---|---|
| 4-600 ms, and 1 s stalls | links | links |
| 1.2 s | session ended | links, frames in 12.8 s |
| 2 s | session ended | links, 21.6 s |
| 3 s | never connects | links, 30.4 s |

**Many players at once** (`test_celio_crowd.mjs`). A Celio session is two
players, one link cable, so many players means many sessions on one server.
Each case runs its pairs at the same time on one server, plus a fifth device
that tries to join a full session:

| case | 8f509bd | with the patch |
|---|---|---|
| 4 players, two LAN pairs | both link | both link |
| 4 players, LAN + 1.2 s | LAN links; the 1.2 s session ends | both link; LAN in 1.1 s, as alone |
| 8 players: 4, 80, 600 ms and 2 s | the 2 s session ends | all four link |

Every case also checks:
- the intruder is refused with "Session is full";
- no frame crosses into another session;
- on the patched server, the LAN pair is no slower beside a slow one. The
  round-trip estimate is per client.

`run.sh` builds `celio_server_patched.mjs` from this patch file, so CI tests
the patch itself.

A LAN pair once failed to link in a crowd, locally and then in CI (on both
servers). The cause was the simulator, not a server: it seeded the boards'
client IDs with `getpid() ^ time(NULL)`. Simulators started back to back
have neighbouring PIDs, and PIDs p and p ^ 1 started in seconds t and t ^ 1
seed alike. Two boards with one ID make the server take the second for a
reconnect of the first, and it drops the first's socket. The IDs now come
from `getrandom`, and the crowd test checks that every board has its own.
Its delay line also keeps order strictly now (one queue, one timer), as a
TCP connection does; the numbers above are unchanged.

To run it against a build of the patched server:
`CELIO_SERVER=<bundle.mjs> node test/gblink_compat/test_celio_latency.mjs build/gblink_compat`.

**What to send.** An issue on Celio-Link/Celio-Server, with the patch and the
table:

> *Sessions end on links with a round trip over about 1 s.*
> `emitWithRetry` waits a fixed 1 s per copy, and socket.io drops an ack
> that comes after its timeout. So on a slow line no ack is ever accepted,
> and after five retries the session ends. Proposal: double the wait on each
> retry, keep each copy's ack listener until the last deadline, start from
> the measured round trip, and raise `pingTimeout` to 5 s. With these, two
> clients hold a session at 1.2, 2 and 3 s round trips that today end or
> never connect. Nothing changes on a fast line.

## Celio-Link/Celio-Server -- four-player sessions

`Celio-Link-Server/0002-sessions-of-up-to-four-players.patch`, on 8f509bd
with 0001. Not sent: sending it is the user's call.

**Why.** A GBA Multi-Play link joins up to four GBAs. Pokémon's Cable Club
multi battles, the Berry Blender and the record corner use it, as do other
games. Celio sessions were fixed at two, and every relay went "to the
opposite client".

**What changes.** A two-player session is exactly what it was on the wire.
- **Creating:** `sessionCreate` takes an optional `{players: 2..4}`. Today's
  clients send `null`, which means 2. The create/join reply adds `players`
  and `slot` (the order of joining, 0 = the creator).
- **`partnerJoined`** means the session is complete: sent to the first
  player in a pair, as now, or to everyone waiting once the last joins.
  `partnerLeft` follows the same rule before the link starts. After it
  starts, a leaver still ends it for everyone.
- **Roles:** a pair as now. A Multi-Play link has one parent GBA and up to
  three children. In GB-Link's terms the parent's board answers as the
  children (`SetModeSlave`) and each child's board drives the link as the
  parent (`SetModeMaster`). So with more than two players, the first ready
  gets `SetModeSlave` and link ID 0, and the others get `SetModeMaster` and
  IDs 1–3. Every command carries the player's `id`, and a board asking again
  keeps its ID. (A first draft gave one master and three slaves, which would
  have made three parents.)
- **Starting:** the handshake starts only when the session is full and every
  player has one, and for all of them. ConnectLink goes to all the others.
- **Data:** each player's packets go to every other player, through each
  receiver's own sender. With more than two players each packet carries
  `from`, the sender's link ID, so a receiver can put each word in its
  Multi-Play slot and keep each sender's order.
- **Boards with several GBAs:** a gbagate carrier board has up to three
  GBAs on its own cable (`doc/CARRIER.md`). `sessionCreate` and
  `sessionJoin` take `gbas` (`sessionJoin` as `{id, gbas}`; a bare id, as
  today, is one GBA). `players` counts GBAs, and a session is full when its
  clients' GBAs fill it; a join that does not fit is refused. Such a board's
  link ID is the first of its slots, and its slots follow in a row: a
  three-GBA board ready first has 0–2, and the fourth player gets 3.

**Tested.**

*`test/session.test.ts`*, upstream's vitest style, 24 tests:
- the options;
- a pair, as before: roles, no link IDs, packets unchanged;
- four players:
  - full at four, a fifth refused;
  - told only when complete;
  - one parent and three children, with their link IDs, kept on a repeat;
  - no handshake until all four;
  - ConnectLink to the other three;
  - each player's data reaches the others once and in order, with its
    sender;
  - leaving before and after the start;
- boards with several GBAs: the options, room counted in GBAs, a join that
  does not fit refused, link IDs as first slots (3 + 1, 1 + 2 + 1), data
  marked with them, the handshake waiting for every board.

The three pair tests pass with and without the patch, and the other 21 fail
without it. All 30 of the server's tests pass with it, and `tsc --noEmit` is
clean.

*`test/gblink_compat/test_celio_mp4.mjs`*, end to end with the board's own
client: two simulated boards, one with three GBAs and one with one, through
the real server. The board's Celio client, `src/mp_online.c` and the relay
run for real, and the GBAs on each board's cable are simulated at the
games' protocol. The three-GBA board gets link ID 0, the other 3; every GBA
reaches frames, hears every other GBA's command and sees no checksum it
disagrees with.

*`test/gblink_compat/test_celio_4p.mjs`*, end to end: four socket.io clients
against the real server bundle, on 2, 40, 150 and 600 ms lines.
- One parent and three children: link IDs 0–3, carried on every command.
  The pair beside them gets none.
- The handshake and ConnectLink go where they should.
- Every player gets all 30 of each other player's packets, in order,
  marked, and none of its own.
- A fifth player is refused.
- A pair running on the same server at the same time still gets
  packets with no `from`.

**0001 amended alongside.** Upstream's `client.test.ts` passes a `backoff`
option, which 0001 had dropped, and it fails copies outright. 0001 now
restores that behaviour: a copy that fails outright is resent after
`backoff`, and a copy that is only slow is resent on the doubling timer.
Upstream's two `emitWithRetry` tests take 11 ms, as before. Without the fix
they took 3 s each and passed only because vitest does not type-check.

**Pull request text** (for Celio-Link/Celio-Server, after 0001):

> *Sessions of up to four players, for Multi-Play links.*
> A GBA Multi-Play link joins up to four GBAs. This lets `sessionCreate`
> take `{players: 2..4}`; clients that send nothing get two, as today, and a
> two-player session is unchanged on the wire. A larger session tells its
> players `partnerJoined` once complete, gives one player the parent's role
> (link ID 0) and the others the children's (1-3) with the ID on every
> command, starts the handshake only when all have one, relays each player's
> data to all the others marked with the sender's link ID, and ends for all
> if anyone leaves a started link. A client may bring several GBAs (one
> board with up to three on its cable): `players` counts GBAs, and such a
> client's link ID is the first of its slots. New `test/session.test.ts`
> (24 tests; 21 fail without the change) and an end to end run with four
> clients on 2-600 ms lines.

**What clients need.** A client that wants four players must:
- create with `{players: 4}` (and `gbas` if it brings more than one);
- keep per-sender order by `from`;
- show its GBAs the others. On the board that is the Multi-Play relay
  (`src/mp_online.c`, `doc/MULTIPLAY.md`).

## Not patches: things worth knowing upstream

- **Multi-Play link counters.** gbagate's `0x4E` reports words clocked,
  words dropped and late answers per PHY, which is what a bring-up or a field
  report of a flaky link needs (upstream has the Wireless Adapter's `0x4C`
  already). Offering it as a patch would claim a command number in GB-Link's
  own protocol, which is theirs to allocate; it is offered as an idea.
- **The soft-reset pulse.** `linkLayer_rfu.c` catches AgbRFU_SoftReset by
  polling SD about once a millisecond against a 1.1 ms pulse, so a late poll
  misses it. gbagate times the pulse from its edges instead, and qualifies it
  by width. The same idea would work with the RP2040's edge latches and a
  timer.
