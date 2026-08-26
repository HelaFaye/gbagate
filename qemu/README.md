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
UART2 carries the framebuffer on TCP `:5557` (see below). They are separate on
purpose: sharing one would interleave log lines with SLIP frames and corrupt
both. QEMU connects each UART peripheral to a `-serial` chardev in order, so the
three `-serial` options in `run.sh` map to UART0, UART1 and UART2.

## Seeing the screen under QEMU

QEMU cannot drive the real ST7735 — there is no SPI device on the other end of
the bus. So under emulation `port/esp32s3/st7735_qemu.c` stands in for the panel
the same way `transport_uart.c` stands in for the GBA PHY: it keeps the very
framebuffer the firmware renders into and, instead of clocking it out over SPI,
streams it out UART2, which `run.sh` maps onto TCP `:5557`.

```
./qemu/build.sh gate
./qemu/run.sh gate                       # UART2 -> :5557
python3 qemu/fbgrab.py --one -o shot.ppm # in another terminal
```

`qemu/fbgrab.py` reads that stream and writes a **P6 PPM in exactly the format
`test/test_render.c` saves** (RGB565 → RGB888 with the same rounding), so a
screen captured from live firmware under emulation and one captured from the
host render test are directly comparable. `make qemu-display` is the shortcut.

The swap is confined to the QEMU build: the real driver is unchanged and is
still what the hardware images link against.

### When the screen stays blank

A blank `viewer.py` with **no error line in its log** is the awkward case: it
means the connect to `:5557` succeeded and zero *complete* frames parsed, and
the viewer only paints once a whole frame is in hand. A socket cannot tell you
why, because a chardev QEMU created but never attached to a UART peripheral
still opens its listening socket and still accepts a client — so the connect
succeeds and nothing ever arrives, silently.

```
bash qemu/uart2check.sh          # boots for 6s with every UART going to a file
```

No ports, no clients, no viewer, so nothing downstream of QEMU can be blamed for
an empty file. UART1 is the built-in control: it is known to work, so if
`uart1-link.bin` fills and `uart2-fb.bin` stays empty in the same run, this
QEMU does not wire UART2 and no firmware or viewer change will help. The script
also checks the console for the tap's own log line first, so "QEMU dropped it"
is never confused with `st7735_init()` having taken its degraded path.

If UART2 *does* forward, the fault is the socket path or the viewer, and

```
./qemu/lab.sh --no-viewer        # leaves :5557 free
python3 qemu/rawtap.py 5557 10   # in another terminal
```

separates whole frames from a truncated stream. `--no-viewer` exists because a
QEMU tcp chardev serves one client at a time: the viewer and a tap cannot both
hold `:5557`.

Note this is not only about the screen. `input_qemu.c` reads the same UART2, so
a UART2 that does not forward also means the encoder is dead under emulation and
no chip can be slotted by hand.

### If this QEMU only wires two UARTs

That is a property of the emulator's `esp32s3` machine, so it cannot be fixed in
the firmware or in `viewer.py`. Either install a QEMU that models three UARTs
(`idf_tools.py install qemu-xtensa`), or move the tap onto a UART that does
exist. `--aux-uart 0` does the latter: the framebuffer shares **UART0** with the
ESP-IDF console, and the link keeps UART1 to itself, so nothing about the link
testing changes.

```
./qemu/build.sh gate --aux-uart 0     # the image and the -serial layout
./qemu/lab.sh --aux-uart 0            # must agree, so pass it to both
```

The image and the runner must be built and started with the *same* value. They
are compiled in (`QEMU_AUX_UART`) and laid out on the command line respectively,
and a mismatch reproduces exactly the blank screen above — so the value is
printed at boot (`framebuffer tap on UARTn`) and echoed by both scripts.

Sharing the console works because the stream is self-delimiting: `viewer.py`
resynchronises on the `FBUF` magic and validates the geometry, so log lines are
skipped rather than fatal. `st7735_qemu.c` also writes each frame in a single
`uart_write_bytes()` call, so a log line can only land *between* frames, never
inside one. The skipped bytes are the console, and are not thrown away:

```
python3 qemu/viewer.py --console-log build/qemu-gate/console.log
```

