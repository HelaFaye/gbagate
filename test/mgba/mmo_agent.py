"""mmo_agent.py -- the in-game agent of the shared overworld, speaking the
board's link words (src/mmo_link.h), for the tests that run Emerald through
the emulated adapter.

The agent is a game Task, as overworld_sync.py's stand-in is (the game's own
RunTasks calls it every frame). Each frame it:
  - decodes the words the board sent (MMO_SPAWN, MMO_MOVE, MMO_REMOVE) into
    its mailbox, dropping a spawn meant for another map;
  - runs the mailbox with the game's own functions, a walk holding back what
    follows it (SpawnSpecialObjectEventParameterized, ObjectEventSetHeldMovement,
    RemoveObjectEvent);
  - empties the mailbox when its player changes map (the game has dropped
    every figure);
  - says MMO_STATE when its player, its count of commands or its free slots
    change.

Its words go through two rings in free EWRAM. What moves them to and from
the board is the test (adapter_link() below), standing in for the GBA's
serial hardware: the agent's own SIO driver, sharing the port with the
game's link code (doc/MMO.md, step 1), is to come.

Layout, in the 4 KB of free EWRAM (emerald.FREE_EWRAM):
  +0x100 the agent's code       +0xD00 the mailbox (overworld_sync.py's)
  +0xD80 TX ring: head, tail, 32 words (the agent writes, the link reads)
  +0xF00 RX ring: head, tail, 32 words (the link writes, the agent reads)
  +0xF48 the frame decoder      +0xF70 counters and what was last said
"""
import emerald as E

AGENT = E.FREE_EWRAM + 0x100
AGENT_MAX = 0x700
MAILBOX = E.FREE_EWRAM + 0xD00
TXRING = E.FREE_EWRAM + 0xD80
RXRING = E.FREE_EWRAM + 0xF00
DECODER = E.FREE_EWRAM + 0xF48
COUNTS = E.FREE_EWRAM + 0xF70
RING = 32
IDLE = 0x0000

# COUNTS: received, overflow, bad, last map, said once, then the 6 words said
C_RECEIVED, C_OVERFLOW, C_BAD, C_MAP, C_SAID = 0, 1, 2, 3, 4

