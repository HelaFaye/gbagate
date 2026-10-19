/*
 * gblink_port.h -- the host-facing half of the GB-Link personality: the USB
 * vendor interface, the CDC byte stream, the board's LED and VCC switch.
 *
 * The vendored code reaches all of this through UsbLayer, SerialLayer and
 * Hardware (the replacement headers in third_party/gblink-firmware/src). Those
 * classes are thin and portable; whatever is board-specific is behind these
 * functions, implemented by port/esp32s3/usb_gblink_esp32s3.c and
 * gblink_hw_esp32s3.c on hardware and by test/gblink_sim/ on a host.
 */
#ifndef GBAGATE_GBLINK_PORT_H
#define GBAGATE_GBLINK_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Endpoint addresses, fixed by the GB-Link host software. EP1 OUT carries
 * commands, EP1 IN carries 2-byte status words, EP2 carries data both ways.
 * (The upstream descriptor names the pairs the other way round; the
 * addresses are what every host matches on.) */
#define GBLINK_EP_CMD_OUT   0x01
#define GBLINK_EP_STATUS_IN 0x81
#define GBLINK_EP_DATA_OUT  0x02
#define GBLINK_EP_DATA_IN   0x82
#define GBLINK_EP_SIZE      64

/* ---- supplied by the port ------------------------------------------------ */

/*
 * Queue len bytes (<= 64) on a vendor IN endpoint. Waits up to timeout_ms for
 * the previous transfer on the same endpoint to complete first, as upstream's
 * per-endpoint semaphore does, because a second write before the first is
 * collected overwrites it. Returns false if the interface is not configured
 * or the endpoint would not accept the transfer.
 */
bool gblink_usb_write(uint8_t ep_in, const uint8_t *data, size_t len, uint32_t timeout_ms);

/* Write bytes to the CDC-ACM stream. Returns false if the host has the port
 * closed or the buffer is full; the frame is then dropped whole. */
bool gblink_cdc_write(const uint8_t *data, size_t len);

/* Board hardware for the 0x4x commands. A board without the part makes the
 * call a no-op; see gblink_hw_caps(). */
void gblink_hw_set_vcc_5v(bool five_volt);
void gblink_hw_set_led(uint8_t r, uint8_t g, uint8_t b, bool on);
void gblink_hw_reboot(void);
void gblink_hw_reboot_to_bootloader(void);

/* ---- called by the port ----------------------------------------------------- */

/* A vendor OUT transfer completed (ep 0x01 or 0x02). Any context that may
 * block briefly; the handlers it reaches are the ones upstream ran from its
 * USB work queue. */
void gblink_usb_on_receive(uint8_t ep_out, const uint8_t *data, size_t len);

/* Bus state: true once SET_CONFIGURATION selects config 1, false on reset,
 * suspend-to-disconnect or unplug. */
void gblink_usb_on_configured(bool configured);

/* Bytes from the CDC-ACM stream, any fragmentation. */
void gblink_cdc_on_receive(const uint8_t *data, size_t len);

/* ---- wireless link cable (port/gblink/peer_glue.cpp, src/gbl_peer.c) --------- */

/* Run GB-Link mode `mode` (0x01 Gen 3 relay, 0x04 Advance Wars) against a peer
 * board instead of a host. nonce decides the roles: draw it at random. */
void gblink_peer_start(uint8_t mode, uint8_t variant, uint32_t nonce);
void gblink_peer_stop(void);
/* A datagram from the peer (radio callback context is fine). */
void gblink_peer_radio_rx(const uint8_t *dgram, size_t len);
/* Supplied by the port: one datagram to the peer. False if refused. */
bool gblink_peer_radio_send(const uint8_t *dgram, size_t len);
/* Optional: every status word the local firmware reports while the cable
 * runs, for a UI (or a test) to watch. Weak default does nothing. Called from
 * the firmware's own threads; must not block. */
void gblink_peer_observe_status(uint16_t status);

/* ---- lifecycle ---------------------------------------------------------------- */

/* Start the GB-Link control thread: run SYS_INITs, start static threads, then
 * loop Control::executeMode() forever. Returns once the thread is running. */
void gblink_start(void);

#ifdef __cplusplus
}
#endif

#endif /* GBAGATE_GBLINK_PORT_H */
