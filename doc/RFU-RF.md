# AGB-015 RF interoperability

**Status:** experimental investigation; no OEM RF interoperability is claimed yet.

This work adds a real AGB-015-compatible radio path to gbagate's existing
Wireless Adapter (GB-Link mode `0x07`) implementation. It does not replace the
already-working GBA-facing RFU command engine. The current ESP-NOW peer path
remains available as the software-radio fallback while the OEM RF path is
proven.

## Confirmed PHY target

The AGB-015 uses the Freescale MC13190 2.4 GHz transceiver. The MC13190 data
sheet specifies:

| Parameter | Documented value |
|---|---:|
| RF band supported by the radio | 2.411–2.473 GHz receive; 2.400–2.484 GHz PLL range |
| AGB-015 nominal carrier | 2.432 GHz (9.5 MHz reference × 256) |
| Baseband data coding | Manchester, 50% duty cycle |
| Baseband data rate | 4–6 Mbit/s; 5 Mbit/s typical |
| Manchester half-bit at 5 Mbit/s | 100 ns |
| AM modulation depth | 50–88%; 60% typical |
| Receive baseband filter | 0.35–6 MHz |
| Occupied bandwidth | approximately 18–22 MHz typical across datasheet revisions |
| Receiver sensitivity | about −71 dBm at BER 2×10⁻⁴ |

Important distinctions:

- This is **AM/ASK**, not a GFSK radio and not an assumed on/off-keyed waveform.
  The nominal modulation depth is 60%; waveform levels and pulse shaping must
  be measured against an original adapter.
- The MC13190 does **not** perform Manchester encoding or decoding. Its
  baseband processor must generate TX Manchester data and recover RX data.
- Its datasheet states that the physical interface supports 4–6 Mbit/s, while
  protocol overhead limits useful application throughput to around 1 Mbit/s.
- The RFU air packet format, preamble/sync words, CRC, TDMA slot schedule,
  retransmission rules, and any frequency changes are still unknown. The
  documented GBA-to-adapter command protocol does not document this radio-side
  protocol.

References:

- [Freescale MC13190 data sheet](https://media.digikey.com/pdf/Data%20Sheets/Freescale%20Semi/MC13190.pdf)
- [GBATEK RFU component notes](https://oafc.anod.cc/man)
- [David GF: Emulating the GBA Wireless Adapter](https://www.davidgf.net/2024/01/13/gba-wireless-adapter/)

## ESP-SDR feasibility

The target needs substantially more than 4 MS/s. At 5 Mbit/s, a 4 MS/s stream
has only 0.8 samples per data bit and cannot recover the Manchester waveform.

### ESP32-S3: first RF capture platform

The upstream [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr) project
is ESP-IDF firmware and supports the ESP32-S3 at 80, 40 and 16 MS/s. It can
capture up to 16,380 complex samples per snapshot, with packed signed 10-bit
I/Q (`CAP20`). One 80 MS/s snapshot spans 204.75 microseconds. This is the
best first measurement path because it uses the ESP32-S3 already fitted to
gbagate and retains more I/Q resolution than the C5 diagnostic path.

The snapshots are finite bursts with gaps between them. They are useful for
characterizing a known or repeatable RFU transmission, but do not yet prove
that gbagate can catch arbitrary packets continuously. The S3's continuous
ring produces 16 MS/s raw I/Q; that is 3.2 samples per data bit, or 1.6 per
Manchester half-bit at 5 Mbit/s. Its streaming firmware decimates this further
for USB. It is a possible experiment, not the first choice for a robust live
Manchester decoder.

There is a scheduling conflict if we use the S3 continuous ring: it owns a
CPU with interrupts masked while it runs. gbagate's current GBA-side RFU link
driver uses core-1-bound dedicated GPIO for the 2 MHz GBA interface. Running
the ring on that same core conflicts with deterministic link-port servicing.
Finite burst capture is the better initial fit; its actual interaction with
the GBA link task and Wi-Fi state still needs measurement. Upstream ESP-SDR is
ESP-IDF-based, but its pinned IDF revision and component integration must be
checked against gbagate's ESP-IDF 5.5.6 build before importing code.

Upstream ESPARGOS/esp-sdr is receive-only. The separate
[esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr) project adds
experimental TX/DAC support, including 40/80 MS/s S3 playback. That TX code is
not a drop-in ESP-IDF component; porting or independently validating it is a
separate task.

### ESP32-C5: optional continuous-RX / replay candidate

The C5 path in `esp-sdr-zephyr` is a possible second-stage radio processor:

- Its diagnostic-bus receive path captures 40 MS/s continuously through
  PARLIO/GDMA. It carries only 4 bits of I and 4 bits of Q per sample, so
  quantization headroom and envelope SNR must be measured. At 40 MS/s it gives
  8 samples per data bit and 4 samples per Manchester half-bit at 5 Mbit/s.
- Its DAC replay engine outputs at 40 or 80 MS/s. Waveforms are preloaded and
  replayed; packet-by-packet generation, maximum waveform length and loop
  behaviour need measurement against real RFU burst sizes.
- The RF path is half-duplex. The RFU's time-division operation may suit this,
  but TX/RX turnaround and receive re-arming are critical tests.
- The documented diagnostic path uses GPIO lanes 0, 1, 3, 4, 5, 7, 10 and 23.
  Confirm these are available on the exact C5 board and verify antenna-switch
  routing before selecting a production board.

The C5 should remain an option, not a prerequisite. First determine what the
existing S3 can capture and transmit; add another processor only if continuous
receive or RF turnaround proves unworkable on the S3.

References:

- [ESPARGOS ESP-SDR (ESP-IDF capture firmware)](https://github.com/ESPARGOS/esp-sdr)
- [ESP-SDR serial capture protocol](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/protocol.md)
- [ESP-SDR hardware quirks and C5 diagnostic path](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/doc/hardware-quirks.md)
- [ESP-SDR C5 replay API](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/include/esp_sdr/esp_sdr_replay.h)

## Intended architecture

Keep responsibilities separate:

1. **S3 / gbagate:** keep ownership of the GBA connector, GB-Link RFU command
   protocol, UI, USB, storage and application state. Use S3 burst I/Q capture
   first for RF characterization; preserve core 1 for the existing GBA link
   driver.
2. **S3 RF backend:** integrate the receive capture path into an isolated
   diagnostic/build mode first. Evaluate S3 TX/DAC separately; do not make
   unverified low-level radio calls part of the live GBA path.
3. **Optional C5 radio processor:** if S3 snapshots cannot cover the necessary
   traffic or continuous receive is required, use a C5 for 40 MS/s diagnostic
   RX and 40/80 MS/s replay. Carry decoded RFU packets/control over a framed
   S3↔C5 link, never raw 40 MB/s I/Q.
4. **Portable host-tested baseband:** keep envelope extraction, Manchester
   coding, timing recovery and packet parsing independent of ESP-IDF/Zephyr so
   synthetic and captured samples can be tested on the host.

Do not remove the current ESP-NOW link until the replacement has passed the
mixed-device interoperability tests.

## Work sequence

### RF-0 — reference capture and electrical validation

- Obtain access to at least one original AGB-015; two originals are better for
  observing both directions and the TDMA exchange.
- Characterize its transmit burst at/around 2432 MHz using an SDR or spectrum
  analyzer with at least 20 MHz usable bandwidth. Start with the existing
  ESP32-S3's 80 MS/s `CAP20` snapshots (10-bit I/Q, up to 204.75 us each).
  Account for gaps between snapshots; arrange a repeatable exchange or capture
  trigger so the burst is actually inside the window. Compare against the C5's
  continuous 40 MS/s, 4+4-bit-I/Q path only if snapshot acquisition is limiting.
- Use a shielded enclosure or a properly attenuated conducted RF setup. Check
  RF power, input limits, impedance and antenna routing before connecting
  boards directly.
- Start with a repeatable S3 capture session: `INFO`, `LIMITS?`, `FREQ 2432`,
  a manually selected gain, `BANDWIDTH 20`, then `CAP20 16380 0` (80 MS/s).
  Sweep manual gain to avoid clipping; the S3 receiver can clip a strong nearby
  signal even with its hardware AGC. Save the packed samples and CRC with every
  capture. Confirm the capture window actually overlaps a transmitted burst.
- Measure carrier accuracy, envelope levels, Manchester polarity and rate,
  pulse shape, preamble, burst length, inter-burst spacing and channel changes.

### RF-1 — portable baseband

- Add host-tested IQ envelope extraction (start with `I² + Q²`, avoiding a
  per-sample square root), baseline/DC tracking and filtering.
- Add Manchester timing recovery and decoding with configurable polarity and
  rate; test against generated signals with amplitude changes, carrier offset,
  noise, quantization and timing error.
- Add TX waveform generation with the measured modulation depth and pulse
  shaping. Do not hard-code packet framing until real captures establish it.

### RF-2 — hardware path proof

- First characterize the S3 capture path at the measured carrier and confirm
  captured bursts are not clipped or aliased.
- If continuous acquisition is required, tune and calibrate the C5 at the
  measured carrier; capture a real AGB-015 burst and compare its 4+4-bit-I/Q
  samples against the S3's 10-bit reference captures.
- Replay a captured or synthetic burst through a conducted attenuated path
  using the ESP-SDR Zephyr DAC path or a separately validated S3/C5 TX backend;
  measure frequency error, occupied bandwidth, envelope depth, harmonics and
  turnaround.
- Determine whether packet buffers fit one DAC replay window and whether
  repeated/preloaded playback meets the RFU timing.

### RF-3 — RFU interoperability

- Recover the over-the-air framing and TDMA state machine from original
  adapters, including ACK/retry timing and any channel behavior.
- Bridge decoded radio frames into the existing gbagate RFU core.
- Test all combinations: gbagate↔gbagate, gbagate↔original AGB-015, and
  original↔original as a control. Use retail GBA games for the end-to-end test.

## Acceptance criteria

OEM interoperability is not established by two gbagate units exchanging a
self-defined packet. It is established when a gbagate unit can discover, join
and exchange game data with an original AGB-015, and the reverse direction
works as well. All capture, replay and decoder claims should have recorded
sample files, reproducible host tests and hardware measurement logs.
