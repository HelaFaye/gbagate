# Bring-up: the GB-Link adapter on a devkit

The order matters. Each step relies on the one before it, and each says what a
good result looks like and what a bad one points at. `tools/gblink_bench.py`
runs the checks over the board's serial port. It needs Python and nothing
else, and it is tested against the simulator (`test_bench.py`), so its "ok"
means what it says.

You need:

- an ESP32-S3 DevKitC-1 and a USB-C data cable to the native USB port (the
  one marked USB, not UART);
- a GBA and a GBC link cable (a GBA cable will do for steps 2-3, but not for
  step 4);
- for step 4, FireRed, LeafGreen or Emerald;
- for step 5, a Mega Man Battle Network game;
- ideally, a logic analyser with 4 channels at 10 MHz or more.

The serial port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on macOS. Where
the steps below say `PORT`, use that.

---

## 0. Flash and boot

    pio run -e gate -t upload       (or tools/build_hw.sh gate, then esptool)
    pio device monitor              the log, on the UART port

**Good:**

- the log shows `Multi-Play PHY on core 1`, `Wireless Adapter PHY on core 1
  (dedicated GPIO)` and `GB-Link USB: 2FE3:000A`;
- the PC sees a "GBLink USB" device;
- on Windows, no driver install is needed (MS OS 2.0).

**Bad:**

- **no USB device**: check the `usb` setting is "GB-Link" (Settings), and
  that the cable is on the native USB port;
- **a panic at boot**: the log has the backtrace. Send it.

## 1. USB and identity

    tools/gblink_bench.py PORT info

**Good:**

    ok    GB-Link protocol 2.2.7, landing page True, cable selection GBC
    ok    gbagate 0.4.0, devkit, gate image, [...]
    ok    cable in effect: GBA
    ok    link counters: mp_words 0, ...

**Bad:**

- **no answer to 0x0F**: the CDC interface is not working. That points at the
  ESP32-S3's four IN endpoints not covering GB-Link's interface plus CDC. Note
  whether the *vendor* interface works; the launcher in Chrome will tell you.

Also open launcher.gblink.io in Chrome with the board plugged in. It should
connect and show the LED colours. **Do not press its update button**: it
would drop the board into its ROM bootloader (see doc/GBLINK.md).

Then:

    tools/gblink_bench.py PORT led 0 32 0

The status LED should go green.

## 2. The Multi-Play wire

Probe SC, SD, SI and SO, with the GBA connected and on, at the title screen.

    tools/gblink_bench.py PORT gen3

When it says "waiting for the GBA", enter the Pokemon Center's Cable Club
(Trade Center) on the GBA, as if a second player were there.

**Good:**

    ok    HandshakeReceived: the GBA answered the handshake (N words, late 0, dropped 0)

**Bad:**

- **"words 0"**: no clock reaches the GBA. Check SC on the analyser: as master
  the adapter should pull it low for about 0.3 ms per word, every ~16 ms while
  handshaking.
- **words climbing but no handshake**: the GBA answers, but not with
  `0xB9A0`. On the analyser, SD should carry 115200-baud serial: 8.68 µs per
  bit, start bit low, 16 data bits LSB first, stop bit high.
  - Our bits too short or too long: the timing constants in
    `gba_link_esp32s3.c` need scaling.
  - The GBA's reply on the wrong line: SD/SI/SO are swapped.
  - A GBC cable on a board without `SD_ALT`: see doc/GBLINK.md, Wiring.

Capture one word each way (ours and the GBA's) and keep it. That capture is
the measurement this whole layer has been waiting for.

Then a real link: two players over the netplay bridge (`game: gen3`) or
Celio, and one trade. Watch counters during it:

    tools/gblink_bench.py PORT watch 60

- `mp_dropped` must stay 0. If it climbs, the PHY task is not keeping up.
- `mp_late` must stay at or near 0. Late answers corrupt frames.

## 3. The board's own modes

The chip gate needs no PC:

    tools/gblink_bench.py PORT gate 4 0x012A

Use 4 for BN6 US/EU; 1, 2 and 3 are BN4, BN5 and EXE6 JP. Open the game's chip
gate screen when asked.

**Good:** "session open; chip 0x012a accepted", then "chip sent", and the game
shows the chip.

## 4. The Wireless Adapter at 2 MHz

This is the hardest step: the ESP32-S3 has to follow a 2 MHz clock edge by
edge. Use a **GBC cable**, or plug the board straight into the GBA's port.

    tools/gblink_bench.py PORT rfu 60

Start FireRed or LeafGreen, and go to a Pokemon Center: the 2F counter's
wireless club, or the Union Room. Emerald is the same. The script prints the
adapter's state as it changes.

**Good:**

    comstate ID exchange ...
    comstate wait command ...
    transfer path: worst pass N us, 0 late, ...
    ok    the game found the adapter (ID exchange done)

The game itself should get past "Checking the Wireless Adapter".

**Bad:**

- **"every word is 0"**: the receive line is grounded, as it is with a GBA
  cable. Use a GBC cable.
- **stuck at "ID exchange"**: the answers do not come back in time. The ID
  exchange runs at 256 kHz, with a fixed ~40 µs between words.
  - `worst pass` well over 30 µs: the per-word callback is too slow. Run it
    from IRAM; upstream relocates the same code to RAM.
  - `aborted` climbing: SC edges are being missed. Capture SC and our TX on
    the analyser. Our bit should change within ~100 ns of SC falling.
- **found, then dropped**: the clock hand-over (MS_Change) is failing.
  Capture SC as the adapter takes it. It must not dip low when it switches to
  output.

Then the real thing: two players' Union Room. Either use the netplay bridge
with `game: rfu` on two PCs, or set Settings → air link to "adapter" on two
gbagates and use Link: Wireless.

## 5. If you have a Switch

Build with `BOARD_HAS_BRIDGE=1 tools/build_hw.sh gate`. Wire a
GB-Link-Switch-LDN board's UART to GPIO 47 (our TX → its RX) and GPIO 2 (its
TX → our RX), and use it as its README describes for an RP2040 GB-Link.

---

## What to send back

- the output of `info`, and of every step that failed;
- the analyser captures from steps 2 and 4, even when the step passed: those
  are the measurements;
- the boot log if it crashed.