`lab.sh --aux-uart 0` passes that automatically. The viewer's status line shows
`console=NB` so the sharing is visible rather than silent; on a *dedicated* UART
that counter should stay at zero, and a number that keeps climbing there means
the stream is being corrupted rather than merely shared.

`--aux-uart 1` also exists — it gives the framebuffer UART1 for a completely
clean stream — but it takes the link's UART, so there is no link at all and
`lab.sh` skips mGBA and the broker. It is only useful for looking at the screen.

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

## The lab: everything at once

```
./qemu/lab.sh                       firmware + screen + tester ROM, automated
./qemu/lab.sh --game .reference/MBN6CG.gba    firmware + screen + a real game
./qemu/lab.sh --no-mgba             just the firmware and its screen
./qemu/lab.sh --sim                 gate_sim instead of QEMU (no ESP-IDF needed)
```

One command starts QEMU, the broker, the viewer and mGBA, and reaps all of them
on exit including on Ctrl-C. It refuses to start if a port is already bound,
because a leftover process is the usual reason a launch looks hung.

Verified here in `--sim` mode: gate, broker and mGBA came up, the tester ROM
ran, verdict **PASS**, everything torn down cleanly.

`--sim` skips the viewer entirely, so that run says nothing about the
QEMU → UART2 → viewer display path. Under QEMU the same lab reaches **PASS** on
the link while the viewer stays blank; see *When the screen stays blank* above.

### Driving the interface without a rotary encoder

QEMU wires UART peripherals, not the GPIO matrix, so nothing the encoder driver
reads will ever change. Two new pieces close that:

- `port/esp32s3/input_qemu.c` reads one-byte commands from **the same UART2
  socket** the framebuffer streams out of. A QEMU tcp chardev is bidirectional,
  so the read direction costs nothing and needs no extra port.
- `qemu/viewer.py` shows the screen live and sends those bytes back.

| key | does | byte |
|---|---|---|
| Up / Down | one detent | `u` / `d` |
| PgUp / PgDn | ten detents, an accelerated spin | `U` / `D` |
| Enter | the encoder button | `s` |
| Backspace | back | `b` |
| Tab | next library tab | `t` |
| `f` | toggle favourite | `f` |
| `p` | save the frame as a PPM | — |
| `q` | quit | — |

Events go through the same `ui_event_t` queue the real encoder would use, so
`ui_service()` cannot tell the difference and no UI code is emulation-aware.

The commands are one byte and typable on purpose: when something is behaving
oddly you can drive the interface with `nc 127.0.0.1 5557` and no viewer at all.

### Two display back ends

Tk gives a real window with nearest-neighbour scaling. If Tk is missing --
common on a server -- it falls back to drawing in the terminal with half-block
characters and 24-bit colour, which needs no libraries and works over ssh. A
128x128 screen becomes 128 columns by 64 rows. Force either with `--tk` or
`--term`.

The frame parser resynchronises on the `FBUF` magic rather than assuming
alignment, because the firmware is usually already streaming when the viewer
connects. When several frames arrive at once only the newest is drawn. Both
cases are covered by the parser test, along with a frame split across two
socket writes.

Colour conversion is checked against `test_render.c`'s `save_ppm()` so a screen
captured from live firmware and one from the host render test are directly
comparable.

## The tester ROM, proven end to end

`rom/tester.gba` is a purpose-built GBA program that drives the gate handshake
and checks itself. It has been run: **ROM in mGBA -> Lua -> broker -> the real
`chipgate.c`, verdict PASS.**

```
PASS
status=1
frames=29
linked=24
gate_id=FF00
```

And the control, with a gate built for BN4 while the ROM expects BN6:

```
FAIL
status=2
frames=630
linked=0
gate_id=0000
```

A test that cannot fail proves nothing, so the mismatch case is part of the
evidence.

`make gate-sim` builds `build/gate_sim`, the firmware's gate as a host process
-- the same `chipgate.c` and `link_proto.c`, minus ESP-IDF. It stands in for the
QEMU firmware so the rest of the chain can be proven first. If the ROM is green
against `gate_sim` and red against QEMU, the difference is the ESP32 build or
the emulator, not the protocol.

