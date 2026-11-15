# gbagate RF capture hardware/resource map

**Review baseline:** `claude/loving-clarke-gkv23y` at `6f6fa2288f22ec29811512f7a8de3cd7bd69f963`
**Reviewed:** 2026-11-14
**Status:** source-level architecture audit; not a capture implementation and not a hardware validation report.

This note maps the existing ESP32-S3 firmware resources against a proposed receive-only AGB-015 RF capture experiment. It intentionally leaves the GBA-facing RFU code unchanged.

## 1. Executive finding

The ESP32-S3 has a plausible **receive-only measurement path** through the chip's own 2.4 GHz receiver and ESP-SDR's raw-I/Q capture mechanism. This is a useful first experiment, but it is not yet proven to recover an AGB-015 waveform. The capture path needs to be validated for actual AGB-015 signal level, envelope contrast, analog bandwidth, clipping, carrier offset, and burst timing.

Do not add continuous RF capture or DSP to the normal gbagate runtime yet. The current firmware reserves core 1 for timing-sensitive GBA link work; ESP-SDR also takes control of the S3 Wi-Fi/PHY receiver and reserves internal SRAM. The upstream continuous-ring implementation disables interrupts for its run. Those are material conflicts, not a component-integration detail.

**Recommended first experiment:** a separate, receive-only ESP-SDR diagnostic firmware image on a spare S3 board (or a board deliberately removed from normal gbagate service), using finite 40/80 MS/s captures, with the existing gbagate firmware and GBA-facing RFU behavior untouched. Analyze saved I/Q files on the host.

## 2. Existing gbagate CPU and task ownership

### Core 1 — preserve for link timing

The source assigns the link subsystem to core 1:

- `pio/src/tasks.c` defines `CORE_LINK 1`, `PRIO_LINK = configMAX_PRIORITIES - 1`, and creates `link_task` pinned to core 1. The task services the gate's GBA link source.
- `port/esp32s3/rfu_link_esp32s3.c` creates `rfu_task` pinned to core 1 at `configMAX_PRIORITIES - 1`. It owns the dedicated-GPIO bundles used for GB-Link's GBA RFU interface. Its `watch()` loop polls SC and shifts 32-bit words inside a critical section; the adapter-master exchange also runs in a critical section.
- `port/esp32s3/gba_link_esp32s3.c` creates `phy_task` pinned to core 1 at `configMAX_PRIORITIES - 1`, and installs the SC GPIO ISR from a helper task pinned to core 1. Its parent transfer path masks interrupts around a word; its child ISR performs the exchange in interrupt context.

The two PHYs share the GBA connector and coordinate ownership (`rfu_link_esp32s3.c` calls `gba_link_yield(true)` while taking the pins). They are not spare compute workers. The `tasks.c` opening comment says “One task” owns core 1; the current code has the separate PHY tasks above as well. Treat the intent—core 1 is timing-critical link territory—as authoritative, not the literal task count in that comment.

**Constraint:** do not schedule raw-I/Q processing, FFTs, file I/O, serial output, or a capture worker on core 1. Do not change the RFU task affinity, critical sections, dedicated-GPIO bundle ownership, ISR allocation, or link timing as part of capture bring-up.

### Core 0 — application and radio/network services

The UI task in `pio/src/tasks.c` is pinned to core 0. `port/esp32s3/net_esp32s3.c` pins the HTTP server, network task, and Celio WebSocket task to core 0. Wi-Fi event/driver work and the rest of the application share the non-link side of the system.

A finite capture command may execute on core 0 in a dedicated diagnostic image, but that does **not** make it safe to run concurrently with gbagate's normal network services: the raw-I/Q path takes over the same integrated Wi-Fi/PHY receiver. Keep CPU scheduling and RF hardware ownership as two separate questions.

## 3. Board I/O map relevant to RF capture

The ESP-SDR receive path uses the S3's integrated 2.4 GHz radio; it does not need an external RF data pin. The host still needs a data-output path.

The current `port/esp32s3/board.h` assignments include:

