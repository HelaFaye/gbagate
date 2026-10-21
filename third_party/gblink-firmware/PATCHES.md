# Changes to the vendored GB-Link firmware

Every change below is marked in the source with `gbagate (PATCHES.md Pn)` and
has a test in `test/gblink_sim/test_gblink.py` that fails on the unmodified
upstream file. The fixes are offered back upstream as patches in
`backport/GBLink-Firmware/`.

| # | file | defect | effect on hardware | test |
|---|---|---|---|---|
| P1 | `callbacks/usbLinkCommand.cpp` | `subspan<48,16>()` on every data packet; anything shorter than 64 bytes reads past the payload | stale words from the previous packet replayed at the GBA (WebSerial: whatever the serial RX buffer last held) | `relay_short_packet` (ASan: global-buffer-overflow upstream) |
| P2 | `module/link.cpp` | `0x12`/`0x13` dereference `m_currentSection`, which is null before `0x10`/`0x11` and during each 400 ms reconnect pause | hard fault; a host retrying a handshake at the wrong moment reboots the adapter | `relay_role_command_before_mode_role` (UBSan + SEGV upstream) |
| P3 | `layers/packetLayer.{hpp,cpp}`, `sections/usbSection.cpp` | a `0x13` that arrives before the first `0xB9A0` has been transmitted makes `isHandshakeEnabled()` unreachable | link never comes up; the netplay bridge avoids it only by waiting 150 ms (`CONNECT_GRACE_MS`) | `relay_connect_without_grace` (hangs upstream) |
| P4 | `payloads/pokemon.cpp` `usbReceivePkmFile` | chunks copied at `g_receivedBytes` with no bound | any split but 50+50 (e.g. one 64-byte USB packet after a 50) overruns a static buffer | `trade_emu_pk3_upload_any_split` (ASan upstream) |
| P5 | `payloads/pokemon.cpp` `partnerPartyConstruct` | no bound on the 600-byte partner party | a partner sending past three 200-byte chunks overruns it | covered with P4 |
| P6 | `sections/tradeConnection.hpp` | the 2 s block-request `k_timer` is never stopped when the section is destroyed | an armed timer fires into a dead stack frame; on Zephyr the timer node itself sits in the kernel timeout list, so it corrupts that list | by inspection; the shim's timer pool asserts no live timer is re-initialised |

| P7 | `control.hpp` | (extension, not a defect) two hooks: unknown modes go to `gbagate::executeMode()`, unknown `0x4x` commands to `gbagate::hardwareCommand()` | none upstream; this is how gbagate's own features (the Battle Chip Gate mode, `0x4F` device info) reach GB-Link hosts without touching any upstream mode | `test_gblink.py` gbagate_* tests |

| P8 | `layers/transport.{hpp,cpp}` | (extension) a third route, `Local`, for a host that lives in the firmware: `injectCommand`/`injectData` reach the registered handlers as USB would, and replies go to a sink | none upstream; it is how the wireless link cable (`src/gbl_peer.c`) drives the relay with no PC. While a local sink is set, `claim()` refuses a USB/serial/bridge host's frames, so a PC cannot cancel the cable's session or draw its replies | `test_peer.py` (`wireless_cable_refuses_a_usb_host`) |

| # | file | defect | effect on hardware | test |
|---|---|---|---|---|
| P9 | `payloads/pokemon.cpp` `tradePkmnAtIndex` | the slot to trade comes from the GBA's `LINKCMD_READY_TO_TRADE` word, unchecked | an index past 5 (a corrupted link word, a homebrew ROM) reads past the partner's party and writes past ours | `trade_emu_party_index_bounded` (`test_party.cpp`, ASan: global-buffer-overflow upstream) |
| P10 | `module/gb.cpp` printer loop | replies shifted out LSB first, where the Game Boy samples MSB first (received bytes were already assembled MSB first) | none today: the only bytes sent, 0x81 and 0x00, are bit-palindromes; a real status (0x02 busy) would arrive as 0x40 | `backport/verify-printer.sh` (status 0x02: read as 0x40 before, 0x02 after); `printer_stream` unchanged |
| P11 | `layers/transport.{hpp,cpp}`, `control.hpp`, `module/gb.cpp` | every send went to whichever host last delivered a frame | a USB host only querying (the launcher polling 0x0F) drew a companion MCU's mode traffic to USB; GB-Link-Switch-LDN, hearing nothing, re-entered the mode after 3 s and reset the adapter under the game. Now a mode's status and data go to the host that started it (`claimForMode`), command replies to the host that asked (`sendReply`) | `bridge_companion_keeps_its_mode_while_usb_queries`; `test_ldn.py` with LDN's own `pico_link.c` (two SetModes before, one after) |
| P12 | `persist.cpp`, `sections/rfuProtocolSection.cpp` | two GCC 14 warnings under `-Wextra` / C++20: an enumerator mixed with `uint8_t` in `?:`, and an assignment chained through a `volatile` | none: same values, same effect | the simulator build is warning-free |

P3 also holds a connect that arrives early until the adapter has sent eight
`0xB9A0` rounds, the margin the netplay bridge measured a child GBA needs. A
host that already waits is unaffected.

P1-P6 and P9-P12 apply unchanged to 2.2.7: upstream has not touched those files since
2.2.3.

## Replaced files

`layers/usbLayer.hpp` and `layers/serialLayer.hpp` keep their public surface
and behaviour but are reimplemented over `port/gblink/gblink_port.h` instead
of Zephyr's USB stack; see the comment at the top of each. `UsbLayer` gains
`setConfigured()`, `receive()` and `hasCommandHandler()` for the port glue, and
`SerialLayer` gains a channel-3 extension hook for gbagate's own protocol.
Since 2.2.7 `SerialLayer` has upstream's second instance, the bridge UART to a
companion MCU (`getBridgeInstance()`), here over `gblink_bridge_write()` /
`gblink_bridge_on_receive()`; a board without one drops its frames, as an
RP2040 with nothing on GP8/GP9 would. `s_rxOverflows` exists for upstream's
telemetry and stays 0: bytes are handed over synchronously, with no ring to
overflow.

## Not gbagate's to fix

- **Celio-Client and Celio-Server do not interoperate at their current heads**
  (the client acks and retries since b057315; the server does not answer
  acks). Nothing in the firmware is involved; the server fix is offered in
  `backport/Celio-Server/` (see backport/README.md).

## Known and left alone

- **One semaphore for both handshake values** in `PacketLayer`
  (`getReceivedHandshake`, `getTransmittedHandshake`). It is given once per
  handshake word, and both values come from that one exchange, so it means "a
  handshake word completed"; only one caller waits at a time. Not a defect, so
  not offered upstream.
