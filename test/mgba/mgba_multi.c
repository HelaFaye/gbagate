/*
 * mgba_multi.c -- up to four GBAs (libmgba cores) on one Multi-Player link
 * cable, for test/mgba (Python, through ctypes).
 *
 * Each core runs in its own thread. They advance in lockstep: an mGBA timing
 * event every QUANTUM cycles parks the core at a barrier until all of them
 * have reached the same emulated time. The cable is modelled only at those
 * boundaries, by the last core to arrive, while every core is parked -- so
 * nothing on the bus races with a running core.
 *
 * The cable, as GBATEK "SIO Multi-Player Mode" gives it:
 *   - SIOCNT bit 2 (SI-Terminal): 0 on the parent (unit 0, whose SI is
 *     grounded), 1 on each child; bit 3 (SD-Terminal): 1 when every unit is
 *     in Multi-Player mode; bits 4-5, the ID, set when a transfer completes.
 *   - The parent sets Start; every unit's Busy is set and SIOMULTI0-3 read
 *     FFFFh; each unit's SIOMLT_SEND goes out in turn; at the end all units
 *     hold the same SIOMULTI0-3 (FFFFh for units that are not there), Busy
 *     clears, and each unit with IRQ enabled takes the serial interrupt.
 *   - A transfer takes mGBA's own cycle counts for the baud rate and the
 *     number of units (GBASIOCyclesPerTransfer).
 *
 * Built without NDEBUG: Ubuntu's libmgba keeps the debug fields in its
 * structs (mLockstepInit stores transferId at 0x58), and the layouts must
 * match.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#define MAXC    4
#define W       240
#define H       160
#define QUANTUM 1024                      /* cycles, ~61 us */

static const int k_cycles[4][MAXC] = {     /* mGBA's GBASIOCyclesPerTransfer */
    { 38326, 73003, 107680, 142356 },
    { 9582, 18251, 26920, 35589 },
    { 6388, 12167, 17947, 23726 },
    { 3194, 6075, 8973, 11863 },
};

struct unit {
    struct mCore *core;
    color_t fb[W * H];
    pthread_t thread;
    int frames;
    struct mTimingEvent ev;
    struct GBASIODriver drv;
    int idx;
};

static struct unit U[MAXC];
static int N;
static bool g_bus;

/* the barrier */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_active, g_arrived;
static unsigned g_gen;

/* the cable */
static bool     g_start_req, g_busy;
static int      g_quanta_left;
static uint16_t g_words[MAXC];
static uint32_t g_transfers;
static uint16_t g_last[MAXC];
#define TRACE_MAX 65536
static uint16_t g_trace[TRACE_MAX][MAXC];   /* every transfer's four words, in order */
static uint32_t g_trace_q[TRACE_MAX];       /* ... and the quantum each one started in */
static uint32_t g_quantum, g_start_q;

static void quiet(struct mLogger *l, int c, enum mLogLevel lv, const char *f, va_list a) {
    (void)l; (void)c; (void)lv; (void)f; (void)a;
}
static struct mLogger g_log = { .log = quiet };

static struct GBA *gba(int i) { return (struct GBA *)U[i].core->board; }
static uint8_t g_plugged = 0xF;           /* the cores on the shared cable (m_bus_plug) */
static bool on_cable(int i) { return (g_plugged >> i) & 1; }
static bool in_multi(int i) { return gba(i)->sio.mode == SIO_MULTI; }

/* Bring every unit's SI/SD bits up to date, start or finish a transfer.
 * Called with every core parked. */