| Resource | Devkit map | Supermini map | Capture implication |
|---|---:|---:|---|
| GBA link SC / SD / SI / SO | GPIO 4 / 5 / 6 / 7 | GPIO 4 / 5 / 6 / 7 | Preserve; these are the physical GBA connector signals. |
| USB D− / D+ | GPIO 19 / 20 (reserved by board header) | GPIO 19 / 20 (USB pins) | ESP-SDR native USB Serial/JTAG is an alternative diagnostic transport, not an additional independent connector assumed to coexist with gbagate TinyUSB. |
| SPI SCK / MOSI / MISO | GPIO 12 / 11 / 13 | GPIO 12 / 11 / 13 | Display and SD-card bus; do not repurpose for capture. |
| LCD CS / DC | GPIO 14 / 21 | GPIO 10 / 48 | Existing display wiring. |
| SD-card CS | GPIO 10 | GPIO 3 | Existing storage wiring; GPIO 3 is not a spare on the Supermini. |
| I2C SDA / SCL | GPIO 8 / 9 | GPIO 1 / 2 | Existing board services. |
| Bridge UART | GPIO 47 / 2 when fitted | not available in Supermini map | Board-specific; do not assume UART capture output is free. |
| UART0 default pins | GPIO 43 / 44 | GPIO 43 / 44 | ESP-SDR's UART0 transport needs a board-level routing check; on carrier variants 43/44 are explicitly used for the bridge UART. |

The Supermini map documents only fourteen usable header pins and says the link, display, and card take twelve, with the remaining two assigned to I2C. It has no spare general-purpose capture pins. This is not a blocker for internal-radio reception, but it rules out casually adding external parallel capture wiring to that board.

**Transport decision is still open:** use a separate native USB Serial/JTAG capture image only after confirming the exact board's USB wiring and host connection, or choose UART0 after verifying GPIO43/44 and console/bridge ownership on that exact board. Do not enable two owners for the same USB/UART peripheral or pins.

## 4. ESP-SDR S3 receive path: what the source actually does

The upstream ESPARGOS `esp-sdr` ESP32-S3 receiver is a raw-I/Q receiver built around the S3's internal radio/ADC path. The project describes this as bypassing the normal fixed-function Wi-Fi modem; it is receive-only. The ESP32-S3 raw receiver can be useful for amplitude-modulated signals in the 2.4 GHz band, but its existence does not prove the required AGB-015 waveform can be recovered.

Source-level observations from the S3 backend:

- `main/targets/esp32s3/receiver.c` sets the snapshot buffer to `0x3fcd0000`, with a maximum of 16,380 complex samples (`IQ_WORDS`).
- `CAP20 <count> <rate-code>` captures signed 10-bit I and signed 10-bit `-Q` values packed at 20 bits per complex sample. The serial protocol deliberately sends `(I, -Q)` pairs; negate the second component when converting to the conventional `(I, Q)` representation. The packing code in `main/common/burst_capture.h` stores two samples in five bytes.
- The S3 rate codes in the current firmware are `0 = 80 MS/s`, `1 = 40 MS/s`, and `6 = 16 MS/s`. At 16,380 samples, a snapshot spans approximately 204.75 µs at 80 MS/s, 409.5 µs at 40 MS/s, or 1.02375 ms at 16 MS/s.
- The receiver waits for the capture-done status with a bounded timeout, restores the SRAM owner and receive filter, then sends the capture payload. This is a finite snapshot, not a gapless stream.
- The binary `DATA` response reports sample count, CRC32, and elapsed capture time. A gbagate capture archive should add its own wall-clock/trigger timestamp, board identity, firmware revisions, configured center frequency, measured gain state, and dropped/invalid-capture flags.
- `LIMITS?` advertises S3 nominal rates and a bandwidth range; the receive-controls documentation warns that gain indices are not calibrated dB values and that AGC can clip strong continuous signals. Use conservative manual gain and inspect for clipping rather than trusting AGC.
- `FREQ 2432` is within the intended initial tuning target. Confirm the tuned carrier and any offset from actual AGB-015 captures; do not treat the setting as a measured adapter carrier.

The upstream `IQS` continuous-I/Q command is **not** an alternative raw-waveform capture mode: on S3 it filters and decimates the 16 MS/s ring by 64–1024, producing at most 250 kS/s of output. That stream is useful for narrowband SDR tooling, but it cannot resolve 100 ns Manchester half-bits. The high-rate finite `CAP20` snapshots are the relevant ESP-SDR path for initial PHY characterization.

At the MC13190's 5 Mbit/s typical baseband rate, the nominal sampling density is:

| S3 sample rate | Samples per data bit | Samples per 100 ns Manchester half-bit |
|---:|---:|---:|
| 16 MS/s | 3.2 | 1.6 |
| 40 MS/s | 8 | 4 |
| 80 MS/s | 16 | 8 |

80 MS/s is the preferred first characterization setting; 40 MS/s is a useful comparison. 16 MS/s is not the initial choice for resolving the half-bit waveform. These ratios say nothing by themselves about analog bandwidth, SNR, dynamic range, calibration, or whether the signal's AM envelope survives the receiver path.

