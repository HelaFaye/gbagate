/*
 * gba_multiboot.h -- send a multiboot program to a GBA over Normal 32-bit SIO.
 *
 * The GBA BIOS contains a download routine that runs at power-on with no
 * cartridge inserted, so any master that speaks this protocol can put a program
 * into the console's 256 KB EWRAM and start it. That gives the device a "Send
 * Program" mode with no flashcart involved.
 *
 * Transport is injected: one function that exchanges a 32-bit word and returns
 * what came back. That keeps the protocol -- which is fiddly, stateful and full
 * of magic constants -- testable against a simulated GBA on a host, and leaves
 * only the SPI plumbing to hardware.
 *
 * Provenance
 * ----------
 * Constants and sequence follow shinyquagsire23/stlink_gba_multiboot, which is
 * a working implementation, cross-read against GBATEK's "BIOS Multi Boot".
 * Getting any of them wrong produces a transfer that completes and then fails
 * the final CRC with no indication of which step was wrong, so they are quoted
 * here rather than paraphrased:
 *
 *   session key step   m = 0x6F646573 * m + 1
 *   word mask          w ^ (~(0x02000000 + offset) + 1) ^ m ^ 0x43202F2F
 *   CRC polynomial     0xC37B, seeded 0xC387
 *
 * Note the mask includes the destination address, so a word encrypted for the
 * wrong offset decrypts to garbage even with the right key.
 */

#ifndef GBA_MULTIBOOT_H
#define GBA_MULTIBOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EWRAM is 256 KB and the program is loaded at 0x02000000. */
#define MB_MAX_SIZE   0x40000
#define MB_LOAD_ADDR  0x02000000
#define MB_HEADER     0xC0    /* cartridge header, sent in the clear */
#define MB_MIN_SIZE   0x1A0   /* header plus enough payload to be meaningful */

#define MB_KEY_MULT   0x6F646573u
#define MB_KEY_XOR    0x43202F2Fu
#define MB_CRC_POLY   0x0000C37Bu
#define MB_CRC_SEED   0x0000C387u

typedef enum {
    MB_OK = 0,
    MB_ERR_ARGS,        /* null pointer, bad size, not 16-byte alignable */
    MB_ERR_NO_GBA,      /* never saw the BIOS sync word                  */
    MB_ERR_HANDSHAKE,   /* sync worked, key exchange did not             */
    MB_ERR_STALLED,     /* the GBA stopped acknowledging our offsets     */
    MB_ERR_CRC          /* everything sent, final checksum disagreed     */
} mb_result_t;

/*
 * Exchange one 32-bit word. Returns what the GBA clocked back during the same
 * transfer, since Normal mode is full duplex.
 */
typedef uint32_t (*mb_xfer_fn)(void *ctx, uint32_t word);

/* Optional progress hook: bytes sent, total. Safe to leave NULL. */
typedef void (*mb_progress_fn)(void *ctx, uint32_t sent, uint32_t total);

typedef struct {
    mb_xfer_fn     xfer;
    mb_progress_fn progress;
    void          *ctx;

    /* How many sync attempts before giving up. At ~10 us per try, 5000 is
     * about 50 ms, which is long enough to catch a console being switched on
     * and short enough that a missing cable reports quickly. */
    uint32_t sync_attempts;

    /* observability */
    uint32_t words_sent;
    uint32_t sync_tries;
    uint16_t client_data;   /* the byte the GBA picked during the handshake */
    uint16_t crc_expected;
    uint16_t crc_received;
} mb_session_t;

void mb_init(mb_session_t *s, mb_xfer_fn xfer, void *ctx);

/*
 * Send a program. rom must include the 192-byte header; size is rounded up to
 * a 16-byte boundary internally, so the caller's buffer must have room for the
 * padding or supply an already-aligned size.
 */
mb_result_t mb_send(mb_session_t *s, const uint8_t *rom, size_t size);

const char *mb_result_name(mb_result_t r);

/* ---- exposed for tests and for a GBA-side decoder ---------------------- */

/* One step of the session key. */
static inline uint32_t mb_key_next(uint32_t m) {
    return MB_KEY_MULT * m + 1u;
}

/* Mask for the word destined for `addr`, given the stepped key. */
static inline uint32_t mb_word_mask(uint32_t m, uint32_t addr) {
    return (~addr + 1u) ^ m ^ MB_KEY_XOR;
}

/* Fold one 32-bit word into the running CRC. */
uint32_t mb_crc_step(uint32_t crc, uint32_t word);

/* Round a ROM size up to what the protocol requires. */
size_t mb_padded_size(size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GBA_MULTIBOOT_H */
