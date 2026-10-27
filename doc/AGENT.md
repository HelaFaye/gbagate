# The memory agent

A small multiboot program, `rom/agent.c`, lets the board read and write a
GBA's memory over the link cable. Send it to a GBA waiting at its boot screen
(no cartridge, as for any multiboot program). It stays on the link as a
Normal-mode 32-bit SIO slave, and the board, as master, can then:

| command | what |
|---|---|
| `PING` | is it there; its version, and the first free EWRAM byte |
| `READ n` | n words from an address |
| `WRITE n` | n words to an address, with a checksum |
| `FILL n` | n copies of one word |
| `POKE16` | one halfword store, for I/O registers and palette RAM |
| `CALL` | run code at an address (bit 0 set: Thumb), and come back |

This is the first step of the multiboot preloader: patching a game's memory
from the board, and later feeding it data while it runs.

## On the board: the GBA Memory applet

**Applets → GBA Memory**. Turn on a GBA with no cartridge, link it, and
confirm: the board sends the agent and connects. Then:

- **Ping**: is the agent still answering?
- **Colour**: writes the backdrop colour (palette entry 0). The GBA's screen
  changes colour, which is the plainest proof that a write landed. The agent
  shows blue while it waits for the board and green once the board has
  pinged it.
- **Test 4 KB**: writes a fresh pattern to free EWRAM, reads it back, and
  compares.

The agent is built into the firmware with a **blank logo**: Nintendo's logo
bytes cannot ship. Before sending, the board copies the logo from the first
valid program on its USB drive (`rom_borrow_logo`). So the drive needs one
program you own on it, as Send Program does. If it has none, the applet says
*put a program on the drive*.

For code, `src/mbagent.h` is the client:

```c
mba_client_t *c = env->agent();          /* from an applet, once started */
uint32_t words[64];
mba_read(c, 0x02000000 + 0x10000, words, 64);
mba_poke16(c, 0x05000000, 0x001F);       /* the backdrop: red */
```

## The protocol

`rom/agent_proto.h` holds the whole protocol as one freestanding core,
compiled into the GBA program and into the host tests alike.

Normal mode is full duplex. In each transfer the board sends a word and gets
back the word the agent staged after the previous one, so every reply
arrives one transfer late. The board clocks a reply out by sending `NOP` (0).

- **Header:** `0xA6 << 24 | cmd << 16 | count`, acknowledged with `~header`,
  so the board knows the agent is in step.
- **WRITE:** echoes the address, then the count of words taken. It ends with
  an FNV-1a checksum the agent checks before reporting `OKAY`, or `BSUM`
  if the checksum fails.
- **READ:** ends with the agent's checksum of what it sent.
- **Status words** spell their meaning in ASCII, so a logic-analyser trace
  reads: `OKAY`, `BSUM`, `PROT`, `ALGN`, `BCMD`, `IDLE`.
- **Protection:** the agent refuses (`PROT`) any write or call into itself:
  its EWRAM image, its IWRAM code and its stack. A write over the running
  agent would end the session in a way the board could not tell from a
  dropped cable.
- **Losing its place:** if the board loses its place, it waits 25 ms. The
  agent drops any half-finished command after 20 ms without a transfer. The
  board then clocks until it sees `IDLE` twice, and retries, up to 3 times.
  A CALL is never retried, so code cannot run twice.

## Timing

The board's SPI leaves 36 µs between words (`mb_spi_xfer`). The agent must
read the word, run the core and restage its reply within that gap.

Its serving loop runs from IWRAM. From EWRAM (a 16-bit bus with two wait
states, 6 cycles an ARM instruction) the worst word took 30 µs of the 36,
too close. From IWRAM it takes 11.7 µs. A FILL takes about 0.8 µs a word,
and the board waits twice that before it asks for the status.

## How it is tested

| what | where |
|---|---|
| The client against the agent's core over a simulated link with the one-word lag. Every single transfer of a write is damaged going out, damaged coming back, or lost: memory is always right and the result is always true. Also a restart mid-command, protection, alignment, and a hopeless line failing honestly. | `test/test_mbagent.c` |
| **The compiled `rom/agent.mb` on an emulated ARM7** (unicorn), from the multiboot entry point, with SIO and timer 3 modelled. The board clocks on schedule, as SPI does, and the real client drives it through ctypes. It checks ping, a 600-word write and read, poke, fill, protection, a CALL into IWRAM, recovery from a broken command, and the per-word timing above. CI runs it on the committed image and on one built with GCC. | `test/agent_emu/agent_emu.py` |
| Logo borrowing on the embedded image: the donor's logo, the complement recomputed, the program untouched; blank or short donors refused | `test/test_catalog.c` |
| The applet: refusing without the services, *sending* shown before the slow part, a failure's reason, ping, the backdrop write landing, the 4 KB test, every line fitting the panel, the link port given back on exit | `test/test_applets.c` |

## Building the agent

The image is committed, `rom/agent.mb` and `src/gbagent_image.c`, because the
firmware's ESP-IDF build has no ARM toolchain. After changing `rom/agent.c` or
`rom/agent_proto.h`:

```
make agent-image                    # arm-none-eabi-gcc
make agent-image TOOLCHAIN=clang    # or clang + lld
```

Lint fails if the embedded array no longer matches the agent's sources.

**Not yet run on a console.** Everything above runs the agent's real bytes,
but on an emulated CPU with modelled SIO.