### The upstream command path

The current ESP-SDR source handles `INFO`, `LIMITS?`, `FREQ`, `BANDWIDTH`, manual gain commands, and `CAP20`. The command handler runs from the receiver application's `app_main()` service loop; it is not a drop-in gbagate component with a pre-existing task-affinity contract.

The upstream S3 application initializes Wi-Fi in `WIFI_MODE_NULL`, starts the Wi-Fi driver, enables promiscuous reception, and sets its channel before preparing the raw receiver. This matters: it is not simply opening an independent ADC peripheral next to an active ESP-NOW session.

## 5. Hard integration blockers

### A. The RF receiver is shared with Wi-Fi/ESP-NOW

`port/esp32s3/peer_espnow_esp32s3.c` initializes Wi-Fi through `radio_init()` and uses ESP-NOW on that radio. `port/esp32s3/net_esp32s3.c` also owns station/hotspot operation. ESP-SDR's startup changes the Wi-Fi mode, enables promiscuous mode, and directly configures the receiver/PHY. Do not run the ESP-SDR receiver setup at the same time as gbagate's ESP-NOW or normal network mode and expect both transports to work.

**Decision:** initial RF capture belongs in a separate diagnostic firmware image. A later integrated mode would need explicit radio ownership, shutdown/restart of Wi-Fi services, a tested return-to-normal path, and acceptance tests for the existing ESP-NOW behavior.

### B. Continuous ring capture is not suitable for the live firmware

The S3 `ring_capture.c` implementation reserves SRAM banks and rotates them while processing. Its own source states that interrupts remain disabled during bank rotation (including no FreeRTOS tick, Wi-Fi ISR, or driver service) and that the mode requires the interrupt watchdog disabled. The S3 `s3_core1.S` worker is a bespoke IRAM worker with interrupts masked at level 15; the upstream S3 sdkconfig also selects `CONFIG_FREERTOS_UNICORE=y` and disables the interrupt and task watchdogs.

That execution model conflicts with gbagate's normal two-core link, Wi-Fi, USB, and task-watchdog assumptions. Do not import `ring_capture.c`, the SPEC worker, the ring assembly, or its watchdog/sdkconfig changes into the normal gbagate image as the first RF milestone.

### C. SRAM layout must be ported deliberately

ESP-SDR reserves the S3 dump/ring region `0x3fcb0000`–`0x3fce0000` and uses `0x3fcd0000` for the finite snapshot buffer. Those are fixed internal SRAM addresses, not heap allocations. The upstream linker guard and `SOC_RESERVE_MEMORY_REGION` declaration are part of the implementation. A component copy without matching reservation/linker treatment can collide with gbagate static data, ROM-reserved areas, or other memory users.

Before any integration build, compare the exact IDF 5.5.6 linker map and memory layout for the selected gbagate board/configuration. Record the occupied region and prove that the capture buffer cannot overlap any existing reservation. Do not “solve” a linker collision by moving the address without checking the hardware register's address window.

### D. Transport and USB ownership are not interchangeable

ESP-SDR supports native USB Serial/JTAG and UART0 on the S3. gbagate's hardware image includes GB-Link USB/TinyUSB paths, and the board map reserves USB D−/D+ pins 19/20. UART0's default GPIO43/44 routing also needs to be checked against console and carrier bridge wiring. Select one transport explicitly in the diagnostic image; do not assume the same connector/peripheral can be concurrently owned by ESP-SDR and gbagate.

## 6. Recommended first prototype boundary

1. Use a spare S3 board or a deliberately isolated diagnostic build. Keep the working gbagate firmware available and do not risk the user's only working board with experimental RF register writes.
2. Pin the ESP-SDR source revision and its ESP-IDF revision. Its S3 target profile currently records an ESP-IDF git ref; resolve and record the exact upstream commit/version before building. Do not silently substitute gbagate's IDF 5.5.6 or IDF 6.x.
3. Start with finite `CAP20` snapshots to validate tuning, gain, sample packing, and host decoding. Begin at 80 MS/s and compare 40 MS/s. Use manual gain sweeps and confirm the sample payload CRC and non-sentinel capture data. A maximum snapshot is only 204.75 µs at 80 MS/s, and snapshots have gaps.
4. If a single snapshot is too short to contain a useful exchange, evaluate bounded `RINGCAP` runs (one to three units) **only in the isolated ESP-SDR diagnostic firmware**. This path uses the upstream ring implementation and masks interrupts during capture; it must not be moved into gbagate's normal runtime. Do not use the decimated `IQS` stream for Manchester waveform recovery.
5. Capture repeatable AGB-015 activity in separately labelled states: idle, power/discovery, join attempt, connected/idle, active multiplayer exchange, and disconnect. Repeat each state and retain the untouched raw payload plus metadata.
6. Analyze on the host first: unpack signed 10-bit `(I, -Q)`, restore conventional Q polarity, and inspect I, Q, magnitude-squared, and phase; measure baseline/noise, clipping, burst start/end, envelope levels, carrier offset, and inter-burst timing. Keep the original wire payload immutable. Do not start with an on-device decoder.
7. Compare the captured waveform with an independent SDR/spectrum analyzer where possible. If the internal S3 receiver does not preserve a separable AM envelope with adequate SNR and timing, stop the S3 PHY port and evaluate a different receiver rather than rewriting the GBA-side RFU code.

