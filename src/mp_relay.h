/*
 * mp_relay.h -- a GBA's Multi-Player link, with the other players far away.
 *
 * A board on a Multi-Player cable with up to three GBAs on it answers for
 * the units that are not there -- remote players, reached through the
 * network -- and relays commands both ways (doc/MULTIPLAY.md). The board
 * offers the four-player relay only once three GBAs answer on its cable
 * (mp_detect), the board making the fourth.
 * This is that relay's logic, with no I/O: the cable driver asks it for the
 * words to put in each slot it answers (mp_tx) before every transfer and
 * gives it every slot's word after (mp_done); the network side feeds it the
 * other players' commands (mp_push) and takes the local GBA's (mp_pop).
 *
 * What the games say on the cable (from four Emeralds on a modelled cable,
 * test/mgba/link4.py, and pokeemerald's link.c):
 *
 *   handshake   every unit B9A0; the parent's 8FFF ends it
 *   one more    a checksum slot no one checks (B9A0 from the children)
 *   frames      8 command words from each player, then a checksum word:
 *               every unit sends the 16-bit sum of all players' 8 words,
 *               and every GBA checks each player's against its own sum
 *
 * Idle is eight zeros. Frames line up on this cable only: a remote player's
 * command is taken from its queue at a frame start on this cable, and the
 * checksum is the sum of the frame as it is here, so the local GBA's own
 * check passes. The games keep each player's commands apart, which is what
 * makes that safe. Idle frames of the local GBA are not relayed, so a
 * faster cable does not fill the others' queues.
 */
#ifndef MP_RELAY_H
#define MP_RELAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MAX_PLAYERS 4
#define MP_CMD_WORDS   8
#define MP_QUEUE       64      /* commands held per remote player */

#define MP_SLAVE_HANDSHAKE  0xB9A0u
#define MP_MASTER_HANDSHAKE 0x8FFFu

typedef enum { MP_HANDSHAKE, MP_CHECKSUM_SLOT, MP_FRAMES } mp_phase_t;

typedef struct {
    uint16_t cmd[MP_QUEUE][MP_CMD_WORDS];
    uint8_t  head, count;
} mp_queue_t;

typedef struct {
    uint8_t    players;         /* on the link, 2..4                        */
    uint8_t    local_mask;      /* slots with a GBA on this board's cable    */
    uint8_t    local;           /* the lowest of them (one-GBA boards)      */
    mp_phase_t phase;
    uint8_t    pos;             /* the word of the frame, 0..8              */

    bool       started;         /* answer B9A0 for the remote players       */
    bool       connect;         /* (a child's board) send 8FFF in slot 0    */
    bool       connected;       /* slot 0 sent 8FFF: frames follow          */
    bool       local_ready;     /* every local GBA's last word was a handshake */
    uint8_t    seen_mask;       /* slots that answered the last handshake    */
    uint8_t    ready_rounds;    /* handshake transfers in a row with every
                                   slot answering, before 8FFF goes out     */

    uint16_t   sum;             /* this frame's words, every player         */
    uint16_t   cur[MP_MAX_PLAYERS][MP_CMD_WORDS];  /* remote commands now   */
    uint16_t   mine[MP_MAX_PLAYERS][MP_CMD_WORDS]; /* each local GBA's      */

    mp_queue_t in[MP_MAX_PLAYERS];   /* from each remote player             */
    mp_queue_t out;                  /* the local GBAs', for the network     */
    uint8_t    out_from[MP_QUEUE];   /* ... whose each one is                */

    /* counters */
    uint32_t   frames, relayed, received, idle_given, dropped, bad_checksums;
} mp_relay_t;

/* A link of `players`; local_mask has a bit for each slot with a GBA on
 * this board's cable (1 << id). */
void mp_init(mp_relay_t *r, uint8_t players, uint8_t local_mask);

/* The slots that answered the last handshake transfer. Before the session
 * starts the board answers nothing, so this is the GBAs on its cable:
 * offer the four-player relay only when three are there. */
uint8_t mp_detect(const mp_relay_t *r);
int     mp_detect_count(const mp_relay_t *r);

/* Before a transfer: the word for every slot this board answers (every slot
 * < players not in local_mask; FFFF past players). Local slots are left
 * alone. */
void mp_tx(mp_relay_t *r, uint16_t out[MP_MAX_PLAYERS]);

/* After a transfer: every slot's word, as on the cable. */
void mp_done(mp_relay_t *r, const uint16_t words[MP_MAX_PLAYERS]);

/* The session is ready: every player's GBA has a handshake (the server's
 * StartHandshake). Until then the remote slots answer FFFF: nobody there. */
void mp_start_handshake(mp_relay_t *r);

/* The parent is remote and has connected (the server's ConnectLink): send
 * 8FFF in slot 0 once the local GBAs are counting every player. Nothing to
 * do when a local GBA is the parent: it connects the link itself. */
void mp_connect(mp_relay_t *r);

/* Network side. A remote player's command; false if its queue is full
 * (counted in dropped). */
bool mp_push(mp_relay_t *r, uint8_t from, const uint16_t cmd[MP_CMD_WORDS]);
/* A local GBA's next command for the network, and its link ID in *from;
 * false if none. */
bool mp_pop(mp_relay_t *r, uint8_t *from, uint16_t cmd[MP_CMD_WORDS]);

#ifdef __cplusplus
}
#endif

#endif
