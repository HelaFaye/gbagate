# Infrared and e-Reader: the plan

What the board should do over infrared for Game Boy Color games and their
companion toys, and as an e-Reader for GBA and GameCube games. Each feature
gets an emulation test before it gets hardware time, as the link features did.

**Status:** a plan. The only infrared code is the GBA's Cyber Drive Zoids
adapter (`src/ir_cdz.c`, `test/test_ir.c`, `port/esp32s3/ir_esp32s3.c`), and
its hardware timing has not met a scope. Every protocol fact below says where
it comes from. Facts marked *(verify)* are from memory or secondary sources:
check them against the source named before building on them.

## Contents

- [What is wanted](#what-is-wanted)
- [How each one reaches the board](#how-each-one-reaches-the-board)
- [Hardware: what the PCB needs](#hardware-what-the-pcb-needs)
- [Software: one engine, many configs](#software-one-engine-many-configs)
- [Testing](#testing)
- [Phases](#phases)
- [Open questions](#open-questions)

## What is wanted

| # | game or device | over | what the board does |
|---|---|---|---|
| 1 | Pokémon Gold, Silver, Crystal | GBC IR | **Mystery Gift**: be the other player, every day, for its daily gift. **Crystal's Trainer House**: exchange trainer data, so the house's team changes. |
| 2 | Pokémon Trading Card Game | GBC IR | **Card Pop!** (a random card from the partner's pool), and card and deck transfers |
| 3 | Pokémon Pinball | GBC IR | send and receive high scores |
| 4 | Super Mario Bros. Deluxe | GBC IR | send and receive high scores and records |
| 5 | Mickey's Speedway USA, Mickey's Racing Adventure | GBC IR | the link that unlocks Speedway's secret track |
| 6 | Ubisoft's GBC games | GBC IR | the "Ubi Key" exchange that unlocks content between them |
| 7 | Game Boy Wars 3 | GBC IR | its two-player game, over the board's own network (two boards, two GBCs, any distance) |
| 8 | Pocket Pikachu 2 GS, with Gold/Silver/Crystal | GBC-protocol IR | be the pedometer: send its watts and gifts to the game, and take the game's gift |
| 9 | Pocket Sakura, with Sakura Taisen GB | GBC-protocol IR | the same exchange, as the pedometer |
| 10 | Dancing Furby (GBC), with Furby toys | IR to the toy | relay between the game and a real Furby, or be the toy |
| 11 | Bomberman Max | TV remote IR | the game reads a TV remote's pulses: send them, so no remote is needed |
| 12 | Pokémon Colosseum (Japan) | e-Reader, GBA–GameCube link | the **Card e Room**: Double Battle e cards, and the Shadow Togepi, Mareep and Scizor cards |
| 13 | Pokémon Channel (JP, US, AU) | e-Reader, GBA–GameCube link | Smeargle's paint cards |
| 14 | Pokémon Box Ruby & Sapphire | (a note) | how Box treats a Berry that came from an e-Reader card *(verify: the exact rule, in Box's manual or its decompilation)* |

The e-Reader rows (12 to 14) also cover the e-Reader cards that Ruby and
Sapphire, and Japanese Emerald, read over the GBA link: Berries, Battle e,
the Eon Ticket. That path is the easiest to test, and comes first.

## How each one reaches the board

### Game Boy Color infrared (rows 1 to 9)

The Game Boy Color has an IR LED and a photodetector on top. A game drives
them through register **RP** (`0xFF56`): bit 0 lights the LED, bit 1 reads the
detector (0 while light falls on it), and bits 6–7 must be set to read. There
is no UART and no carrier. Each game times its own pulses in software, so each
protocol is a pattern of on and off times, measured in CPU cycles.

The board needs nothing from the GBC but light. Its emitter faces the GBC's
window and plays the other side's pulses. Its detector, a raw photodiode (not
a TSOP: see the Zoids driver's note), times the game's pulses.

Sources, per protocol:
- **Gold, Silver, Crystal** (rows 1, 8): pret's `pokecrystal` and
  `pokegold`, `engine/link/mystery_gift.asm`. The timing loops are there in
  cycles, with the data framing, the daily limits (one gift per partner per
  day, five a day *(verify)*), the Pokémon Pikachu 2 GS path, and Crystal's
  Trainer House data. The disassembly's symbols also say where a test can read
  "a gift arrived" in the game's RAM.
- **Pokémon TCG** (row 2): pret's `poketcg`, its IR and Card Pop! routines.
- **Pocket Pikachu 2 GS, Pocket Sakura, Super Mario Bros. Deluxe, Pinball,
  the Ubi Key, Game Boy Wars 3, Mickey's Speedway** (rows 3 to 9): Shonumi's
  "Edge of Emulation" articles cover the GBC IR exchanges between games and
  devices *(verify which titles each article covers)*. Where none exists, we
  record the exchange between two real units with a logic analyser on the
  photodetector, and decode it ourselves.
- **Game Boy Wars 3** (row 7): its IR link plays the game in turns, so its
  latency budget decides whether two boards can carry it over the network.
  Measure that before promising it.

### Companion devices (rows 8 to 10)

The pedometers speak the GBC's own IR protocol, so to the game the board is
just another unit. A real Furby is another matter: Furbies talk to each other
over IR *(verify the modulation: if it uses a 38 kHz carrier, the board needs a
demodulating receiver for it, below)*.

### TV remote (row 11)

A TV remote is 38 kHz on–off keying, usually NEC. A GBC's photodetector
responds to the bursts, and the game reads the envelope. To be the remote, the
board's RMT transmits the carrier; to hear one, it needs a demodulating
receiver. *(verify how Bomberman Max reads the remote, and which codes it
accepts)*

### e-Reader (rows 12 to 14)

The e-Reader is a GBA cartridge with a dot-code scanner. A scanned card is a
data block, with a small header and usually compressed. It reaches its game in
one of two ways:
- **GBA to GBA** (Ruby, Sapphire, Japanese Emerald): the e-Reader GBA sends
  the card's data to the game's GBA over the link cable. The board is already
  on that cable: it can be the e-Reader GBA, sending card data from the
  microSD card.
- **GBA to GameCube** (Colosseum, Channel): the e-Reader GBA is plugged into
  the GameCube with the GBA–GameCube cable, and the GameCube talks JOY Bus to
  it. For the board to be the e-Reader there, it needs a GameCube controller
  connector, and must answer JOY Bus as a GBA would. JOY Bus is one open-drain
  line at 4 µs a bit, which the S3's RMT can do on a free channel. The
  GameCube games' e-Reader exchanges need decoding from Dolphin traces *(no
  public description that we know of)*.

**Card data** is the user's own: scans of cards they own, as the raw dot-code
files the e-Reader community's tools read and write. None ship here, as no ROM
does.

## Hardware: what the PCB needs

This feeds the PCB plan (`doc/RF-HARDWARE-MAP.md`, and the RMT notes).
- **Emitter:** one IR LED (940 nm), with a transistor driver, as
  `ir_esp32s3.c` says. RMT drives it:
  - plain pulses for the GBC and Zoids protocols;
  - a 38 kHz carrier (the RMT's carrier generator) for the remote, and for a
    Furby if it is modulated.
- **Two receivers:**
  - a raw photodiode or phototransistor into a comparator, for the GBC
    protocols (no carrier);
  - a 38 kHz demodulating receiver (TSOP-class), for remotes and modulated
    toys.

  One part cannot do both: the TSOP blanks unmodulated pulses, and the
  photodiode is too slow and too short-ranged for a 38 kHz envelope at a
  distance.
- **Placement:** the emitter and photodiode face the GBC's IR window, on the
  board's top edge, with the GBC lying beside it. Range is a few centimetres;
  ambient light matters, so hood the photodiode.
- **GameCube JOY Bus:** an optional controller-port connector (5 V, 3.3 V,
  data, ground). Data is open-drain at 3.3 V, so one GPIO with RMT, through a
  level-safe buffer.
- **RMT budget on the S3** (4 TX and 4 RX channels):
  - GBA link: 2 TX, 2 RX;
  - IR: 1 TX, plus 1 RX (both receivers, muxed);
  - JOY Bus: 1 TX, 1 RX.

  That is 4 and 4: all of them, so the modes must share at run time. The mode
  switch already tears the link mode down and builds the next one
  (`doc/RADIO-MODE-SWITCHING.md`). The RMT channels must be freed in that same
  step, or the next mode finds none.

## Software: one engine, many configs

The Zoids code already has the right shape: a classifier of gaps, a decoder,
and an encoder, all off the hardware. Generalise it:

- `src/ir_pulse.[ch]`, an **IR pulse engine**:
  - a received train is a list of (on, off) durations from RMT RX;
  - a sent train is a list for RMT TX;
  - the engine matches them against a protocol description.
- `ir/*.cfg`, **a config per game or device**, read from the microSD card:
  - the timing classes, with tolerances;
  - framing: header, bit cells, checksums;
  - the message sequence;
  - what to answer.

  Gold, Silver and Crystal's Mystery Gift, Card Pop!, the scores and the
  pedometers each become a config, not a module. Data that belongs to a
  player (a gift, a team, a score table) is a file beside the config, in the
  card's `ir/` directory.
- **Applets** (`doc/APPLETS.md`), one per user-facing task. Each lists only
  when its config is on the card, as the Save Tool does:
  - **Mystery Gift** (GSC, Pikachu 2 GS), and **Trainer House**;
  - **Card Pop!**;
  - **Scores** (Pinball, SMB Deluxe);
  - **Unlocks** (Mickey's Speedway, Ubi Key);
  - **Remote**;
  - **e-Reader** (cards from the card's `ereader/` directory).
- **Network relays** for the two-player IR games (Game Boy Wars 3, and any
  game whose exchange can be relayed): the board forwards trains to a peer
  board through the online service (`doc/ONLINE.md`), when the latency allows.

## Testing

Every config gets three tiers of test, as the link modes have:

1. **Host tests** (`make`):
   - the engine against synthetic trains, with jitter, noise, ambient light and
     clipped edges;
   - each config against a recording of the real exchange, once we have one,
     kept as timings only (no game data).
2. **Emulation, the game itself:**
   - **GBC IR:** SameBoy's core emulates the GBC's IR port. Its API lets a
     host see the LED and drive the detector, and pret's symbols find the
     game's state. A harness runs the user's ROM and connects the port to
     the engine, as the mGBA shim connects the link port. The pass condition is
     the game's own, in RAM: the gift arrived, the Trainer House changed, the
     card was added, the score listed.
   - **e-Reader over the GBA link:** mGBA, with Ruby, Sapphire or Emerald and
     the existing two-core link (`test/mgba/mgba_multi.c`). The board's
     e-Reader stand-in sends a card, and the test reads the game's Berry or
     trainer data.
   - **e-Reader to GameCube:** Dolphin, with its GBA link device. This is the
     heaviest harness, so it comes last.
3. **The board in QEMU:**
   - `qemu_esp32` models RMT TX and RX (its pending RMT work); its model of
     the IR receiver lights the pin from the harness's train;
   - the firmware's applet runs against it;
   - the CI job runs it with the others.

Real hardware last: a scope on the emitter, and one real GBC per protocol.

## Phases

| phase | what | first test |
|---|---|---|
| IR-0 | the pulse engine and config format; Zoids moved onto it, with `test_ir` still passing | host |
| IR-1 | Gold, Silver, Crystal Mystery Gift, the board as the partner; Pikachu 2 GS's side | host, with SameBoy and Crystal |
| IR-2 | Crystal's Trainer House | SameBoy |
| IR-3 | Pokémon TCG: Card Pop!, card transfers | SameBoy, with pret's `poketcg` symbols |
| IR-4 | scores and unlocks: Pinball, SMB Deluxe, Mickey's Speedway, Ubi Key; Pocket Sakura | SameBoy, with recordings where no source exists |
| IR-5 | the TV remote, and Furby once its modulation is known | host; then a real GBC |
| IR-6 | Game Boy Wars 3 between two boards, if its latency allows | two SameBoys, a relay in between |
| ER-0 | the e-Reader card format: read and check the user's dot-code files | host |
| ER-1 | e-Reader over the GBA link: a Berry or a Battle e card into Ruby, Sapphire or Emerald | mGBA, two cores |
| ER-2 | JOY Bus, the board as a GBA to the GameCube | Dolphin traces, then the GameCube itself |
| ER-3 | Colosseum's Card e Room, and Pokémon Channel's paint cards | Dolphin |
| HW | the emitter, two receivers and the JOY Bus port on the next PCB; RMT sharing in the mode switch | QEMU's RMT model, then bench |

IR-0 and ER-0/ER-1 need nothing new in hardware or emulation, and go first.

## Open questions

- Which protocols have no public description: we will need two real units
  and a logic analyser for those.
- Furby's IR modulation, and Bomberman Max's remote codes.
- Pokémon Box Ruby & Sapphire: the rule for e-Reader Berries.
- JOY Bus timing margins with the RMT, and whether one GPIO is enough (with
  the RMT's TX and RX channels both on it), or two with a buffer.
- Game Boy Wars 3: how long its IR exchange can wait before it gives up.
