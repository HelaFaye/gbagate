# A shared overworld: plan

Where the memory agent (`doc/AGENT.md`) goes next, and the netcode a shared
Emerald overworld needs. This is a design to build against, in order; none of
it past step 1 exists yet. Each step is tested on the agent emulator
(`test/agent_emu`) before the next is built on it.

## 1. The agent inside a running game

Today the agent runs alone, multiboot with no cartridge. To edit a running
game it has to live beside the game.

**Boot.** GBATEK ("Multiboot Slave with Cartridge"): a GBA with a
cartridge in enters multiboot instead of the game if SELECT and START are held
at power-on. So the player holds them, the board sends the agent, and the
cartridge is already there to read: the agent checks its header (game code,
version, header checksum) against a map it has. No cartridge goes in or out
with the power on.

**Surviving the game's own start.** The agent cannot simply jump to the
ROM's entry and expect to be there afterwards:

- The game's start-up resets RAM and I/O through the BIOS's RegisterRamReset
  (or its own clearing). GBATEK: that clears all of EWRAM and IWRAM *except*
  `0x03007E00`–`0x03007FFF`, resets the SIO registers if asked, and always
  blanks the screen.
- It then installs its own interrupt handler at `0x03007FFC` (the BIOS
  forwards every IRQ through that pointer, in ARM state, with
  `0x03007F00`–`0x03007F9F` as the IRQ stack; GBATEK "BIOS Interrupt
  handling").

So hooking `0x03007FFC` before the jump is overwritten by the game, and the
agent's code anywhere in RAM may be erased. What survives is a per-game
question, answered from the map:

1. Where to live: a region that game's start-up does not clear and its code
   never uses. The 512 bytes at `0x03007E00` survive every reset but hold
   the BIOS's stacks and the IRQ vector, so at most a few words there.
2. When to take hold: after the game's start-up, not before. The map names a
   point the game reaches once it is running (its main loop, its own IRQ
   table in RAM); the agent patches the game's RAM-resident IRQ dispatch
   there, so the game's handler stays the handler and calls ours.
3. How to get there: run the game's start-up ourselves up to that point, or
   start the game normally and have the board re-send a small agent through
   the game's own link code. Which one works is the first experiment of
   step 1, on the emulator with the game's real start-up code.

**Sharing the serial port.** The game uses SIO itself for link play. The
agent serves only while the game's link is idle (the game's link state
variable, from the map) and hands the port back with SIOCNT restored, so a
link battle started through the agent uses the game's own link code
untouched.

**Timing.** Work is done in VBlank, a bounded number of words per frame, so
the game never drops a frame. The agent already uses GBATEK's SO ready
handshake (`doc/AGENT.md`): when an interrupt or the game delays it, the
board waits for it, instead of clocking a word it would lose. The emulator
gains a VBlank IRQ and a scripted "game" loop to prove the budget.

## 2. Per-game maps

A map names what the agent may touch in one exact ROM: addresses of
variables and functions, the free IWRAM and EWRAM, the IRQ table. For
Emerald it comes from pret's `pokeemerald`, whose build is byte-identical to
the retail ROM, so its linker symbol file gives real addresses. The map is
generated from that file by a tool, checked against the ROM's game code and
checksum at boot, and kept as a small table on the board. Nothing in it is
typed by hand.

The first symbols needed:

| what | for |
|---|---|
| the main callbacks (`gMain`) | knowing which screen the game is on; starting a battle, a warp, a script |
| the player's object event and position | sending our steps |
| the object event table | drawing other players with the game's own sprites |
| the map header and backup map buffer | loading a custom map built from existing tilesets |
| the script engine entry | dialog and events, run the game's own way |
| the save blocks | the trainer card, the party, for the server |
| the link state | knowing when the serial port is the game's |

Functions are called through `CALL` with the arguments the map's signature
says. Every write goes through the map, so a wrong ROM is refused and not
corrupted.

## 3. What the game does, what the server does

The game does everything it already knows how to do: walking, drawing,
menus, dialog, battles. The server decides what is shared: who is where, who
may talk to whom, what map an instance is. The board is the go-between.

