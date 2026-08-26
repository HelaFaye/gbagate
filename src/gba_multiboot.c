/*
 * gba_multiboot.c -- see gba_multiboot.h
 */

#include <string.h>

#include "gba_multiboot.h"

#define SYNC_REQ      0x00006202u
#define SYNC_ACK      0x72026202u
#define RECOGNITION   0x00006102u
#define HEADER_DONE   0x00006200u
#define KEY_REQUEST   0x000063D1u
#define CRC_POLL_REQ  0x00000065u
#define CRC_POLL_ACK  0x00750065u
#define CRC_SEND_REQ  0x00000066u

#define DEFAULT_SYNC_ATTEMPTS 5000u

uint32_t mb_crc_step(uint32_t crc, uint32_t word) {
    for (int bit = 0; bit < 32; bit++) {
        if ((crc ^ word) & 1u) {
            crc = (crc >> 1) ^ MB_CRC_POLY;
        } else {
            crc >>= 1;
        }
        word >>= 1;
    }
    return crc;
}

size_t mb_padded_size(size_t size) {
    /* The payload after the header is transferred in 16-byte groups. */
    size_t payload = size - MB_HEADER;
    payload = (payload + 0x0F) & ~(size_t)0x0F;
    return payload + MB_HEADER;
}

void mb_init(mb_session_t *s, mb_xfer_fn xfer, void *ctx) {
    memset(s, 0, sizeof *s);
    s->xfer          = xfer;
    s->ctx           = ctx;
    s->sync_attempts = DEFAULT_SYNC_ATTEMPTS;
}

static uint32_t xfer(mb_session_t *s, uint32_t word) {
    s->words_sent++;
    return s->xfer(s->ctx, word);
}

/* Read a 16-bit little-endian halfword; the header goes out as halfwords. */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