static void bus_step(void) {
    g_quantum++;
    bool all = true;
    for (int i = 0; i < N; i++) if (on_cable(i)) all = all && in_multi(i);
    for (int i = 0; i < N; i++) {
        if (!on_cable(i) || !in_multi(i)) continue;
        uint16_t *cnt = &gba(i)->sio.siocnt;
        *cnt = (uint16_t)((*cnt & ~0x000C) | (i ? 0x0004 : 0) | (all ? 0x0008 : 0));
    }
    if (g_start_req && !g_busy) {
        g_start_req = false;
        int baud = gba(0)->sio.siocnt & 3, units = 0;
        for (int i = 0; i < N; i++) {
            const bool here = on_cable(i) && in_multi(i);
            if (here) units++;
            g_words[i] = here ? gba(i)->memory.io[REG_SIOMLT_SEND >> 1] : 0xFFFF;
        }
        for (int i = N; i < MAXC; i++) g_words[i] = 0xFFFF;
        for (int i = 0; i < N; i++) {
            if (!on_cable(i) || !in_multi(i)) continue;
            gba(i)->sio.siocnt |= 0x0080;
            for (int k = 0; k < MAXC; k++) gba(i)->memory.io[(REG_SIOMULTI0 >> 1) + k] = 0xFFFF;
        }
        g_busy = true;
        g_start_q = g_quantum;
        g_quanta_left = (k_cycles[baud][units ? units - 1 : 0] + QUANTUM - 1) / QUANTUM;
    } else if (g_busy && --g_quanta_left <= 0) {
        g_busy = false;
        if (g_transfers < TRACE_MAX) {
            memcpy(g_trace[g_transfers], g_words, sizeof g_words);
            g_trace_q[g_transfers] = g_start_q;
        }
        g_transfers++;
        memcpy(g_last, g_words, sizeof g_last);
        for (int i = 0; i < N; i++) {
            if (!on_cable(i) || !in_multi(i)) continue;
            struct GBA *g = gba(i);
            for (int k = 0; k < MAXC; k++) g->memory.io[(REG_SIOMULTI0 >> 1) + k] = g_words[k];
            g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~(0x0080 | 0x0030 | 0x0040)) | (i << 4));
            if (g->sio.siocnt & 0x4000) GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        }
    }
}

/* ---- relay mode: each core on its own cable, the other slots answered by
 * an mp_relay (src/mp_relay.c), the relays passing commands to each other
 * through a delay line, as boards would through a four-player session ---- */

#include "../../src/mp_relay.h"

static bool       g_relay;
static mp_relay_t R[MAXC];
static struct cable {
    bool     start_req, busy;
    int      quanta_left;
    uint32_t next_q;            /* a child's cable: when the relay clocks next */
    uint16_t words[MAXC];
    uint32_t transfers;
} K[MAXC];
static uint32_t g_delay_q;      /* the network, one way, in quanta           */
static bool     g_started, g_connect_sent;
static uint32_t g_connect_at;
#define NET_LINE_MAX 1024
static struct { uint32_t due; uint8_t from; uint16_t cmd[MP_CMD_WORDS]; } g_line[NET_LINE_MAX];
static int g_line_n;

static void relay_cable(int i) {
    struct GBA *g = gba(i);
    struct cable *k = &K[i];
    if (!in_multi(i)) return;
    g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~0x000C) | (i ? 0x0004 : 0) | 0x0008);
    const bool clock = (i == 0) ? k->start_req : (g_quantum >= k->next_q);
    if (clock && !k->busy) {
        k->start_req = false;
        uint16_t out[MAXC];
        mp_tx(&R[i], out);
        for (int s = 0; s < MAXC; s++) k->words[s] = out[s];
        k->words[i] = g->memory.io[REG_SIOMLT_SEND >> 1];
        g->sio.siocnt |= 0x0080;
        for (int s = 0; s < MAXC; s++) g->memory.io[(REG_SIOMULTI0 >> 1) + s] = 0xFFFF;
        k->busy = true;
        k->quanta_left = (k_cycles[g->sio.siocnt & 3][N - 1] + QUANTUM - 1) / QUANTUM;
        if (i == 0) g_start_q = g_quantum;
    } else if (k->busy && --k->quanta_left <= 0) {
        k->busy = false;
        for (int s = 0; s < MAXC; s++) g->memory.io[(REG_SIOMULTI0 >> 1) + s] = k->words[s];
        g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~(0x0080 | 0x0030 | 0x0040)) | (i << 4));
        if (g->sio.siocnt & 0x4000) GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        mp_done(&R[i], k->words);
        k->transfers++;
        if (i == 0) {
            if (g_transfers < TRACE_MAX) {
                memcpy(g_trace[g_transfers], k->words, sizeof k->words);
                g_trace_q[g_transfers] = g_start_q;
            }
            g_transfers++;
            memcpy(g_last, k->words, sizeof g_last);
        }
        /* the parent's cadence, as measured on four Emeralds (link4.py) */
        k->next_q = g_quantum + (R[i].phase == MP_HANDSHAKE ? 275 : R[i].phase == MP_FRAMES && R[i].pos == 0 ? 66 : 26);
    }
}

