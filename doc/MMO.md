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

Next on mGBA:
- **A Wireless Adapter on mGBA's serial port.** Our firmware's adapter
  emulation, paced to real time, so the real `RunUnionRoom` runs.
- **A custom room** loaded through the header window and the IRQ-table hook.
- **The ROM-patch route,** for a modded copy.

## Order of work

1. The agent hooked into a running game, on the emulator with a scripted
   game loop: VBlank budget, giving the serial port back.
2. The map generator from pokeemerald's symbols, and the boot check.
3. One remote player drawn and walked from a script, on the emulator.
4. The step protocol and server, two boards, then a simulated crowd.
5. Commands: interact, trainer card, battle through the existing link relay.
6. Custom maps from existing tilesets.