| feature | how |
|---|---|
| other players walking | each is an object event using a player sprite; their steps arrive as movement actions the game animates itself |
| entering buildings | a warp; the server moves the player to that map's instance, and the players shown change |
| dialog | the game's script engine, with text the server sends |
| trainer cards | each player's card is read from their save at join; the game's own card screen shows a fetched one |
| battles | the server pairs two players and relays their link exactly as Link: Online does; the agent starts the link battle from the overworld instead of the Cable Club |
| trades | the same, with the trade screen |
| custom maps | layouts built from tiles the game already has: the server sends metatile ids and collision, the agent writes them to the map buffer |

The game's own limits are the design's limits. It has a fixed number of
object event slots, shared with NPCs, so a screen shows only some of the
players near you: the server picks the nearest. A map's size is bounded by
the backup map buffer.

## 4. Netcode

Movement in Gen 3 is on a tile grid, in whole steps: a walk is 16 frames, a
run 8, a bike 4. So players are synced as steps, not as continuous vectors,
and a step is tiny:

```
step: player id (2) | x, y (2 + 2) | facing + movement kind (1) | tick (1)
```

**Authority.** The server owns positions. A player's own step happens at
once on their GBA (the game does it); the board sends it, and the server
checks it against the map's collision. A refused step snaps the player back,
which in normal play never happens, so it costs nothing to the honest player
and stops a patched one.

**Showing others: a short delay, not prediction.** Each remote player's
steps are played back about one step late from a small queue. Steps are
discrete, so there is nothing to extrapolate; a late step only shifts the
animation. If the queue grows past two steps (a stall, then a burst), the
next ones play at run speed until it catches up, and past a limit the player
is placed directly. This is what keeps a 300 ms phone connection looking
smooth.

**Two channels.**

| channel | carries | delivery |
|---|---|---|
| state | steps, presence | unreliable: only the newest matters; each packet acks the last few by bitfield, and a lost step is repaired by the next snapshot |
| commands | chat, interact, battle and trade requests, trainer card fetches, warps, map data | reliable, in order, resent on the RTO the Celio client already uses |

Both go over UDP where the network allows, over a WebSocket where it does
not. Over a WebSocket the state channel is still reliable underneath, so the
server sends only the newest state per player and drops what a slow socket
has not taken.

**Interest management.** The world is cut into chunks a screen across
(15 × 10 tiles); a player hears the players in their chunk and the eight
around it. Buildings are their own maps and so their own areas. A server tick
(20 per second) sends each player only what changed in their area since the
last snapshot they acknowledged.

**Chunk and map loading.** Map data is sent by the command channel ahead of
need: the chunks around a player, and a building's layout when they step on
its door. It is small: two bytes a metatile, so a 64 × 64 map is 8 KiB, sent
once and cached on the board.

**Scale.** At hundreds of players per server the cost is the fan-out, not
the CPU. A player who sees ten others walking receives about ten 8-byte steps
every quarter second: about 300 bytes a second, plus headers. Per area, the
server's work is linear in who is in it, and areas are independent, so a
server shards by map.

**Latency, measured.** The harness that drives two boards through the real
Celio server under delay, jitter and stalls (`test_celio_latency.mjs`) is
how this netcode will be tested too: a scripted crowd of simulated boards
against the server, each on its own bad line.

## Tested on mGBA (Emerald)

`make mgba-emerald EMERALD_ROM=...` runs `test/mgba/union_room.py`. It
uses Ubuntu's libmgba and the user's own ROM, which must be the build pret's
pokeemerald matches; nothing derived from it is committed. Addresses come
from pret's symbols. It shows, from power-on:

- **The game's own warp works from injected code.** A Thumb routine runs once
  as `gMain.callback2`, calls `SetWarpDestination` and `DoWarp`, and hands
  back to `CB2_Overworld`.
- **The Union Room needs a Wireless Adapter.** Its on-resume script runs
  `RunUnionRoom`, which ends in "Communication error" with none.
- **The RAM map header can be redirected.** The game copies the header from
  ROM into `gMapHeader`, then runs the room's scripts from that copy a few
  frames later (`ResumeMap`). Pointing `mapScripts` at an empty table on the
  frame of the copy gives a quiet room. The same window serves `events` (who
  stands in the room) and `mapLayout`. The copy happens twice during a warp,
  so the write must follow it, not lead it.