static void relay_step(void) {
    g_quantum++;
    for (int i = 0; i < N; i++) relay_cable(i);
    /* each relay's local commands into the line, due a network's delay later */
    uint16_t cmd[MP_CMD_WORDS];
    uint8_t from;
    for (int i = 0; i < N; i++)
        while (g_line_n < NET_LINE_MAX && mp_pop(&R[i], &from, cmd)) {
            g_line[g_line_n].due = g_quantum + g_delay_q;
            g_line[g_line_n].from = from;
            memcpy(g_line[g_line_n].cmd, cmd, sizeof cmd);
            g_line_n++;
        }
    int keep = 0;
    for (int n = 0; n < g_line_n; n++) {
        if (g_line[n].due <= g_quantum) {
            for (int j = 0; j < N; j++) if (j != g_line[n].from) mp_push(&R[j], g_line[n].from, g_line[n].cmd);
        } else {
            g_line[keep++] = g_line[n];
        }
    }
    g_line_n = keep;
    /* the server: StartHandshake once every GBA is handshaking; ConnectLink
     * to the children's boards once the parent's cable has connected */
    if (!g_started) {
        bool all = true;
        for (int i = 0; i < N; i++) all = all && R[i].local_ready;
        if (all) { for (int i = 0; i < N; i++) mp_start_handshake(&R[i]); g_started = true; }
    }
    if (R[0].connected && !g_connect_sent) { g_connect_sent = true; g_connect_at = g_quantum + 2 * g_delay_q; }
    if (g_connect_sent && g_connect_at && g_quantum >= g_connect_at) {
        for (int i = 1; i < N; i++) mp_connect(&R[i]);
        g_connect_at = 0;
    }
}

/* ---- board mode: boards with one to three GBAs each, the real board code
 * between them and the real Celio-Server ------------------------------------
 *
 * Each board has one cable with its GBAs on it, in port order, and runs the
 * board's own Celio client in its multi mode (src/celio.c) and the
 * Multi-Play glue (src/mp_online.c), whose relay (src/mp_relay.c through
 * src/mp_link.c) answers every slot that is not one of its GBAs. The bus is
 * modelled per transfer (all four slots' words), as on the shared cable
 * above; test/test_mpbus.c checks the electrical layer under it. The
 * WebSocket to the server is the caller's (link4_online.py): it hands the
 * frames in and takes them out. Everything here runs with g_mx held: in the
 * step, with every core parked, or in the caller's m_board_* calls. */

#include <time.h>

#include "../../src/celio.h"
#include "../../src/mp_online.h"

#define WS_OUT_MAX 256

