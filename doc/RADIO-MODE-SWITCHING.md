# ESP32-S3 exclusive radio mode switching

**Baseline:** `claude/loving-clarke-gkv23y` at `00225faf7eddf731010586898819a0d5255f118b`  
**Scope:** lifecycle contract and host-tested state machine; no ESP-IDF radio calls are changed by this overlay.

## Why this is needed

The Online and Wireless features use the ESP32-S3's one integrated Wi-Fi radio. Switching modes does not require simultaneous operation or a second radio, but the present lifecycle is not reversible:

- `net_esp32s3.c::radio_init()` has a one-way static `up` guard and starts the Wi-Fi driver. It does not expose a matching radio stop/start lifecycle.
- `net_start()` starts the HTTP server, Celio WebSocket task, and network retry task. There is no suspend API.
- `net_task()` can retry station association after the Online screen has exited.
- `mode_exit_online()` stops the Celio session but leaves the network service and radio running.
- `peer_espnow_start()` calls `esp_now_init()` and sets `s_up`; `wireless_cable_stop()` stops the protocol and clears the peer lock, but does not call `esp_now_deinit()` or relinquish the radio.
- `mode_exit_wireless()` therefore does not leave a clean hand-off point for Online or SDR capture.

Relevant source baseline: `port/esp32s3/net_esp32s3.c`, `port/esp32s3/net_esp32s3.h`, `port/esp32s3/peer_espnow_esp32s3.c`, `pio/src/gate_app.c`, and `pio/src/app_main.c`.

## Ownership model

Exactly one of these logical clients owns the RF block at a time:

| State | Client | Required behavior |
|---|---|---|
| `OFF` | none | Wi-Fi/PHY stopped or otherwise explicitly relinquished |
| `ONLINE` | station / setup AP / HTTP / Celio | network services may use Wi-Fi; no ESP-NOW or SDR raw-PHY setup |
| `WIRELESS` | ESP-NOW peer cable | ESP-NOW owns the radio; Online's retry and socket services are quiesced |
| `SDR_RX` | finite raw-I/Q receiver | Online and ESP-NOW are stopped; receiver controls Wi-Fi mode/PHY |
| `FAULT` | unknown | a client could not clean up a partial start; refuse new owners until platform recovery or reboot |

`src/radio_mode.c` is a platform-neutral coordinator. Each ESP-IDF client supplies `start` and `stop` callbacks. The manager:

1. Stops the current client before starting the next one.
2. Treats a failed stop as a hard barrier: it does not start another client.
3. Calls the target client's stop callback after a failed start, because startup may have partially acquired resources.
4. Restarts the previous owner if target startup fails and target cleanup succeeds.
5. Reports `FAULT` if cleanup fails; it never pretends the radio is free.
6. Saves the pre-capture owner and restores it on `radio_mode_capture_end()`.
7. Does not silently restart SDR capture when restoration fails; the safe result is `OFF`.

The coordinator is deliberately synchronous and has no FreeRTOS dependency. Its API contract requires calls to be serialized by the caller. Firmware integration must make the app/UI task on core 0 the owner of transitions and must not call it from a Wi-Fi callback, ISR, or core-1 link task.

## Required ESP-IDF client behavior

### Online stop / resume

Before yielding the radio, Online must:

- Mark the network service suspended before any asynchronous work can retry or reconnect.
- Stop Celio intent, wake its task, and wait for any in-flight transport connect/read path to quiesce with a bounded timeout.
- Stop the HTTP server and invalidate the `/gblink` socket descriptor.
- Stop station retry and ignore late Wi-Fi/IP events while suspended.
- Disconnect and stop the Wi-Fi driver without destroying the global default event loop or netif objects.
- Retain stored station/AP configuration so `ONLINE.start` can restore it.

Resume must be idempotent, restart the driver in the intended mode, reapply STA/AP configuration, re-open HTTP service, and restart association without creating duplicate tasks or event handlers. Initialization of `esp_netif` / the default event loop must remain one-time; stopping a radio client is not permission to tear down process-global ESP-IDF infrastructure.

### Wireless stop / resume

Before yielding the radio, Wireless must:

- Stop the `gbl_peer` session and prevent new sends.
- Clear the locked-peer state and stale peer address.
- Deinitialize ESP-NOW and reset its `s_up` lifecycle flag only after successful deinitialization.
- Leave the Wi-Fi driver in a documented state for the next owner; do not claim that stopping `gbl_peer` alone releases the radio.

On start, acquire the Wi-Fi driver, set the channel, initialize ESP-NOW, register the receive callback, add the broadcast peer, then start the protocol. Every partial failure must unwind in reverse order.

### SDR capture stop / restore

SDR is a temporary lease. Acquire only after Online or Wireless has stopped cleanly. Use finite snapshots initially; do not import the upstream continuous ring worker into the normal two-core firmware. On release, stop capture, disable promiscuous reception, restore Wi-Fi driver's mode/channel/power-save settings as required, and then start the saved owner. The S3's integrated Wi-Fi/PHY driver is shared; no assumption of concurrent ESP-NOW and raw-SDR operation is permitted.

The exact PHY teardown/reinitialization sequence remains to be proven against the selected ESP-SDR revision and ESP-IDF revision before implementing the SDR callbacks. Do not deinitialize global netif/event-loop objects as a substitute for a radio hand-off.

## Mode transition expectations

- `Link: Online` -> `Link: Wireless`: stop Celio and Online services, then initialize ESP-NOW.
- `Link: Wireless` -> `Link: Online`: stop `gbl_peer`, deinitialize ESP-NOW, then resume Online services.
- `Online` or `Wireless` -> `SDR_RX`: stop the existing owner, take the temporary capture lease.
- `SDR_RX` -> previous owner: fully release SDR, then restore exactly the prior owner and its saved configuration.
- Any failed stop blocks the next owner. A failed new start attempts cleanup and rollback. A failed cleanup enters `FAULT` rather than risking simultaneous radio ownership.

`net_boot()` currently auto-starts Online when credentials are stored. Preserve that behavior initially, but treat it as the initial `ONLINE` owner; entering Wireless must suspend it. A later product decision may change boot policy, but it is not required for exclusive mode switching.

## Core and GBA-link constraints

Do not move radio transition logic, DSP, capture-buffer processing, or file I/O to core 1. The existing GBA RFU and Multi-Play link paths use core 1 and timing-sensitive GPIO/critical-section behavior. The coordinator is a small control-plane operation on core 0. The actual finite capture backend must be reviewed separately for interrupt, memory, and core usage.

## This increment and its limits

This overlay adds only the portable ownership state machine and its host test. It does **not** modify `net_esp32s3.c`, `peer_espnow_esp32s3.c`, `gate_app.c`, or the ESP-SDR receiver. Therefore it does not yet enable live mode switching. That is intentional: the next firmware integration increment must add and test the Online/ESP-NOW stop/resume APIs, wire the coordinator into the mode enter/exit paths, and only then add the SDR capture client.

## Host validation

Run:

```sh
make run-radio_mode
```

The tests cover stop-before-start exclusivity, idempotence, failed-stop blocking, partial-start cleanup, rollback, `FAULT`, capture restoration, restoration failure, retry after a failed SDR stop, and re-entrant transition rejection.