**A Wireless Adapter on mGBA.** Our firmware's Wireless Adapter mode
(0x07), unchanged, in the GB-Link simulator, plugged into mGBA's link port:
- **The cable** is shared memory (`test/gblink_sim/rfu_wire.h`):
  - On the simulator's side, `sim_rfu_wire.c` implements the same
    adapter-side driver as `sim_rfu.c` (`GBLINK_RFU_WIRE=<file>` selects it).
  - On the game's side, an mGBA serial driver in `test/mgba/mgba_shim.c`
    plays the GBA's serial hardware. Transfers the game clocks (GBATEK:
    Normal 32-bit, 2 MHz) take the adapter's staged word, the serial IRQ is
    raised, SIOCNT bit 2 shows the adapter's SI, and the reset pulse on SD is
    read from RCNT.
- **Pacing.** The adapter is firmware with real-time waits, so the emulator
  is paced to real time. An mGBA timing event every 256 cycles brings the
  wires up to date and waits for the wall clock.
- **One player** (`rfu_union.py`): the real `RunUnionRoom` runs with no
  error. The game logs in and alternates hosting (0x19/0x1A) with searching
  (0x1C-0x1E). The wire shows no unstaged or late words.
- **Two players** (`rfu_union2.py`): two mGBA processes, each on its own
  simulator, with the 64-byte radio stream relayed between the adapters.
  - Every scan finds the other adapter.
  - Each room lists the other trainer among its players.
  - Each field draws the other with the sprite the game's own table
    (`sUnionRoomObjGfxIds`) gives their gender and trainer ID.

  This is the real Union Room, two players together, on our adapter
  emulation.

**Four on one cable** (`link4.py`, `mgba_multi.c`): four mGBA cores in
lockstep (1,024-cycle quanta, one thread each) on a modelled Multi-Player
cable, as GBATEK gives it:
- SI/SD terminals and IDs;
- the parent's Start;
- every unit's SIOMLT_SEND latched in turn;
- SIOMULTI0-3 the same on all at the end;
- mGBA's cycle counts per transfer.

Four new games, each with its own trainer, call the game's own `OpenLink()`.
Every game sees four players, and only the parent is master. After the
parent's `CheckShouldAdvanceLinkState()`, every game holds the same four
trainer IDs in link order, with no link errors. This is the ground truth for
the board's 4-player relay.

**Four players in one overworld** (`overworld_sync.py`, step 3 of the order
below): four Emeralds warped into Littleroot Town, each walking a scripted
route with emulated controls (steps, turns in place, bumps into walls and
each other). On every game an agent stand-in runs as a game Task:
`CreateTask` puts it in the game's own `RunTasks`, called every frame. It
reads a mailbox and acts with the game's own functions:
- `SpawnSpecialObjectEventParameterized` draws each other player as Brendan
  or May;
- `ObjectEventSetHeldMovement` walks or turns them, one step after another;
- `RemoveObjectEvent` takes them away.

A server stand-in reads each player's map, tile, facing and elevation every
frame and queues their steps for the others.
- **While walking:** every figure stays within one step of its player.
- **Once everyone stops:** every figure, on every game, stands on its
  player's tile facing the same way, drawn and on screen where the two
  positions say.
- **Screens:** the four screens show the same four trainers in the same
  places.

**In and out of a building.** By Professor Birch's lab:
- player 0 goes in: gone from the three screens outside, and alone inside;
- player 1 follows: the two inside see each other in place, and the two
  outside see neither;
- player 0 walks about inside, and player 1 sees it;
- player 0 goes back out: seen again outside at the door, and gone from
  player 1's lab.

A warp clears a game's object events, so the server forgets what that game
was showing when its map changes, and every map is quieted as it loads.

**Talking to another player.** Player 2 turns to face player 3's figure and
presses A. The server answers with a script that the game's own engine
runs, started with `ScriptContext_SetupScript` (lockall, message,
waitmessage, waitbuttonpress, closemessage, releaseall, end). Its text is
player 3's own name, from their save, and a line. Checked:
- the message box says it (`gStringVar4`), and player 2's controls are
  locked;
- player 3 keeps walking meanwhile: talking holds up only the one reading;
- A closes the box, the script is done, the controls come back, and
  player 2 walks again.

A figure has no script of its own (it is not from a map template), so
pressing A at one does nothing in the game itself. In the test, the server
stand-in sees the A press and that player 2 is facing the figure. On the
board, the agent checks the same two things: the press, and a figure on
the tile ahead.