static bool g_boards;
static int  NB;
static struct board {
    int             ncores, core[3];       /* in port order               */
    celio_t         celio;
    mp_online_t     mpo;
    mp_bus_layout_t layout;
    mp_link_t      *link;
    bool            bus_on, started;
    struct cable    k;
    uint32_t        gap_us;                /* as parent: the wait after this transfer */
    char            out[WS_OUT_MAX][CELIO_FRAME_MAX];
    uint16_t        out_len[WS_OUT_MAX];
    int             out_head, out_n;
    uint32_t        out_lost;
} BD[MAXC];
static int g_core_board[MAXC], g_core_port[MAXC];

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool b_ws_send(void *ctx, const char *t, size_t n) {
    struct board *b = ctx;
    if (b->out_n >= WS_OUT_MAX || n >= CELIO_FRAME_MAX) { b->out_lost++; return false; }
    const int at = (b->out_head + b->out_n) % WS_OUT_MAX;
    memcpy(b->out[at], t, n);
    b->out_len[at] = (uint16_t)n;
    b->out_n++;
    return true;
}
static uint32_t b_rand(void *ctx) { (void)ctx; return (uint32_t)random() ^ ((uint32_t)random() << 16); }
static void b_mp_command(void *ctx, uint8_t c, int id) {
    struct board *b = ctx;
    mp_online_command(&b->mpo, b->celio.players, c, id);
}
static void b_mp_data(void *ctx, uint8_t from, const uint8_t d[64]) { mp_online_data(&((struct board *)ctx)->mpo, from, d); }
static bool b_bus_start(void *ctx, const mp_bus_layout_t *l, mp_link_t *link) {
    struct board *b = ctx;
    b->layout = *l;
    b->link = link;
    memset(&b->k, 0, sizeof b->k);
    b->bus_on = true;
    return true;
}
static void b_bus_stop(void *ctx) { ((struct board *)ctx)->bus_on = false; }
static void b_status(void *ctx, uint16_t st) { celio_on_status(&((struct board *)ctx)->celio, st, now_ms()); }
static void b_data_out(void *ctx, const uint8_t d[64]) { celio_on_data(&((struct board *)ctx)->celio, d, 64, now_ms()); }

static int slot_of(const struct board *b, int port) { return b->layout.first + port - 1; }

/* One board's cable, one quantum: as relay_cable, with several GBAs on it
 * and the relay answering the rest. */
static void board_cable(struct board *b) {
    if (!b->bus_on) return;
    struct cable *k = &b->k;
    bool all = true;
    for (int p = 0; p < b->ncores; p++) all = all && in_multi(b->core[p]);
    if (!all) return;
    for (int p = 0; p < b->ncores; p++) {
        struct GBA *g = gba(b->core[p]);
        g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~0x000C) | (slot_of(b, p + 1) ? 0x0004 : 0) | 0x0008);
    }
    const bool local_parent = b->layout.first == 0;
    const bool clock = local_parent ? k->start_req : (g_quantum >= k->next_q);
    if (clock && !k->busy) {
        k->start_req = false;
        uint16_t out[MAXC];
        uint32_t gap_us = 0;
        mp_link_next(b->link, out, &gap_us);
        b->gap_us = gap_us;
        for (int s = 0; s < MAXC; s++) k->words[s] = s < b->layout.players ? out[s] : 0xFFFF;
        for (int p = 0; p < b->ncores; p++) k->words[slot_of(b, p + 1)] = gba(b->core[p])->memory.io[REG_SIOMLT_SEND >> 1];
        for (int p = 0; p < b->ncores; p++) {
            struct GBA *g = gba(b->core[p]);
            g->sio.siocnt |= 0x0080;
            for (int s = 0; s < MAXC; s++) g->memory.io[(REG_SIOMULTI0 >> 1) + s] = 0xFFFF;
        }
        k->busy = true;
        const int baud = gba(b->core[0])->sio.siocnt & 3;
        k->quanta_left = (k_cycles[baud][b->layout.players - 1] + QUANTUM - 1) / QUANTUM;
    } else if (k->busy && --k->quanta_left <= 0) {
        k->busy = false;
        for (int p = 0; p < b->ncores; p++) {
            struct GBA *g = gba(b->core[p]);
            for (int s = 0; s < MAXC; s++) g->memory.io[(REG_SIOMULTI0 >> 1) + s] = k->words[s];
            g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~(0x0080 | 0x0030 | 0x0040)) | (slot_of(b, p + 1) << 4));
            if (g->sio.siocnt & 0x4000) GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        }
        mp_link_done(b->link, k->words);
        k->transfers++;
        k->next_q = g_quantum + (b->gap_us * 1000u + 61035u - 1) / 61035u;   /* as parent: the cadence */
        if (b == &BD[0]) {
            if (g_transfers < TRACE_MAX) { memcpy(g_trace[g_transfers], k->words, sizeof k->words); g_trace_q[g_transfers] = g_quantum; }
            g_transfers++;
            memcpy(g_last, k->words, sizeof g_last);
        }
    }
}