## 7. Evidence ledger and source locations

### gbagate files reviewed at the handoff baseline

| File | Blob SHA | Why it matters |
|---|---|---|
| `pio/src/tasks.c` | `b08c7cf806533e8689797bd2d62d51951e69dd61` | Core affinity and application task priorities. |
| `pio/src/app_main.c` | `bebee25b68241d80ee75f397740d4022c5d99442` | Starts GBA/RFU drivers and network services. |
| `port/esp32s3/rfu_link_esp32s3.c` | `bf0b385768dd145aba0303d1848a9cb98ec4e5d1` | Core-1 RFU task, dedicated GPIO, mailbox, critical sections, SD ISR. |
| `port/esp32s3/gba_link_esp32s3.c` | `91d4f799076be0eb612c7b1ac47a9362bd6efdf6` | Core-1 Multi-Play task and SC ISR; shares the connector. |
| `port/esp32s3/board.h` | `626c0ab8fc481a131bd0df1570803033df7e4676` | Board pin ownership. |
| `port/esp32s3/peer_espnow_esp32s3.c` | `fed3ba32250bee04eb836914ffca54fe53b27fb3` | ESP-NOW uses the same integrated Wi-Fi radio. |
| `port/esp32s3/net_esp32s3.c` | `9f434053c9b1bce9d2acab6d2bb865b20100dfd4` | Station/hotspot and core-0 task ownership. |
| `pio/src/CMakeLists.txt` | `2ee07b28ceff5d268d237f92952c7b84b6d8d140` | Hardware source/component selection. |

### ESP-SDR sources inspected (upstream `main`, 2026-11-14)

- `main/targets/esp32s3/receiver.c` — finite snapshot capture, tuning, radio startup, buffer address, command handling.
- `main/common/burst_capture.h` — IQ10/IQ8 packing and `DATA` CRC header.
- `main/common/burst_limits.h` — S3 rate and bandwidth limits.
- `main/common/ring_capture.c` and `main/common/ring_capture.h` — continuous ring behavior and interrupt requirements.
- `main/targets/esp32s3/s3_core1.S` — bespoke core-1 worker.
- `main/targets/esp32s3/linker.lf` — no-flash mapping for ring-processing kernels.
- `sdkconfig.defaults.esp32s3` — upstream single-core/watchdog configuration.
- `firmware-targets.json` — S3 profile pins ESP-IDF commit `25fe69f946311abdaf9ad56591f25fedbc20ac98` in `espressif/esp-idf` (resolved as a real commit); do not confuse this with gbagate's IDF 5.5.6 environment.
- ESP-SDR's repository README declares the firmware `GPL-3.0-or-later`; preserve its notices and satisfy the license before reusing code. The README distinguishes the uncopyrightable capture mechanism from the project's implementation code, so an independent implementation of the mechanism is an option.

Upstream references:

- [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr)
- [S3 receiver implementation](https://github.com/ESPARGOS/esp-sdr/blob/main/main/targets/esp32s3/receiver.c)
- [Continuous IQ stream documentation](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/iq-stream.md)
- [Receive controls and clipping guidance](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/rx-controls.md)
- [ESP-SDR serial protocol, including the `(I, -Q)` convention and `CAP20` packing](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/protocol.md)

## 8. Explicit non-claims

This source audit does not establish that an ESP32-S3 can decode an AGB-015 packet, that 2432 MHz is the actual carrier in every adapter state, that the waveform is recoverable with the S3's analog front end, or that any ESP-SDR transmit/replay path is compatible with the MC13190. No RF capture, RF transmission, original-adapter join, or hardware interoperability test was performed for this review.