**A battle between two players, in progress** (`overworld_sync.py
--battle`). The session's place is the overworld: two players in Littleroot
start a link battle where they stand, with no trip to the Cable Club. Each
is given a starter with `ScriptGiveMon`. Their games link on a cable between
just the two of them (`m_bus_plug`), with the game's own `OpenLink` and
link-player exchange (both pass), then `StartSendingKeysToLink`, as the
Club does. Then the Club's own `Task_StartWiredCableClubBattle` is meant to
start the battle. It does not yet: once keys go over the cable, the
game's `HandleLinkConnection` skips every callback in a frame where nothing
arrived, and in the harness that is every frame. Next: find whether the
modelled cable falls short of the key-exchange timing, or the Club does
more set-up before this point. Once it runs, the test checks:
- each game fights the other's real Pokémon;
- both games see the same HP, change by change;
- one wins and one loses;
- both come back to the same overworld spot.

**Open question: should other players be solid?** As object events they
are, so a player standing on a doormat blocks the door for everyone, and a
crowd can wall a corridor off. The test's players step off the mat. An
MMO usually lets players pass through each other. Here that means a
figure the player's own movement ignores: one of the game's collision
checks (`GetCollisionAtCoords` and the object-event collision under it),
patched in RAM by the loader to skip figures.

What it took:
- **Not `gMain.callback2`.** The overworld's input (`CB1_Overworld`) only
  runs while callback2 *is* `CB2_Overworld`, so a hook there freezes the
  player. A Task is the right place.
- **The intro's step-on triggers.** Littleroot's coord events move a
  new-game player. They are dropped in the RAM header's copy of the events
  as the warp lands; the doors stay, for buildings.
- **The EWRAM past pokeemerald's data is 4 KB** (`0x0203F000`–`0x0203FFFF`).
  Past it is a mirror of `0x02000000`, the game's heap, which overwrites
  anything put there.

Next on mGBA:
- **Talking, trading and battling** in the room. Each starts a link session
  over the same adapters.
- **More than two players.** The firmware tracks up to 4 remote peers, and a
  room holds 5.
- **A custom room** loaded through the header window and the IRQ-table hook.
- **The ROM-patch route,** for a modded copy.

## Through the emulated adapter

The board now does its own part. Before, the test script sat between the
games, reading one game's RAM and writing the others' agent mailboxes. Now
the in-game agent talks to the board over link words, and the board talks
to the server:

```
Emerald (agent)  --link words-->  board: Link: NovaBoot  --mmo_net-->  server
                 <--commands----  src/mmo_relay.c          <--players--
```

- **Link words** (`src/mmo_link.h`): frames of 16-bit words, one word each
  way per link transfer, as Multi-Play moves them, with a checksum. 0xFFFF
  is never sent, since on the bus it means an empty slot.
  - The agent sends `MMO_STATE`: its player, plus how many commands it has
    received and how many mailbox slots are free.
  - The board sends the commands the agent already runs: `MMO_SPAWN` (for
    one map), `MMO_MOVE` and `MMO_REMOVE`.
- **The relay** (`src/mmo_relay.c`) is the server stand-in's logic, moved
  to the board, as section 3 says. It spawns a figure when a player arrives
  on this map, walks it a step at a time, turns it in place, places it
  again after a stall or a warp, and removes it when the player leaves.
  - Credits: it sends no more commands than the agent has room for.
  - On this board's own change of map, it starts the figures again.
  - A queue that overflows marks the players who lost a step, and they are
    placed again when there is room.
- **Board to server** (`src/mmo_net.h`): SLIP frames with a CRC.
  - Board to server: `MN_HELLO`, and `MN_STATE` on every change.
  - Server to board: `MN_WELCOME` (a player id), `MN_PLAYER` for the
    players on this board's map, and `MN_GONE`.
  - Interest is the map; chunks come when maps get crowded.
- **The mode:** Link: NovaBoot (a working name) in the Link chooser. It is offered only
  where the board has a way to the server (`HW_CAP_SERVER`). Today that is
  the hybrid emulator image's UART2 tunnel, which Diagnostics and the
  mode's own page mark as a stand-in for the board's network.

Tested:
- `test_mmo` (host), against a model agent with the real agent's contract:
  - the framing at both layers;
  - arrivals, walks, turns, jumps, departures and changes of map;
  - credits under pressure (twelve fast steps on a fast link);
  - fifteen players walking at once through a link of one word a frame.
  - Each fails when what it covers is broken.