static void boards_step(void) {
    g_quantum++;
    const uint32_t now = now_ms();
    for (int i = 0; i < NB; i++) {
        struct board *b = &BD[i];
        board_cable(b);
        if (!b->started) continue;
        celio_tick(&b->celio, now);
        if (b->celio.multi) mp_online_tick(&b->mpo);
    }
}

static void arrive(bool leaving) {
    pthread_mutex_lock(&g_mx);
    if (leaving) g_active--;
    else g_arrived++;
    if (g_arrived >= g_active) {
        if (g_bus && g_active > 0) { if (g_boards) boards_step(); else if (g_relay) relay_step(); else bus_step(); }
        g_arrived = 0;
        g_gen++;
        pthread_cond_broadcast(&g_cv);
    } else if (!leaving) {
        const unsigned gen = g_gen;
        while (gen == g_gen) pthread_cond_wait(&g_cv, &g_mx);
    }
    pthread_mutex_unlock(&g_mx);
}

static void quantum_cb(struct mTiming *t, void *ctx, uint32_t late) {
    struct unit *u = ctx;
    mTimingSchedule(t, &u->ev, QUANTUM - (int32_t)late);
    arrive(false);
}

/* SIOCNT writes in Multi-Player mode. The parent's Start asks for a
 * transfer; on a child, Start is read-only. Busy reads back set while the
 * cable is busy. */
static uint16_t multi_write(struct GBASIODriver *d, uint32_t address, uint16_t value) {
    struct unit *u = (struct unit *)((char *)d - offsetof(struct unit, drv));
    if (address != REG_SIOCNT) return value;
    pthread_mutex_lock(&g_mx);
    const uint16_t keep = gba(u->idx)->sio.siocnt & (0x0004 | 0x0008 | 0x0030);
    bool busy;
    if (g_boards) {
        struct board *b = &BD[g_core_board[u->idx]];
        struct cable *k = &b->k;
        const bool parent = b->bus_on && b->layout.first == 0 && g_core_port[u->idx] == 1;
        if (parent && (value & 0x0080) && !k->busy) k->start_req = true;
        busy = k->busy || (parent && k->start_req);
    } else if (g_relay) {
        struct cable *k = &K[u->idx];
        if (u->idx == 0 && (value & 0x0080) && !k->busy) k->start_req = true;
        busy = k->busy || (u->idx == 0 && k->start_req);
    } else {
        if (u->idx == 0 && (value & 0x0080) && !g_busy) g_start_req = true;
        busy = g_busy || (u->idx == 0 && g_start_req);
    }
    value = (uint16_t)((value & ~(0x0080 | 0x000C | 0x0030)) | keep | (busy ? 0x0080 : 0));
    pthread_mutex_unlock(&g_mx);
    return value;
}

static void *run_thread(void *arg) {
    struct unit *u = arg;
    for (int f = 0; f < u->frames; f++) u->core->runFrame(u->core);
    arrive(true);
    return NULL;
}

int m_open(int n, const char *rom) {
    mLogSetDefaultLogger(&g_log);
    if (n < 1 || n > MAXC) return -1;
    N = n;
    for (int i = 0; i < n; i++) {
        struct unit *u = &U[i];
        u->idx = i;
        u->core = GBACoreCreate();
        if (!u->core || !u->core->init(u->core)) return -2;
        mCoreInitConfig(u->core, NULL);
        u->core->setVideoBuffer(u->core, u->fb, W);
        if (!mCoreLoadFile(u->core, rom)) return -3;
        u->core->reset(u->core);
    }
    return 0;
}

/* Plug the cable in (after any state loads: a load rebuilds the timing
 * queue). */
void m_bus_attach(void) {
    for (int i = 0; i < N; i++) {
        struct unit *u = &U[i];
        memset(&u->drv, 0, sizeof u->drv);
        u->drv.writeRegister = multi_write;
        GBASIOSetDriver(&gba(i)->sio, &u->drv, SIO_MULTI);
        u->ev = (struct mTimingEvent){ .context = u, .callback = quantum_cb, .name = "multi quantum", .priority = 0x90 };
        mTimingDeschedule(&gba(i)->timing, &u->ev);
        mTimingSchedule(&gba(i)->timing, &u->ev, QUANTUM);
    }
    g_bus = true;
}