mb_result_t mb_send(mb_session_t *s, const uint8_t *rom, size_t size) {
    if (!s || !s->xfer || !rom) {
        return MB_ERR_ARGS;
    }
    if (size < MB_MIN_SIZE || size > MB_MAX_SIZE) {
        return MB_ERR_ARGS;
    }

    size_t padded = mb_padded_size(size);
    if (padded > MB_MAX_SIZE) {
        return MB_ERR_ARGS;
    }

    s->words_sent = 0;
    s->sync_tries = 0;

    /* ---- 1. wait for the BIOS ---------------------------------------- */

    bool synced = false;
    for (uint32_t i = 0; i < s->sync_attempts; i++) {
        s->sync_tries++;
        if (xfer(s, SYNC_REQ) == SYNC_ACK) {
            synced = true;
            break;
        }
    }
    if (!synced) {
        return MB_ERR_NO_GBA;
    }

    xfer(s, SYNC_REQ);
    xfer(s, RECOGNITION);

    /* ---- 2. header, in the clear, as 96 halfwords -------------------- */

    uint32_t offset = 0; /* bytes accepted by the GBA so far */
    for (size_t i = 0; i < MB_HEADER; i += 2) {
        xfer(s, rd16(rom + i));
        offset += 2;
    }

    xfer(s, HEADER_DONE);
    xfer(s, SYNC_REQ);

    /* ---- 3. key exchange --------------------------------------------- */
    /*
     * The GBA answers the second 0x63D1 with a byte of its own choosing in
     * bits 16..23. Both sides derive the session key from it, so it must be
     * read from the *second* exchange -- the first primes the BIOS.
     */
    xfer(s, KEY_REQUEST);
    uint32_t r = xfer(s, KEY_REQUEST);

    uint32_t client = (r & 0x00FF0000u) >> 16;
    s->client_data = (uint16_t)client;
    if (client == 0 || client == 0xFF) {
        /* Neither is a legal client byte; the GBA is not really talking. */
        return MB_ERR_HANDSHAKE;
    }

    /*
     * GBATEK builds both magic words as four packed bytes, least significant
     * first (see "Pseudo Code for SWI 25h", m=dword(pp,cc,cc,cc)):
     *
     *   m = pp | cc1<<8 | cc2<<16 | cc3<<24    pp = palette byte we just sent
     *   f = hh | rr1<<8 | rr2<<16 | rr3<<24    rr = each slave's random byte
     *
     * Only one slave sits on a Normal-mode link, and GBATEK specifies that the
     * client and random bytes of absent slaves read back as FFh -- hence the
     * constant 0xFFFF high half on both.
     *
     * hh is handshake_data = 11h + client_data[1] + client_data[2] +
     * client_data[3] = cc + 0x20F, and it occupies a single byte (it goes out
     * as the low half of 64hh). It MUST be masked to 8 bits before it is
     * packed: otherwise the carry out of byte 0 lands in the rr byte, and f --
     * which is folded into the final checksum -- is wrong for every client
     * byte above F0h. The transfer then completes and fails the CRC with no
     * indication of the cause, which is exactly the failure this file's header
     * comment warns about.
     */
    uint32_t m = 0xFFFF0000u | (client << 8) | (KEY_REQUEST & 0xFFu);
    uint32_t h = (client + 0x0Fu) & 0xFFu;

    r = xfer(s, h | 0x00006400u);

    uint32_t payload_words = (uint32_t)((padded - 0x190u) / 4u);
    r = xfer(s, payload_words);

    uint32_t f = 0xFFFF0000u | ((r & 0x00FF0000u) >> 8) | h;
    uint32_t crc = MB_CRC_SEED;

    /* ---- 4. encrypted payload ---------------------------------------- */

    if (s->progress) {
        s->progress(s->ctx, MB_HEADER, (uint32_t)padded);
    }

    for (size_t pos = MB_HEADER; pos < padded; pos += 4) {
        /*
         * Words past the caller's buffer are zero: the protocol transfers in
         * 16-byte groups and the ROM is rarely a multiple of that.
         */
        uint32_t w = 0;
        if (pos + 4 <= size) {
            w = rd32(rom + pos);
        } else if (pos < size) {
            uint8_t tail[4] = { 0, 0, 0, 0 };
            memcpy(tail, rom + pos, size - pos);
            w = rd32(tail);
        }

        crc = mb_crc_step(crc, w);
        m   = mb_key_next(m);

        uint32_t enc = w ^ mb_word_mask(m, MB_LOAD_ADDR + offset);
        uint32_t got = xfer(s, enc);

        /*
         * The GBA echoes the offset it has just accepted in the high half.
         * Checking it turns a silent corruption into an immediate, located
         * failure -- without it, the only symptom is a CRC mismatch after the
         * whole ROM has gone out.
         */
        if (got != 0xFFFFFFFFu && (got >> 16) != (offset & 0xFFFFu)) {
            return MB_ERR_STALLED;
        }

        offset += 4;

        if (s->progress && (offset & 0xFFFu) == 0) {
            s->progress(s->ctx, MB_HEADER + offset, (uint32_t)padded);
        }
    }

    /* ---- 5. final checksum ------------------------------------------- */

    crc = mb_crc_step(crc, f);
    s->crc_expected = (uint16_t)crc;

    bool ready = false;
    for (uint32_t i = 0; i < s->sync_attempts; i++) {
        if (xfer(s, CRC_POLL_REQ) == CRC_POLL_ACK) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        return MB_ERR_STALLED;
    }

    xfer(s, CRC_SEND_REQ);
    r = xfer(s, crc);
    s->crc_received = (uint16_t)(r >> 16);

    if ((r & 0xFFu) != 0x66u || (r >> 16) != (crc & 0xFFFFu)) {
        return MB_ERR_CRC;
    }

    if (s->progress) {
        s->progress(s->ctx, (uint32_t)padded, (uint32_t)padded);
    }
    return MB_OK;
}

const char *mb_result_name(mb_result_t r) {
    switch (r) {
    case MB_OK:            return "ok";
    case MB_ERR_ARGS:      return "bad arguments";
    case MB_ERR_NO_GBA:    return "no GBA responding";
    case MB_ERR_HANDSHAKE: return "handshake failed";
    case MB_ERR_STALLED:   return "transfer stalled";
    case MB_ERR_CRC:       return "checksum mismatch";
    default:               return "?";
    }
}