- `qemu/test_mmo_adapter.py`: two touch carriers, each the hybrid image
  with its own firmware and drivers on qemu_esp32, each in Link: NovaBoot
  (reached by touch). Each has a model GBA on its link, through one server
  (`qemu/mmo_server.py`):
  - each sees the other where they stand, facing their way;
  - a walk is followed at most a step behind and ends on the walker's
    tile;
  - going indoors removes the figure, and coming back out shows it again;
  - no mailbox overflow, no bad frame at either end, no word resent.
  - With the server's departures removed, it fails.
- `test/mgba/novaboot_adapter.py`: the same with Emeralds. The agent
  (`test/mgba/mmo_agent.py`) is a game Task speaking the link words, and
  the players walk overworld_sync.py's routes. It needs the ROM.

Stand-ins, said as such:
- The test script moves the agent's words between its EWRAM rings and the
  board's bridge, standing in for the GBA's serial hardware. The agent's own
  SIO driver, sharing the port with the game's link code, is step 1 below.
- The server tunnel is a UART. A board's own network, WiFi as Link: Online
  has, is to come, and until then no real board offers the mode.

Found on the way: each link transfer through QEMU's TCP serial sockets
took 25-50 ms. Nagle's algorithm met delayed ACKs, and the UART transport
waited out a 20 ms read for every packet. With `nodelay` and reads that
take what has arrived, a transfer takes about 4 ms (`qemu/mmo_adapter.py`,
`port/esp32s3/transport_uart.c`).

## Two ways to carry other players

**Object events, driven by the agent** (tested: `overworld_sync.py`). The
agent spawns each nearby player as an object event and plays their steps
as held movements. Each step is a small packet, and nothing in the game's
own link code is involved, so it works on any map, at any distance, with
any number of players the object table can hold.

