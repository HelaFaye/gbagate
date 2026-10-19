/*
 * gblink_frame.c -- see gblink_frame.h.
 */
#include "gblink_frame.h"

#include <string.h>

size_t gblink_frame_encode(uint8_t channel, const uint8_t *payload, size_t len,
                           uint8_t *out, size_t cap) {
    if (len > GBLINK_FRAME_MAX_PAYLOAD || cap < GBLINK_FRAME_HEADER + len) {
        return 0;
    }
    if (len > 0 && payload == NULL) {
        return 0;
    }
    out[0] = GBLINK_SYNC0;
    out[1] = GBLINK_SYNC1;
    out[2] = channel;
    out[3] = (uint8_t)(len & 0xFFu);
    out[4] = (uint8_t)(len >> 8);
    if (len > 0) {
        memcpy(&out[GBLINK_FRAME_HEADER], payload, len);
    }
    return GBLINK_FRAME_HEADER + len;
}

void gblink_rx_init(gblink_rx_t *rx) {
    memset(rx, 0, sizeof *rx);
    rx->state = GBLINK_RX_SYNC0;
}

static bool complete(gblink_rx_t *rx, gblink_frame_t *out) {
    out->channel = rx->channel;
    out->payload = rx->buf;
    out->len     = rx->len;
    rx->frames++;
    rx->state = GBLINK_RX_SYNC0;
    return true;
}

bool gblink_rx_feed(gblink_rx_t *rx, uint8_t b, gblink_frame_t *out) {
    switch (rx->state) {
    case GBLINK_RX_SYNC0:
        if (b == GBLINK_SYNC0) rx->state = GBLINK_RX_SYNC1;
        return false;

    case GBLINK_RX_SYNC1:
        if (b == GBLINK_SYNC1)      rx->state = GBLINK_RX_CHANNEL;
        else if (b == GBLINK_SYNC0) rx->state = GBLINK_RX_SYNC1;
        else                        rx->state = GBLINK_RX_SYNC0;
        return false;

    case GBLINK_RX_CHANNEL:
        rx->channel = b;
        rx->state = GBLINK_RX_LEN_LO;
        return false;

    case GBLINK_RX_LEN_LO:
        rx->len = b;
        rx->state = GBLINK_RX_LEN_HI;
        return false;

    case GBLINK_RX_LEN_HI:
        rx->len |= (uint16_t)((uint16_t)b << 8);
        if (rx->len > GBLINK_FRAME_MAX_PAYLOAD) {
            rx->oversize++;
            rx->state = GBLINK_RX_SYNC0;
            return false;
        }
        rx->pos = 0;
        if (rx->len == 0) {
            return complete(rx, out);
        }
        rx->state = GBLINK_RX_PAYLOAD;
        return false;

    case GBLINK_RX_PAYLOAD:
        rx->buf[rx->pos++] = b;
        if (rx->pos >= rx->len) {
            return complete(rx, out);
        }
        return false;
    }
    rx->state = GBLINK_RX_SYNC0;
    return false;
}
