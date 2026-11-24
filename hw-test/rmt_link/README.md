# RMT GBA link test: one ESP32-S3 SuperMini, two jumpers, a logic analyzer

Does the S3's RMT send and receive the GBA link's waveforms, on real silicon?
This firmware answers that with nothing but the board.

RMT sends the link's waveforms on two pins, jumpers feed them back into two
more, and RMT receives them. The firmware decodes what came back and prints
PASS or FAIL over USB. A logic analyzer then checks the timing against its own
clock, which the loopback cannot do: send and receive share the ESP32's clock.

## What it checks

| part | what | passes when |
|---|---|---|
| A | Multi-Player frames on SD (start, 16 bits, stop; 115200 baud), 16 words. RX idle threshold 200 µs. | every word comes back |
| B | the same with the RMT guide's `signal_range_max_ns` of 60 µs | the words the host model predicts fail do fail, and the rest pass |
| C | normal mode: SC and SO started together by a sync manager, 8 words at 256 kHz and at 2 MHz; SO read at SC's rising edges | every word comes back |
| D | channels made, used and deleted 200 times, each torn down with a receive still pending (as a mode switch would) | no driver error, every frame back, the heap returns |

Then it sends one burst a second (A's frames, then C's words) with GPIO1 high
for its length, for the analyzer.

**What B shows.** The 60 µs threshold breaks frames two ways:
- **Cut short:** 0x0000 holds SD low for 17 bits (147.6 µs). The capture ends
  part-way, and the frame is reported short.
- **Read as the wrong word, with no error:** a high run over 60 µs in mid-frame
  also ends the capture, and what is left looks like the stop bit running into
  the idle line. 0xF87F reads as 0xFFFF.

At 256 kHz, normal mode's SO can hold one level for a whole 32-bit word
(125 µs), so it needs a threshold over that too. 200 µs covers both.

## The bench

**Wiring** (SuperMini pin labels are the GPIO numbers):

| GPIO | role | jumper | analyzer |
|---|---|---|---|
| 4 | SC out | to GPIO 7 | D0 |
| 5 | SO / SD out | to GPIO 6 | D1 |
| 6 | SO / SD in | from GPIO 5 | D2 (optional) |
| 7 | SC in | from GPIO 4 | D3 (optional) |
| 1 | marker: high during a burst | | D4, trigger on its rise |
| GND | | | GND |

The pins avoid the S3's strapping pins (0, 3, 45, 46), USB (19, 20) and the
SuperMini's LED on 48. Nothing else connects: no GBA, no level shifter, no
power but USB.

**Flash** (from this directory, with ESP-IDF 5.5):

```sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Or a prebuilt merged image, at offset 0:

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 rmt_link_s3_supermini.bin
```

If the port does not appear, hold BOOT while plugging the board in.

**Read the result.** The serial monitor shows each word, and then:

```
RMTLINK RESULT A PASS, B PASS, C PASS, D PASS
```

`nothing came back on GPIO6` means the GPIO5 to GPIO6 jumper is off.

**Capture a burst.** Sample at 24 MHz with a trigger on D4's rise, for 200 ms
(one burst is about 30 ms). With sigrok, for an fx2lafw-class analyzer:

```sh
sigrok-cli -d fx2lafw --config samplerate=24m --time 200 \
    --channels D0,D1,D2,D3,D4 --triggers D4=r -O vcd -o burst.vcd
```

Or in PulseView: capture, then File > Export > Value Change Dump.

**Check it:**

```sh
python3 tools/la_check.py burst.vcd --sc D0 --so D1 --mark D4
```

It checks the following:
- every Multi-Player word decodes;
- the bit time, fitted over every edge, is within 1% of 115200 baud's, and
  each edge is within a sample and a half of the fit;
- every normal-mode word reads back;
- SC's period is within 1%;
- SO changes within a sample and a half of SC's fall, which is the sync
  manager's skew.

At 24 MHz a sample is 41.7 ns, so a 2 MHz half period is 6 samples. The checks
on single edges allow for that.

## Tested without the board

- `make rmt-link-host`, from gbagate's top level, is part of `make`:
  - `test/test_codec.c`: every frame encoded, received through a model of the
    RX idle threshold, and decoded. It includes 1100 Multi-Player words and
    the 60 µs predictions: 91 of them fail, cut short or read wrong. It also
    covers normal mode at both rates, and a 2% fast or slow sender.
  - `test/test_la_check.py`: `la_check.py` against captures made at 24 MHz:
    - it passes a correct burst, and one 0.3% off;
    - it fails a clock 3% fast or 1.5% slow, a wrong word, and SO 100 ns late.
- **In qemu_esp32** (patch 0022 models RMT's transmit registers, not its
  receive side, and does not drive the pin), with a UART console build:
  - it boots;
  - it makes its four channels and the sync manager;
  - transmits complete;
  - 200 rounds of making and deleting channels, with a receive pending, give
    no error and no heap loss.
  - A to C fail there, as they must: nothing comes back.