SRC = r'''
typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef void (*fn)(void);
typedef u8 (*spawn_fn)(u8, u8, u8, s16, s16, u8);
typedef u8 (*id_fn)(u8, u8, u8);
typedef u8 (*u8obj_fn)(void *);
typedef u8 (*set_fn)(void *, u8);
typedef void (*rm_fn)(void *);
#define MB ((volatile u16 *)MAILBOX)
#define TX ((volatile u16 *)TXRING)
#define RX ((volatile u16 *)RXRING)
#define D  ((volatile u16 *)DECODER)       /* have, n, type, sum, w[15] */
#define C  ((volatile u16 *)COUNTS)        /* received, overflow, bad, map, said, said[6] */
#define NMB 8

static void tx(u16 w) {
    u16 t = TX[1];
    TX[2 + (t & 31)] = w;
    TX[1] = (u16)(t + 1);
}

static int tx_room(void) { return 32 - (u16)(TX[1] - TX[0]); }

/* A frame of the board's, whole: into the mailbox, as the stand-in's commands. */
static void command(u16 type, u16 group, u16 num) {
    C[0] = (u16)((C[0] + 1) & 0x7FFF);
    int n = MB[0];
    if (n >= NMB) { C[1]++; return; }
    volatile u16 *c = MB + 1 + 6 * n;
    if (type == 3) {                                  /* MMO_SPAWN */
        if (D[4 + 5] != (u16)(group << 8 | num)) return;   /* for another map */
        c[0] = 1; c[1] = D[4]; c[2] = D[5]; c[3] = D[6]; c[4] = D[7]; c[5] = D[8];
    } else if (type == 4) {                           /* MMO_MOVE */
        c[0] = 2; c[1] = D[4]; c[2] = D[5]; c[3] = c[4] = c[5] = 0;
    } else if (type == 5) {                           /* MMO_REMOVE */
        c[0] = 3; c[1] = D[4]; c[2] = c[3] = c[4] = c[5] = 0;
    } else {
        return;
    }
    MB[0] = (u16)(n + 1);
}

/* One word from the board (src/mmo_link.c, the same rules). */
static void hear(u16 w, u16 group, u16 num) {
    if (D[0] == 0) {
        if (w == 0 || w == 0xFFFF) return;
        if ((w & 0xFF00) != 0xA500 || (w & 0xF0) == 0) { C[2]++; return; }
        D[2] = (w >> 4) & 15; D[1] = w & 15; D[3] = (u16)(0x5A5A + w); D[0] = 1;
        return;
    }
    if (w == 0xFFFF) { C[2]++; D[0] = 0; return; }
    if (D[0] <= D[1]) {
        D[4 + D[0] - 1] = w; D[3] = (u16)(D[3] + w); D[0]++;
        return;
    }
    D[0] = 0;
    u16 sum = D[3] == 0xFFFF ? 0xFFFE : D[3];
    if (w != sum) {
        C[2]++;
        if ((w & 0xFF00) == 0xA500 && (w & 0xF0)) hear(w, group, num);
        return;
    }
    command(D[2], group, num);
}

void entry(u8 task) {
    (void)task;
    const u8 *sb = *(u8 **)SYM_gSaveBlock1Ptr;
    const u8 group = sb[4], num = sb[5];
    const u16 map = (u16)(group << 8 | num);

    if (C[3] != map) {                     /* a new map: the game dropped every figure */
        C[3] = map;
        MB[0] = 0;
        D[0] = 0;
    }

    while (RX[0] != RX[1]) {               /* the board's words */
        u16 h = RX[0];
        hear(RX[2 + (h & 31)], group, num);
        RX[0] = (u16)(h + 1);
    }

    /* The mailbox, as overworld_sync.py's stand-in runs it. */
    int n = MB[0], k = 0;
    for (; k < n; k++) {
        volatile u16 *c = MB + 1 + 6 * k;
        const u8 lid = (u8)c[1];
        const u8 id = ((id_fn)(SYM_GetObjectEventIdByLocalIdAndMap | 1))(lid, num, group);
        u8 *obj = (u8 *)SYM_gObjectEvents + 0x24 * id;
        if (c[0] == 1) {
            if (id == 16)
                ((spawn_fn)(SYM_SpawnSpecialObjectEventParameterized | 1))((u8)c[2], 0, lid, (s16)c[3], (s16)c[4], (u8)c[5]);
        } else if (c[0] == 2) {
            if (id == 16) continue;
            const u8 st = ((u8obj_fn)(SYM_ObjectEventClearHeldMovementIfFinished | 1))(obj);
            if (st == 0) break;
            ((set_fn)(SYM_ObjectEventSetHeldMovement | 1))(obj, (u8)c[2]);
        } else if (c[0] == 3) {
            if (id != 16) ((rm_fn)(SYM_RemoveObjectEvent | 1))(obj);
        }
    }
    for (int i = k; i < n; i++)
        for (int w = 0; w < 6; w++) MB[1 + 6 * (i - k) + w] = MB[1 + 6 * i + w];
    MB[0] = (u16)(n - k);

    /* MMO_STATE, when anything in it changed. */
    const u8 *pa = (const u8 *)SYM_gPlayerAvatar;
    const u8 *ob = (const u8 *)SYM_gObjectEvents + 0x24 * pa[5];
    u16 s[6];
    s[0] = map;
    s[1] = *(const u16 *)(ob + 0x10);
    s[2] = *(const u16 *)(ob + 0x12);
    s[3] = (u16)((ob[0x18] & 15) | (ob[0x0B] & 15) << 4 | (pa[7] & 1) << 8);
    s[4] = C[0];
    s[5] = (u16)(NMB - MB[0]);
    int same = C[4] != 0;
    for (int i = 0; i < 6; i++) same = same && C[5 + i] == s[i];
    if (!same && tx_room() >= 8) {
        u16 hdr = 0xA500 | 1 << 4 | 6, sum = (u16)(0x5A5A + hdr);
        tx(hdr);
        for (int i = 0; i < 6; i++) { tx(s[i]); sum = (u16)(sum + s[i]); C[5 + i] = s[i]; }
        tx(sum == 0xFFFF ? 0xFFFE : sum);
        C[4] = 1;
    }
}
'''


def build(sym):
    code = E.build_thumb(SRC, AGENT, sym, {'MAILBOX': hex(MAILBOX), 'TXRING': hex(TXRING),
                                           'RXRING': hex(RXRING), 'DECODER': hex(DECODER),
                                           'COUNTS': hex(COUNTS)})
    assert len(code) <= AGENT_MAX, 'the agent outgrew its room (%d bytes)' % len(code)
    return code


def install(core, code):
    """The agent's code, and its rings and counters cleared."""
    core.write(AGENT, code)
    core.write(MAILBOX, bytes(2 + 12 * 8))
    core.write(TXRING, bytes(4 + 2 * RING))
    core.write(RXRING, bytes(4 + 2 * RING))
    core.write(DECODER, bytes(0x28))
    core.write(COUNTS, bytes(0x20))


def tx_pop(core):
    """The agent's next word for the board, or IDLE."""
    h, t = core.r16(TXRING), core.r16(TXRING + 2)
    if h == t:
        return IDLE
    w = core.r16(TXRING + 4 + 2 * (h & (RING - 1)))
    core.w16(TXRING, (h + 1) & 0xFFFF)
    return w


def rx_push(core, w):
    """A word from the board, for the agent. False if its ring is full."""
    h, t = core.r16(RXRING), core.r16(RXRING + 2)
    if ((t - h) & 0xFFFF) >= RING:
        return False
    core.w16(RXRING + 4 + 2 * (t & (RING - 1)), w)
    core.w16(RXRING + 2, (t + 1) & 0xFFFF)
    return True


def counts(core):
    return {'received': core.r16(COUNTS), 'overflow': core.r16(COUNTS + 2), 'bad': core.r16(COUNTS + 4)}
