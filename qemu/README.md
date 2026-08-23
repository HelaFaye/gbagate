# Running the firmware under QEMU

```
./qemu/setup.sh              check ESP-IDF, install Espressif's QEMU
./qemu/build.sh gate         build an image and assemble a flash image
./qemu/run.sh gate           boot it, console here, link_proto on :5556
python3 qemu/broker.py --loop        (in another terminal)
```

`make qemu-proto` runs the one check here that needs neither ESP-IDF nor an
emulator, and it is the one worth running first.

## What this proves, and what it does not

**Can:** boot the firmware and run `app_main`; variant resolution; task
creation and core pinning; NVS and the FAT partition on emulated flash; the
portable logic on real Xtensa (alignment, stack depth, endianness — things an
x86 host test cannot catch); `link_bridge` end to end against `broker.py`.

**Cannot:** the GBA link PHY — no peripheral is wired to anything, and
bit-banged GPIO timing is exactly what QEMU does not model. USB-OTG is not
emulated on the S3, so no CDC and no MSC. No WiFi, no ESP-NOW. No SPI device
on the other end of the ST7735 bus.

So QEMU exercises the firmware's *logic* in place. **It does not remove a
single item from the bench list in `PROJECT_STATUS.md`.** Both are worth doing;
neither substitutes for the other.

## The UART transport

QEMU does not emulate the S3's USB-OTG, so CDC never enumerates and cannot be
the channel to a host. UART, however, QEMU maps straight onto a socket.
`port/esp32s3/transport_uart.c` is the same `link_bridge` behind a UART instead
of CDC — the protocol under test is the real one, not a QEMU-shaped substitute.

UART0 carries the ESP-IDF console. UART1 carries `link_proto` on TCP `:5556`.
They are separate on purpose: sharing one would interleave log lines with SLIP
frames and corrupt both.

## The broker

```
[ firmware in QEMU ] --UART1--> :5556 <--> broker <--> :7000 [ mGBA ]
```

| mode | what it does |
|---|---|
| `--loop` | answers the firmware itself; no game needed |
| `--mgba PORT` | listens for `qemu/mgba_link.lua` |
| `--peer HOST:PORT` | relays to another broker, so two emulated devices link |

**Start with `--loop`.** If the firmware will not complete a HELLO exchange
against a broker that always answers correctly, nothing further is worth
trying.

`qemu/lp.py` implements the wire format in Python and is **cross-checked
against `src/link_proto.c` over 2004 packets**, including payloads full of SLIP
delimiters. A broker that framed packets even slightly differently would fail
as "the firmware ignores everything" — no error, nothing to bisect.

## Testing the gate against MMBN6

Three steps, in order. Each one is cheap and each rules out a whole class of
problem, so do not skip to the last.

**1. Prove the broker before the firmware.** Needs no ESP-IDF and no emulator.

```
make qemu-proto
```

This checks that `lp.py` frames packets exactly as `link_proto.c` does, and
that the broker's `--gate` handshake actually drives the real `chipgate.c` to a
linked state on all four gate identities. If either fails, nothing downstream
can work.

**2. Prove the firmware with no game.**

```
./qemu/build.sh gate
./qemu/run.sh gate                        # terminal 1
python3 qemu/broker.py --gate bn6 -v      # terminal 2
```

The broker plays the game's part: sync word, acknowledge, idle frames. Expect
to see the gate announce `FF00`, then the 9-slot rotation. If the firmware will
not link against a script that gets the handshake right every time, an emulator
will not help.

**3. Add MMBN6.**

```
python3 qemu/broker.py --mgba 7000                       # first: it listens
mgba-headless --script qemu/mgba_link.lua mmbn6.gba
```

In the game, go to the Comm menu and select the Chip Gate option. The firmware
supplies the gate side.

### The honest caveat about step 3

`mgba_link.lua` drives the game's SIO registers from Lua at frame granularity,
because mGBA has no external SIO driver. Whether a retail game accepts that
depends on how closely it watches `SIOCNT`'s start and busy bits, which the
script clears by hand -- and that is the least certain part of this whole
setup.

If the game will not link but steps 1 and 2 pass, the problem is the Lua
register poking, not the firmware. The robust fix is to add a `GBASIODriver`
to mGBA alongside `GBASIODolphin`, which is already a socket-backed driver, and
point it at the broker. That is a patch to mGBA rather than a script, and it is
what upstream issue #2379 asks for.

## Connecting mGBA

`qemu/mgba_link.lua` drives the game's SIO registers from Lua at frame
granularity, because mGBA has no external SIO driver. That exercises the
firmware's protocol handling — handshake, sequencing, pacing, chip words — and
it is explicitly not a timing test.

```
python3 qemu/broker.py --mgba 7000        # first: it listens
mgba-headless --script qemu/mgba_link.lua game.gba
```

`tools/build-mgba-termux.sh` builds the `mgba-headless` this needs.

## Notes on the earlier scripts

The QEMU scripts previously in `tools/` have been removed; they could not have
worked:

- `qemu-system-esp32s3` is not a binary that exists. Espressif's QEMU is
  `qemu-system-xtensa` with `-machine esp32s3`.
- `-kernel firmware.bin` is a Linux boot option. An ESP32 machine boots from an
  emulated flash image (`-drive if=mtd`) containing bootloader, partition table
  and app at their proper offsets — which is what `build.sh` assembles with
  `esptool.py merge_bin`.
- `-append "console=ttyS0,115200"` is a Linux kernel command line.
- `-gdbserver` is not a QEMU option; it is `-gdb tcp::3333` (and `-S` to halt).
- The build paths were absolute to one machine.
- Piping QEMU's serial output into mGBA with `socat` does nothing: mGBA has no
  serial input. That is the gap `broker.py` and `mgba_link.lua` fill.

The QEMU sdkconfig also enabled TinyUSB CDC, which cannot work under emulation
for the reason above — `build.sh` disables it and uses UART instead.
