# Four players over the network: design

A Multi-Player link joins up to four GBAs, for Pokémon's Cable Club multi
battles, the Berry Blender, the record corner and other games. Over the
network each GBA has its own board, so each board must stand in for the
other three GBAs on its GBA's cable. This is the plan and the evidence for
it. The status of each piece is in the table at the end.

## What the games say on the cable

From four real Emeralds on a modelled cable (`test/mgba/link4.py`, which
writes `build/emerald/link4-trace.txt`: every transfer, all four words):

```
  1-10   b9a0 b9a0 b9a0 b9a0     handshake: every unit answers B9A0
    11   8fff b9a0 b9a0 b9a0     the parent: connect
    12   b9a0 b9a0 b9a0 b9a0
    13-  frames of 9 transfers
```

A frame is 8 command words from each player, then a checksum word:

```
frame 4  p0: bbbb 003c 0080 0000 0000 0000 0000 0000 | f1e2
         p1: bbbb 003c 0081 0000 0000 0000 0000 0000 | f1e2
         p2: bbbb 003c 0082 0000 0000 0000 0000 0000 | f1e2
         p3: bbbb 003c 0083 0000 0000 0000 0000 0000 | f1e2
```

- **The checksum is shared.** Every unit sends the same ninth word: the
  16-bit sum of all four players' eight words in that frame. (Frame 4:
  4 × BBBB + 4 × 003C + 80 + 81 + 82 + 83 = 2F1E2.) So it depends on what
  every player sent in that frame.
