# Four players over the network: design

A Multi-Player link joins up to four GBAs, for Pokémon's Cable Club multi
battles, the Berry Blender, the record corner and other games. Over the
network each GBA has its own board, so each board must stand in for the
other three GBAs on its GBA's cable. This is the plan and the evidence for
it. Nothing in it is built in the firmware yet.

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

## What has to be built

| piece | where |
|---|---|
| A PHY mode that answers in up to three slots, and one that drives slot 0 and answers for the other children | `port/gblink/link_phy.h`; the ESP32-S3 driver; the simulator |
| The 4-player frame builder above | a new section beside the vendored `UsbSection`, under gbagate's extension mode numbers |
| Per-sender queues and four-player sessions in the board's Celio client | `src/celio.c` |
| A cable between mGBA's Multi-Player hardware and the simulator's board, as `rfu_wire.h` is for the Wireless Adapter | `test/mgba`, `test/gblink_sim` |
| The end-to-end test: four mGBA Emeralds, four simulated boards, one four-player Celio session; `link4.py`'s checks must pass unchanged | `test/mgba` |
