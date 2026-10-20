/*
 * gblink_frame.h -- the GB-Link serial framing, for CDC-ACM and UART.
 *
 *     | 0x47 0x42 | channel:1 | len:2 LE | payload[len] |
 *        'G' 'B'    0 command, 1 data, 2 status
 *
 * WebUSB has endpoints to keep a command, a data packet and a status word
 * apart; a byte stream does not, so every GB-Link host that talks over a
 * serial port (WebSerial in the browser apps, the ESP32 Switch bridge over
 * UART) wraps each would-be USB transfer in one of these frames. One frame is
 * one transfer: the receiver hands each payload to the same handler the USB
 * endpoint would have reached.
 *
 * The decoder matches upstream's serialLayer.cpp state for state, including
 * the two details that look like accidents and are not:
 *   - a 'G' while waiting for 'B' stays in the "seen G" state, so "GGB..."
 *     still syncs;
 *   - an over-long length resyncs immediately instead of swallowing that many
 *     bytes, so one corrupt header costs one frame, not the next 64 KB.
 *
 * Channels above 2 are not GB-Link's. gbagate uses 3 for its own link
 * protocol (see GBLINK_CH_GBAGATE) so both can share one port; a GB-Link
 * device ignores the channel, and a GB-Link host never sends it.
 */
#ifndef GBLINK_FRAME_H
#define GBLINK_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBLINK_SYNC0 0x47u
#define GBLINK_SYNC1 0x42u

#define GBLINK_CH_COMMAND 0u
#define GBLINK_CH_DATA    1u
#define GBLINK_CH_STATUS  2u
/* gbagate extension: link_proto packets (see link_proto.h), unframed. */
#define GBLINK_CH_GBAGATE 3u

/* Upstream's limit for the adapter. The ESP32 bridge's own console allows
 * 128; nothing a GB-Link host sends to an adapter exceeds 64. */
#define GBLINK_FRAME_MAX_PAYLOAD 64u
#define GBLINK_FRAME_HEADER      5u
#define GBLINK_FRAME_MAX         (GBLINK_FRAME_HEADER + GBLINK_FRAME_MAX_PAYLOAD)

/* Encode one frame. Returns bytes written, or 0 if the payload is too long
 * or cap too small. */
size_t gblink_frame_encode(uint8_t channel, const uint8_t *payload, size_t len,
                           uint8_t *out, size_t cap);

typedef enum {
    GBLINK_RX_SYNC0 = 0,
    GBLINK_RX_SYNC1,
    GBLINK_RX_CHANNEL,
    GBLINK_RX_LEN_LO,
    GBLINK_RX_LEN_HI,
    GBLINK_RX_PAYLOAD
} gblink_rx_state_t;

typedef struct {
    gblink_rx_state_t state;
    uint8_t  channel;
    uint16_t len;
    uint16_t pos;
    uint8_t  buf[GBLINK_FRAME_MAX_PAYLOAD];
    uint32_t frames;
    uint32_t oversize;   /* headers rejected for length */
} gblink_rx_t;

typedef struct {
    uint8_t        channel;
    const uint8_t *payload; /* points into the decoder; valid until next feed */
    uint16_t       len;
} gblink_frame_t;

void gblink_rx_init(gblink_rx_t *rx);

/* Feed one byte. Returns true when *out holds a complete frame. */
bool gblink_rx_feed(gblink_rx_t *rx, uint8_t byte, gblink_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GBLINK_FRAME_H */