/* Run every core `frames` frames, in lockstep. */
void m_run(int frames) {
    g_active = N;
    g_arrived = 0;
    for (int i = 0; i < N; i++) {
        U[i].frames = frames;
        pthread_create(&U[i].thread, NULL, run_thread, &U[i]);
    }
    for (int i = 0; i < N; i++) pthread_join(U[i].thread, NULL);
}

void m_keys(int i, uint32_t k) { U[i].core->setKeys(U[i].core, k); }
uint32_t m_r8(int i, uint32_t a)  { return U[i].core->busRead8(U[i].core, a); }
uint32_t m_r16(int i, uint32_t a) { return U[i].core->busRead16(U[i].core, a); }
uint32_t m_r32(int i, uint32_t a) { return U[i].core->busRead32(U[i].core, a); }
void m_w8(int i, uint32_t a, uint8_t v)   { U[i].core->busWrite8(U[i].core, a, v); }
void m_w16(int i, uint32_t a, uint16_t v) { U[i].core->busWrite16(U[i].core, a, v); }
void m_w32(int i, uint32_t a, uint32_t v) { U[i].core->busWrite32(U[i].core, a, v); }
uint32_t m_frame(int i) { return U[i].core->frameCounter(U[i].core); }

void m_screen(int i, uint8_t *rgb) {
    for (int p = 0; p < W * H; p++) {
        color_t c = U[i].fb[p];
        rgb[p * 3] = (uint8_t)c; rgb[p * 3 + 1] = (uint8_t)(c >> 8); rgb[p * 3 + 2] = (uint8_t)(c >> 16);
    }
}

size_t m_state_size(int i) { return U[i].core->stateSize(U[i].core); }
int m_save_state(int i, void *b) { return U[i].core->saveState(U[i].core, b) ? 0 : -1; }
int m_load_state(int i, const void *b) { return U[i].core->loadState(U[i].core, b) ? 0 : -1; }

/* transfers completed, and the last four words */
uint32_t m_bus_transfers(uint16_t last[MAXC]) {
    memcpy(last, g_last, sizeof g_last);
    return g_transfers;
}

/* Copy up to max traced transfers (four words each) into out; returns how
 * many there are in all. */
uint32_t m_trace(uint16_t *out, uint32_t max) {
    const uint32_t n = g_transfers < TRACE_MAX ? g_transfers : TRACE_MAX;
    memcpy(out, g_trace, (n < max ? n : max) * sizeof g_trace[0]);
    return g_transfers;
}

uint32_t m_trace_quantum(uint32_t k) { return k < TRACE_MAX ? g_trace_q[k] : 0; }

/* Relay mode instead of one shared cable: before m_bus_attach. */
void m_relay_mode(uint32_t delay_quanta) {
    g_relay = true;
    g_delay_q = delay_quanta;
    for (int i = 0; i < N; i++) mp_init(&R[i], (uint8_t)N, (uint8_t)(1u << i));
}

/* A relay's counters: frames, relayed, received, idle_given, dropped,
 * bad_checksums, phase, connected, cable transfers. */
void m_relay_stats(int i, uint32_t out[9]) {
    out[0] = R[i].frames; out[1] = R[i].relayed; out[2] = R[i].received;
    out[3] = R[i].idle_given; out[4] = R[i].dropped; out[5] = R[i].bad_checksums;
    out[6] = R[i].phase; out[7] = R[i].connected; out[8] = K[i].transfers;
}

/* ---- board mode's API --------------------------------------------------- */

/* Boards instead of one cable, before m_bus_attach: gbas[b] GBAs on board b,
 * the cores taken in order (board 0's first). */
