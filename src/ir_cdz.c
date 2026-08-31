/*
 * ir_cdz.c -- see ir_cdz.h
 */

#include <string.h>

#include "ir_cdz.h"

cdz_pulse_t cdz_classify(uint32_t gap_us) {
    if (gap_us <= CDZ_MINI_MAX_US)   return CDZ_PULSE_MINI;
    if (gap_us <= CDZ_SHORT_MAX_US)  return CDZ_PULSE_SHORT;
    if (gap_us <= CDZ_MEDIUM_MAX_US) return CDZ_PULSE_MEDIUM;
    return CDZ_PULSE_LONG;
}

const char *cdz_pulse_name(cdz_pulse_t p) {
    switch (p) {
    case CDZ_PULSE_MINI:   return "mini";
    case CDZ_PULSE_SHORT:  return "short";
    case CDZ_PULSE_MEDIUM: return "medium";
    case CDZ_PULSE_LONG:   return "long";
    default:               return "?";
    }
}

void cdz_rx_init(cdz_rx_t *rx) {
    memset(rx, 0, sizeof *rx);
}

bool cdz_rx_edge(cdz_rx_t *rx, uint32_t now_us, uint16_t *cmd) {
    if (!rx->have_last) {
        /* Nothing to measure against yet; this edge only starts the clock. */
        rx->last_us   = now_us;
        rx->have_last = true;
        return false;
    }

    /* Unsigned subtraction, so a wrapped microsecond counter still gives the
     * right interval. */
    uint32_t gap = now_us - rx->last_us;
    rx->last_us = now_us;

    switch (cdz_classify(gap)) {
    case CDZ_PULSE_MINI:
        /*
         * Keep-alives sit between everything else and carry nothing. Counting
         * them as bits is the obvious way to get a decoder that almost works:
         * the bit count overruns and every command is discarded.
         */
        rx->minis++;
        return false;

    case CDZ_PULSE_SHORT:
    case CDZ_PULSE_MEDIUM: {
        bool one = (cdz_classify(gap) == CDZ_PULSE_MEDIUM);
        if (rx->count < CDZ_CMD_BITS) {
            rx->bits = (uint16_t)((rx->bits << 1) | (one ? 1u : 0u));
            rx->count++;
        } else {
            /* More than twelve before a terminator: the train is malformed,
             * but keep counting so the Long still resets us cleanly rather
             * than leaving a partial command to merge with the next one. */
            rx->count++;
        }
        return false;
    }

    case CDZ_PULSE_LONG:
    default: {
        bool ok = (rx->count == CDZ_CMD_BITS);
        uint16_t value = (uint16_t)(rx->bits & 0x0FFF);
        rx->bits  = 0;
        rx->count = 0;
        if (ok) {
            rx->commands++;
            if (cmd) {
                *cmd = value;
            }
            return true;
        }
        rx->aborted++;
        return false;
    }
    }
}

size_t cdz_encode(uint16_t cmd, uint32_t *gaps_us, size_t cap) {
    if (!gaps_us || cap < CDZ_CMD_BITS + 1u) {
        return 0;
    }
    /* MSB first, matching the decoder's shift-left assembly. */
    for (int i = 0; i < CDZ_CMD_BITS; i++) {
        bool one = (cmd >> (CDZ_CMD_BITS - 1 - i)) & 1u;
        gaps_us[i] = one ? CDZ_MEDIUM_US : CDZ_SHORT_US;
    }
    gaps_us[CDZ_CMD_BITS] = CDZ_LONG_US;
    return CDZ_CMD_BITS + 1u;
}