**The game's own link protocol, changed in RAM by the loader.** Emerald
already moves linked players in a shared room: in the Cable Club rooms the
games exchange their keys every frame over the link (`gLinkPlayerObjectEvents`,
the link field's key exchange), and every game moves every player's
avatar from the same keys. The loader can patch that code where it runs,
in RAM:
- let it run on overworld maps, not only in the Cable Club rooms;
- carry steps instead of raw keys, so a slow player does not hold up the
  others;
- leave the transport as it is: the board relays it as Link: Online does.

That reuses the game's own drawing, collision and facing for linked
players, and the link battle and trade paths that start from them. Both
routes draw with the same object events, so the step stream, the server
and the checks in `overworld_sync.py` carry over.

**Custom worlds need chunk preloading either way.** A map that is not in
the ROM has no layout for the game to load. The board sends its metatiles
and collision ahead of the player, chunk by chunk (Netcode, above), and
the loader writes them into the map buffer before the player can see
them.

## How many players in sight: the scale test

Once the player limit is raised and the link protocol changed, the question
is how many players can be within each other's draw/sync distance before
something gives. Three places can give first, and each needs its own bench:

| where | what runs out | how it shows | tested on |
|---|---|---|---|
| the GBA | object events (Emerald has 16, the player holds one), sprite palettes (16 in hardware), sprite tile VRAM, the agent's time in VBlank | a player not drawn, wrong colours, a dropped frame | mGBA: `overworld_sync.py` with N figures in one chunk |
| the cable | the steps a frame of the link can carry to the GBA | steps arriving late, then the catch-up at run speed | mGBA and the board, together |
| the board | core time (PHY on core 1, network and relay on core 0), queue depth, heap per player, WiFi | queues filling, drops, late steps, a missed link frame | the board's own code under realistic timing: **qemu_esp32** |

The board's side is why the test runs on qemu_esp32
(https://github.com/HelaFaye/qemu_esp32), which is Espressif's QEMU with
the patch series in `patches/espressif-qemu`. That patch set is still in
progress. `qemu/accurate.sh` pins the commit last checked and builds it
there.

**Checked on it so far** (`qemu/accurate.sh boot gate carrier-touch`, and
`devkit`): the hardware image runs, not the emulator image (`GBAGATE_QEMU=0`),
with the board's parts on its pins. On the touch carrier these are the
ILI9341, the XPT2046, an SD card and the Battle Chip board's expanders; on
the devkit, the ST7735.
- The panel drivers draw a picture that is read back from the glass and
  checked. The touch is pressed through the emulated controller.
- The SD card mounts, and the link PHYs start on core 1 on real GPIO.
- USB-OTG is still not emulated on the S3.

qemu/README.md lists the bugs this has found.

**Still needed before the test can run.** First, from the emulator (watch
the fork for these):
- A GBA on the link pins: a device on SC, SD, SI and SO, wired with
  `esp32s3.pin-link`, that speaks pin-level to an mGBA core, or to the sim's
  GBA model, over a socket.
- A network for the board on the S3 machine. On the S3 the fork has
  `open_eth`, which ESP-IDF drives as `CONFIG_ETH_USE_OPENETH`. Its master
  branch has a WiFi model, but only for the ESP32.

Then from us:
- An openeth netif in place of WiFi for the QEMU run.
- The step protocol on the board (Order of work, step 4).
- A crowd generator: N scripted players walking one chunk through the server.

**Timing caveat.** QEMU is not cycle-accurate. Runs use `-icount` so the
numbers repeat. Core load is compared between N and N+1, not read as an
absolute. One bench run on a real board calibrates it.

**The run.** N = 1, 2, 4, 8, … players in one chunk, each on a scripted
route. For each N, measure:
- on every GBA: steps shown late, and players not drawn;
- on the board: queue depth and drops, core load (FreeRTOS run-time stats),
  heap low-water, and missed link frames.

The limit is the first N where any of these crosses its line. The table
above says where to look first.

## Custom maps: authoring, format, loading

A server's overworld is its own maps, drawn in porymap
(HelaFaye/porymap) and played on retail cartridges. This is the design; none
of it is built. The facts it rests on are pokeemerald's (`src/fieldmap.c`,
`src/overworld.c`), and step 1 below proves them on the emulator before
anything else is built on them.

### What the game allows

- **The map buffer.** The overworld reads blocks from one RAM buffer,
  `sBackupMapData` (`0x02032318`, 20 KB): the map with a 7-block margin, at
  most (width + 15) x (height + 14) = 10240 blocks. So a map can be up to
  about 86 x 86, or any shape inside that bound.
- **When it is filled.** `InitMap` fills it from `gMapHeader.mapLayout->map`
  on a warp (`LoadMapFromWarp`), across a map border
  (`LoadMapFromCameraTransition`), and on loading a save
  (`InitMapFromSavedGame`, which then restores the 15 x 14 view the save
  kept). Returning from a battle, a menu or the bag does not refill it
  (`ReturnToFieldLocal` resumes the map as it was). So a custom map written
  into the buffer once, right after the warp, lasts until the next warp.
- **The header is RAM.** `gMapHeader` (`0x02037318`) is a copy the game makes
  from the ROM on each warp, with the layout pointer, music, weather, map
  type, the running, cycling and escape flags, and the events. It can be
  pointed at a layout in RAM for as long as the player is on that map.
- **A block** is two bytes: metatile id (bits 0-9), collision (10-11),
  elevation (12-15), exactly porymap's `map.bin`.
- **Graphics** come from tilesets already in the cartridge: 75 named
  `gTileset_*` symbols in Emerald (`build/ref/pokeemerald.sym`). A map uses
  one primary and one secondary tileset, as every vanilla map does. New tiles
  or palettes are out of scope (they need VRAM and palette work the game does
  not do for us).

### Loading: a host map, then a swap

1. The server puts the player on a custom map by warping them, through the
   agent, to its **host map**: a vanilla map the pack names (a map type,
   music and region-map section to start from). The game does the warp its
   own way: fade, load, object events.
2. Before the screen fades in, the agent swaps the map in, with the game's
   own functions: `gMapHeader.mapLayout` to a 24-byte `MapLayout` in free
   RAM (the pack's width, height, border and tilesets), the header fields
   the pack overrides, the host's connections cleared (a custom map's edges
   are the server's, as its warps are), `gBackupMapLayout`'s size, the
   blocks written into the
   buffer, then the tilesets and palettes loaded (`CopyMapTilesetsToVram`,
   `LoadMapTilesetPalettes`) and the view redrawn (`DrawWholeMapView`).
3. The blocks are streamed from the board at that moment, not kept on the
   GBA: free EWRAM is about 12 KB in all, which the agent shares, and a
   64 x 64 map is 8 KB. At the agent's Normal-mode clock (115200 Hz,
   `doc/AGENT.md`) that is about 14 KB/s before handshakes, so under a
   second, about a fade's length. The Multi-Play words the step protocol
   uses carry about 450 B/s (gen3-poke-multiplayer measured 226 words/s),
   18 s for the same map, so the swap is a Normal-mode transfer. The board
   keeps every map it was sent, so a warp never waits on the network.
4. After a battle or a menu nothing needs doing (the buffer is untouched).
   After loading a save, the agent repeats step 2 for the map the save is on.
5. A save made on a custom map records the host map and a 15 x 14 view of
   custom blocks. Booted later without NovaBoot, the player stands on the
   host map with a patch of custom tiles around them until the next warp:
   untidy, never corrupt. The server can avoid even that by choosing host
   maps whose tilesets match the custom map's.

Step 2's timing is the first experiment: the swap must land after
`LoadMapFromWarp` and before the first frame is shown. If the agent's hook
cannot reach that window, the fallback is to swap during the fade-in with
the screen held black for those frames.

### The map pack

What a server publishes, made from the porymap project by a converter
(`tools/mappack.py`, to come) that reads the files porymap saves (each map's
`map.json`, `data/layouts/layouts.json`, the layouts' `map.bin` and
`border.bin`). JSON with the block data in base64; one pack per server, and
every map in it named by the SHA-256 of its canonical encoding, so a board's
cache is keyed by content:

```
{"format": "novaboot-mappack", "version": 1,
 "game": "BPEE",                 # Emerald; maps name tilesets, not addresses
 "maps": {"<sha256>": {
   "name": "MyTown",             # porymap's map name
   "host": [0, 9],               # vanilla map group, num
   "width": 32, "height": 24,
   "tilesets": ["gTileset_General", "gTileset_Petalburg"],
   "border": "<base64, 4 blocks>",
   "blocks": "<base64, width x height blocks, u16 LE>",
   "header": {"music": "MUS_LITTLEROOT", "weather": "WEATHER_SUNNY",
              "map_type": "MAP_TYPE_TOWN", "allow_running": true,
              "allow_cycling": true, "allow_escaping": false, "show_map_name": true},
   "connections": [{"dir": "up", "offset": 0, "map": "<sha256>"}],
   "warps": [{"x": 10, "y": 5, "elevation": 3, "to": "<sha256>", "warp": 0}],
   "signs": [{"x": 4, "y": 7, "text": "MYTOWN"}],
   "npcs": [{"gfx": "OBJ_EVENT_GFX_BOY_1", "x": 12, "y": 9,
             "movement": "MOVEMENT_TYPE_WANDER_AROUND", "text": "..."}]}},
 "start": {"map": "<sha256>", "x": 10, "y": 8}}
```

Constants (music, weather, graphics, movement types) are porymap's names;
the board resolves them per cartridge from the symbol map, as it does
tilesets. The converter refuses a pack that would not run: an unknown
tileset or constant, a metatile id past its tilesets' count, a map past the
buffer bound, a warp or connection to a map not in the pack.

Warps, signs and NPCs are the server's, not the game's script engine's: the
agent reports the player stepping on a warp or facing a sign or NPC (it
already reports every step), and the server answers with the warp or the
text, which the agent shows with the game's own message box. Connections
work the same way at map edges. Scripts written in porymap's script files
are out of scope for version 1.

### Netcode for maps

On the reliable command channel (section 4), between the server and the
board (`src/mmo_net.h`), three messages:

| message | when | carries |
|---|---|---|
| `MN_MAP` | on joining, and on every warp the server makes | the map's hash, its host map, the player's tile |
| `MN_MAP_GET` | from the board, for a hash it has not cached | the hash |
| `MN_MAP_DATA` | the answer, in chunks | the map's canonical encoding, checked against its hash |

Between the board and the agent (`src/mmo_link.h`), the swap of step 2 as
`MMO_MAP_BEGIN` (layout, header fields), `MMO_MAP_ROWS` (blocks, a few rows
a frame) and `MMO_MAP_END`; the agent acknowledges with its next
`MMO_STATE`. A board fetches a server's whole pack when it joins, so it
has every map before the first warp; the hashes make a second visit free.

Interest is per map instance, as it already is (`qemu/mmo_server.py`): the
custom map's hash is its key, where a vanilla map's group and number are
today.

### In porymap

Authors use porymap as it is, on a pokeemerald project: pick a host-like
map type and tilesets, draw, place warps, signs and NPCs, save. The fork
adds nothing the converter needs, since porymap's saved files are the
input. What it can add, as a script in porymap's own scripting API
(`utility.registerAction`), is a check run before publishing: the map's
size against the buffer bound, its tilesets vanilla, its events of the
kinds the server handles. That keeps the fork a plain porymap that follows
upstream.

### Order

1. On the emulator: the swap of step 2 into a vanilla host map, from a
   block array the test supplies; timing, redraw, a battle and a save in it.
2. `tools/mappack.py` from a pokeemerald project as porymap saves it, with
   the canonical encoding and the checks; tested on vanilla maps, whose
   blocks it must reproduce exactly.
3. The messages above in `mmo_net` and `mmo_link`, the board's cache, the
   test server serving a pack.
4. Warps, signs and NPCs through the server.
5. The porymap check script.

## The server

`qemu/mmo_server.py` is the tests' server. The real one is proposed to grow
out of Celio-Server, which already does what a board needs from a server:
sessions keyed by a short code, clients that come and go, commands
acknowledged and resent, data batched and ordered per receiver. The MMO adds
what Celio does not have: a persistent world, map instances, interest
management, the map packs.

**Which Celio-Server.** Three lines exist:

| | head | state |
|---|---|---|
| GB-Link/Celio-Server | main 9f02d3e (June), MULTI 368327e (July) | stopped |
| HelaFaye/Celio-Server `gbagate-fixes` | 94c5dde | GB-Link's June code plus two of ours: Celio-Client's acknowledged protocol, and forgetting a client that leaves |
| Celio-Link/Celio-Server | main = production f4b92bc (17 October) | acknowledged protocol since early October, 4-player sessions, 3.0's player settings; what Celio-Client 3.0 speaks |

Both of `gbagate-fixes`' commits are already in Celio-Link's main: it speaks
the acknowledged protocol natively, and its `removeClient()` forgets a
leaving client's state (`clientState.delete`), as our second commit does. So
`gbagate-fixes` is not where the MMO server should start: Celio-Link's main
is, as a fork with the MMO in its own module beside the link sessions, so the
link play the board already relies on keeps working on the same server. Its
4-player sessions also supersede `backport/Celio-Link-Server/0002`. Fixes to
the shared core go back as pull requests; GB-Link/Celio-Server would need
Celio-Link's October work before any of ours applies there. MMO features stay
in the fork.

## Order of work

1. The agent hooked into a running game, on the emulator with a scripted
   game loop: VBlank budget, giving the serial port back.
2. The map generator from pokeemerald's symbols, and the boot check.
3. One remote player drawn and walked from a script, on the emulator.
4. The step protocol and server, two boards, then a simulated crowd. The
   protocol, the board's relay and a test server run through emulated
   adapters (above); next are the board's own network transport and the
   crowd, which is the scale test.
5. Commands: interact, trainer card, battle through the existing link relay.
6. Custom maps from existing tilesets ("Custom maps" above).
7. Packet inspection on the board (below), before anything is opened to
   players who do not know each other.

## Packet inspection: the board as the GBA's firewall

Over a cable, a GBA trusts its partner, because the partner is another
cartridge. Over the network, the partner is whatever the other end sends.
The Gen 3 games accept link data that can crash them or run code: a
malformed Pokémon, an over-long mail or a record mixing block. On an open
server, one player could reach everyone's GBA.

The board is the last hop before the GBA and the only one the player owns,
so it checks there. The server checks too, but a GBA should not depend on
a server it does not run.

- **Positive validation, per game and per activity.** Know the shape of
  every block the activity sends (`BBBB size id`, then `8888` continuations,
  as `src/mp_relay.c` sees them), reassemble it, and pass it only if it
  parses:
  - Pokémon: checksum, species, moves, item and ability in range, no Bad
    Egg flags, and strings terminated. The PKHeX port's legality checks are
    the natural source.
  - Mail: terminated text, a valid item.
  - Link player records, trainer cards, record mixing blocks: fixed sizes
    and value ranges.
- **Anything it cannot parse is dropped**, not passed: the session ends
  cleanly on that board, and its page says why.
- **The same for the Wireless Adapter stream** (Union Room, trading over
  RFU). There the blocks arrive inside librfu's frames.
- **Tests:** every captured session from the mGBA suites must pass
  unchanged, and a corpus of crafted bad blocks must all be stopped.

This comes after the 250-player tests and the audit, which will show which
activities players actually use, so those are covered first.