int m_board_mode(int nb, const int *gbas) {
    int c = 0;
    if (nb < 1 || nb > MAXC) return -1;
    memset(BD, 0, sizeof BD);
    for (int b = 0; b < nb; b++) {
        if (gbas[b] < 1 || gbas[b] > 3) return -1;
        BD[b].ncores = gbas[b];
        for (int p = 0; p < gbas[b]; p++) {
            if (c >= N) return -1;
            BD[b].core[p] = c;
            g_core_board[c] = b;
            g_core_port[c] = p + 1;
            c++;
        }
    }
    if (c != N) return -1;
    NB = nb;
    g_boards = true;
    return 0;
}

/* Start board b's Celio client: create (session "" ) or join. */
void m_board_start(int b, const char *session, int players) {
    pthread_mutex_lock(&g_mx);
    struct board *bd = &BD[b];
    const celio_io_t io = { b_ws_send, NULL, NULL, b_rand, bd, b_mp_command, b_mp_data };
    const mp_online_io_t mio = { b_bus_start, b_bus_stop, b_status, b_data_out, NULL, NULL, bd };
    mp_online_init(&bd->mpo, &mio, (uint8_t)players, (uint8_t)bd->ncores);
    celio_start_multi(&bd->celio, &io, session, (uint8_t)players, (uint8_t)bd->ncores, now_ms());
    bd->started = true;
    pthread_mutex_unlock(&g_mx);
}

void m_board_ws_open(int b) {
    pthread_mutex_lock(&g_mx);
    celio_on_open(&BD[b].celio, now_ms());
    pthread_mutex_unlock(&g_mx);
}
void m_board_ws_closed(int b) {
    pthread_mutex_lock(&g_mx);
    celio_on_close(&BD[b].celio, now_ms());
    pthread_mutex_unlock(&g_mx);
}
void m_board_ws_text(int b, const char *t, size_t n) {
    pthread_mutex_lock(&g_mx);
    celio_on_text(&BD[b].celio, t, n, now_ms());
    pthread_mutex_unlock(&g_mx);
}
/* The next frame board b sends, or 0 when there is none. */
size_t m_board_ws_take(int b, char *buf, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&g_mx);
    struct board *bd = &BD[b];
    if (bd->out_n) {
        n = bd->out_len[bd->out_head];
        if (n > cap) n = cap;
        memcpy(buf, bd->out[bd->out_head], n);
        bd->out_head = (bd->out_head + 1) % WS_OUT_MAX;
        bd->out_n--;
    }
    pthread_mutex_unlock(&g_mx);
    return n;
}

/* Board b: [celio state, players, multi, bus_on, first, locals, cable
 * transfers, relay frames, relayed, received, bad_checksums, dropped,
 * celio lost, bad packets, out_lost]; and its session code into sid. */
void m_board_view(int b, uint32_t out[15], char sid[40]) {
    pthread_mutex_lock(&g_mx);
    struct board *bd = &BD[b];
    const mp_relay_t *r = bd->link ? &bd->link->relay : NULL;
    out[0] = bd->celio.state; out[1] = bd->celio.players; out[2] = bd->celio.multi; out[3] = bd->bus_on;
    out[4] = bd->layout.first; out[5] = bd->layout.locals; out[6] = bd->k.transfers;
    out[7] = r ? r->frames : 0; out[8] = r ? r->relayed : 0; out[9] = r ? r->received : 0;
    out[10] = r ? r->bad_checksums : 0; out[11] = r ? r->dropped : 0;
    out[12] = bd->celio.dropped + bd->celio.given_up; out[13] = bd->mpo.bad_packets; out[14] = bd->out_lost;
    snprintf(sid, 40, "%s", bd->celio.session);
    pthread_mutex_unlock(&g_mx);
}

/* The slot core i's GBA has on the link, or -1 before its board has a role. */
int m_core_slot(int i) {
    const struct board *b = &BD[g_core_board[i]];
    return b->bus_on ? slot_of(b, g_core_port[i]) : -1;
}

/* Which cores are on the shared cable (bit i: core i); the rest are not
 * plugged in, and the cable is ready without them. Core 0 is the parent. */
void m_bus_plug(unsigned mask) {
    pthread_mutex_lock(&g_mx);
    g_plugged = (uint8_t)mask;
    pthread_mutex_unlock(&g_mx);
}
