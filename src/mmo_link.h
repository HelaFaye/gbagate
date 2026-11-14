/*
 * mmo_link.h -- the shared overworld's words between the GBA and the board
 * (doc/MMO.md).
 *
 * The in-game agent and the board exchange one 16-bit word each per link
 * transfer, both ways at once, as Multi-Play does: the same words whether
 * they travel the real link port (the PHY) or the emulated adapter's bridge.
 * Messages are short frames of words:
 *
 *     header   0xA5 << 8 | type << 4 | n      n payload words, 0..15
 *     payload  n words, none of them 0xFFFF
 *     check    (0x5A5A + header + payload words) & 0xFFFF, 0xFFFE if 0xFFFF
 *
 * Between frames either side sends MMO_IDLE. 0xFFFF is never sent: on the
 * bus it is what an empty slot reads as (link_proto.h, GBA_WORD_EMPTY), so a
 * missing partner can never pass for data. A frame that fails its check is
 * dropped and the reader looks for the next header.
 *
 * Messages:
 *   GBA -> board  MMO_STATE    where the player is, and the agent's mailbox
 *   board -> GBA  MMO_SPAWN    a remote player's figure, on a map
 *                 MMO_MOVE     a held movement for a figure (walk, face)
 *                 MMO_REMOVE   a figure goes
 */

#ifndef MMO_LINK_H
#define MMO_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MMO_IDLE      0x0000u
#define MMO_HDR       0xA500u
#define MMO_MAX_WORDS 15

typedef enum {
    MMO_STATE  = 1,
    MMO_SPAWN  = 3,
    MMO_MOVE   = 4,
    MMO_REMOVE = 5,
} mmo_type_t;

/*
 * MMO_STATE, from the agent: its player, and its mailbox, so the board sends
 * no more commands than the agent has room for.
 *   w0  map group << 8 | map num
 *   w1  x            w2  y          (map tiles, with the map's 7-tile offset)
 *   w3  facing (DIR_*, 1-4) | elevation << 4 | gender << 8
 *   w4  commands received, mod 0x8000
 *   w5  mailbox slots free
 *
 * MMO_SPAWN  lid, gfx, x, y, elevation, map group << 8 | map num
 * MMO_MOVE   lid, movement action (MOVEMENT_ACTION_*)
 * MMO_REMOVE lid
 */
typedef struct {
    uint8_t  type;
    uint8_t  n;
    uint16_t w[MMO_MAX_WORDS];
} mmo_msg_t;

/* ---- writing ------------------------------------------------------------ */

/* A frame's words into out (n + 2 of them). 0 if a payload word is 0xFFFF or
 * n is too large: such a message cannot be sent. */
int mmo_encode(const mmo_msg_t *m, uint16_t *out, int cap);

/* ---- reading ------------------------------------------------------------ */

typedef struct {
    mmo_msg_t msg;
    int       have;       /* words of the current frame so far, header included */
    uint16_t  sum;
    uint32_t  bad;        /* frames dropped: a failed check, a stray word      */
} mmo_rx_t;

void mmo_rx_init(mmo_rx_t *r);

/* One received word. True when it completed a frame, now in r->msg. */
bool mmo_rx_word(mmo_rx_t *r, uint16_t word);

#ifdef __cplusplus
}
#endif

#endif /* MMO_LINK_H */