```
make gate-sim
./build/gate_sim 5556 bn6                              # terminal 1
python3 qemu/broker.py --qemu 127.0.0.1:5556 --mgba 7000   # terminal 2
LINK_PORT=7000 mgba-headless --script qemu/mgba_tester.lua rom/tester.gba
```

### Three things that had to be fixed to get there

**mGBA discards a script's writes to the SIO data registers.** The ROM
originally read `SIOMULTI0` and wrote `SIOMLT_SEND`, on the reasonable theory
that avoiding `SIOCNT` was enough. It is not:

```
GBA Serial I/O: GPIO write: Unhandled SIOMULTI0 <- FF00
```

The gate's answer never reached the ROM, so the handshake never advanced past
the gate announcing itself and the run failed after 1200 frames with the gate
still in `waiting`. The exchange now goes through a **mailbox in EWRAM**, where
a script's write always lands. Since the ROM is ours, nothing is lost by
defining the interface rather than borrowing registers an emulator is entitled
to arbitrate.

The mailbox is sequence-numbered rather than frame-paced, because a socket
round trip routinely outlasts a frame and a flag would be missed or
double-counted.

**`os.exit()` from inside a callback segfaults mGBA.** Exit 139, empty log, no
Lua error to work from. The verdict now goes to a result file and the harness
timeout ends the run.

**`emu:screenshot()` during teardown does the same.** Dropped; `fbgrab.py`
takes a picture from the firmware side instead.

**A socket callback plus `pump()` in the frame handler is reentrant.** The
`sock:add("received", ...)` registration is gone; `pump()` is called from
`onFrame` and nowhere else.

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

## The fully automated test: a custom tester ROM

The caveat above is about a *retail* game whose SIOCNT timing we cannot see. A
purpose-built ROM removes it: `rom/tester.c` drives the handshake itself and
never touches SIOCNT. Each frame it reads the gate's word from `SIOMULTI0`,
stages its own into `SIOMLT_SEND`, and runs the game side of the handshake
(`rom/handshake.h`) edge-driven on the observed word — so it tolerates the
frame or two of latency the QEMU ↔ broker ↔ mGBA round trip adds. It asserts the
gate announces `FF00` then the documented 9-slot rotation, writes a PASS/FAIL
verdict to a fixed EWRAM address, and paints the screen green or red.

`qemu/mgba_tester.lua` is the correct bridge for this: the firmware gate
*consumes* `WORD` packets and *emits* `RESULT` packets (the gate's answer is
`RESULT` word[1] — see `link_bridge.c` / `link_source.c`), so the script sends
`WORD` and receives `RESULT`. `mgba_link.lua` has these the other way round,
which is why the retail relay is unreliable; the tester ROM does not use it. The
same script then reads the ROM's verdict and exits.

```
make qemu-rom-selftest      # tool-free: ROM logic vs real chipgate.c + PPM path
make qemu-rom               # full end-to-end, or a clean SKIP if tools are absent
```

`make qemu-rom-selftest` (also `qemu/test_rom_selftest.py`) needs only `cc` and
`python3`: it compiles `rom/handshake.h` against the real `src/chipgate.c`,
drives a full round trip to a linked, rotating session on all four gate
identities, and checks that a driver which never sends the sync word fails to
link. It also feeds a synthetic frame through `fbgrab.py` and checks the PPM
matches `test_render.c` byte-for-byte. Run it first, exactly like `qemu-proto`.

`make qemu-rom` (`qemu/test_rom.sh`) runs the whole thing: it builds
`rom/tester.gba` (bare `arm-none-eabi`, no devkitPro; header finished with
`tools/fix_header.py`), boots the gate in QEMU headless, relays through
`broker.py --mgba`, runs the ROM in `mgba-headless`, grabs the gate's screen
with `fbgrab.py`, and reports PASS/FAIL. Any missing heavyweight tool (ESP-IDF,
Espressif's QEMU, the ARM toolchain, a scriptable mGBA) turns the live run into
a clean SKIP with an install hint — the tool-free self-test still runs. Outputs
land in `build/rom-test/`.

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