- **An idle player sends eight zeros.**
- **Each player's commands are its own.** `BBBB size id` starts a block send
  (here each player's 60-byte link record) and `8888` continues it. The games
  queue each player's commands separately: in frame 8 each player's record
  carries its own trainer ID.

## What each board does

Exactly one GBA is the parent (link ID 0). Celio-Server's four-player
sessions (`backport/Celio-Link-Server/0002`) assign the roles:
- **The parent's board** gets `SetModeSlave` and answers as children 1–3.
- **Each child's board** gets `SetModeMaster` with its link ID. It drives
  the cable as the parent does and answers as the other children; its GBA
  fills its own slot.

Each board builds its own GBA's view of the link, one frame at a time:

- **Remote players:** a queue of 8-word commands for each of the three,
  from the session's data (`from` = link ID).
- **At each frame:** take one command per remote player, or the idle
  command if its queue is empty, and send its words in that player's slot.
- **The checksum word,** in every slot the board answers for, is the sum of
  the frame as it is on this cable: the three remote commands and the eight
  words the local GBA sent. The GBA computes the same sum and sees no
  error.
- **The local GBA's command** for that frame goes to the session. Idle frames
  are not sent, so a sender whose cable runs slightly faster does not fill
  the others' queues. The 2-player relay thins idle keys the same way.
- **Handshake:** answer `B9A0` for a remote slot only once the server has
  started the handshake (`StartHandshake`, sent when all four are ready).
  The parent's `8FFF` reaches the children's boards as `ConnectLink`, and
  their boards then send it in slot 0.

Frames no longer line up across boards: player 1's command X can land in
frame 5 on one cable and frame 7 on another. That is safe only because the
games keep a queue per player, which the trace shows. Activities that send
held keys every frame (minigames) see the latency as lag, as they do over
the 2-player relay.

## Status

The relay's logic is built and proven under real games:
- `src/mp_relay.c` is the frame builder above, with no I/O;
  `test/test_mprelay.c` checks each rule.
- In `test/mgba/link4.py`, four Emeralds each sit on their own cable. On
  each cable an `mp_relay` answers the other three slots, driving the cable
  at the parent's measured cadence where its GBA is a child. The relays
  pass commands to each other through a delay line, and a stand-in for the
  server sends StartHandshake and ConnectLink.
- Results: with no delay and with 50 ms each way, every check that passes on
  one shared cable passes the same way. The games see four players, the
  parent alone is master, and each knows its ID. Every game holds the same
  four trainer IDs in link order, with no link errors. No relay sees a bad
  checksum from its GBA, and each relays its GBA's commands and receives
  the others'.

What remains is putting it on the board, in the table below.

## What has to be built

| piece | where | status |
|---|---|---|
| The 4-player frame builder above | `src/mp_relay.c` | done |
| The bus engine: the board's side of one transfer in every slot it answers, on the carrier's chain of up to three GBAs, as child (a local GBA is parent) or as parent (standing in for a remote one) | `src/mp_bus.c` | done, tested against modelled GBAs (`test/test_mpbus.c`); timing not yet on a scope |
| The relay as the driver's client, with the remote parent's cadence | `src/mp_link.c` | done, tested with the engine through the handshake and a frame |
| The ESP32-S3 driver's four-player mode: the engine in the SC interrupt (child) or the link task (parent), the chain's presence probe | `port/esp32s3/gba_link_esp32s3.c`, `gba_link_mp4.h` | written, compiled by the lint for every board; not run on hardware |
| Four-player sessions in the board's Celio client: create and join with the players and the board's GBAs, the session's commands and data to the relay, each sender's data in its own order | `src/celio.c` (its multi mode) | done, tested (`test_celio`) |
| The server side: boards bringing several GBAs, link IDs as first slots | `backport/Celio-Link-Server/0002` | done, 24 server tests |
| The glue: roles to bus layouts, the relay's statuses, its commands in data packets (each entry tagged with its slot, anything malformed refused) | `src/mp_online.c`, in `port/gblink/celio_glue.cpp` | done, tested with two boards (`test_mponline`) and end to end through the real server (`test_celio_mp4.mjs`) |
| The firmware mode: Link: Online's Players (2–4) and Local Players (1–3) rows; more than two players or more than one local GBA starts a Multi-Play session, which waits for the local GBAs to be found on the chain. Any board can play its one GBA | `pio/src/gate_app.c`, `src/ui_screens.c` | done; not run on hardware |
| The end-to-end test with real games: four mGBA Emeralds on boards of 1+1+1+1, 3+1, 1+3 and 2+2 GBAs, each board running its own Celio client, `mp_online` and relay, through the real patched Celio-Server; `link4.py`'s checks unchanged | `test/mgba/link4_online.py` (board mode in `mgba_multi.c`) | done: all four pass |

### The bus engine and its model

`test/test_mpbus.c` runs the engine against GBAs modelled from the wire's
shape. The parent drops SC and sends. A child sends a few microseconds
after its SI goes low, then drops its SO. Every unit hears every word by its
start bit, and SC rising ends the transfer. For every layout the chain can
carry (2–4 players, 1–3 GBAs, the board as child or as parent), with random
turn delays, interrupt latency and the clock wrapping, every GBA ends with
every slot's word and no two units ever drive SD at once. The test was
checked by breaking the engine: wrong bit order, late sampling, sending over
a stop bit, not waiting for a start bit and not passing the turn are all
caught.

What the model cannot settle: the real GBA's delay between its SI going low
and its start bit, and how long a parent waits for a slot that never
starts. The model sweeps the first (1–60 µs) and assumes the second is at
least the board's own 168 µs. A scope on the carrier settles both.

### The layout a board takes

The server gives each board a link ID: the first of its slots. A board's
GBAs take that many slots in a row, and the board answers every other
slot. With link ID 0 its port 1 GBA is the parent and the bus runs as
child. With any other ID a remote player is the parent and the board stands
in for it: as parent on the bus, it sends the slots before its GBAs back to
back and passes the turn (SO low) just before its first GBA's. So a board
with one GBA can be the fourth player: it answers slots 0–2 and its GBA is
slot 3. `test_mpbus` checks every such layout.

### Real games through the real server

`test/mgba/link4_online.py` runs four Pokémon Emeralds (mGBA cores, in
lockstep) on boards of one to three GBAs each. Each board runs the board's
own code: its Celio client in its multi mode, `mp_online` and the relay.
The script is each board's network driver, with one WebSocket per board to
the real Celio-Server with `backport/Celio-Link-Server` applied. Each
board's cable is modelled per transfer of all four slots (the electrical
layer under it is `test_mpbus`'s).

The checks are `link4.py`'s, which four Emeralds pass on one real cable:
- every game sees four players and only the parent is master;
- each game knows its own link ID;
- the games exchange their link player records, and every game holds the
  same four trainer IDs in link order;
- no game has a link error.

The boards add their own: no relay sees a bad checksum, and nothing is
dropped, lost or refused.

| boards | slots | |
|---|---|---|
| 1 + 1 + 1 + 1 | each board one: from any board, one local and three remote | all pass |
| 3 + 1 | the three-GBA board 1–3, standing in for the remote parent | all pass |
| 1 + 3 | the three-GBA board 0–2, its port 1 GBA the parent | all pass |
| 2 + 2 | 2–3 and 0–1 | all pass |

The server gives link ID 0 to whichever board reports AwaitMode first; in
these runs that was the first to join, which is why 3 + 1 and 1 + 3 cover
both roles of the three-GBA board.
