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

static void quiet(struct mLogger *l, int c, enum mLogLevel lv, const char *f, va_list a) {
    (void)l; (void)c; (void)lv; (void)f; (void)a;
}
static struct mLogger g_log = { .log = quiet };

static struct GBA *gba(int i) { return (struct GBA *)U[i].core->board; }
static bool in_multi(int i) { return gba(i)->sio.mode == SIO_MULTI; }

/* Bring every unit's SI/SD bits up to date, start or finish a transfer.
 * Called with every core parked. */
static void bus_step(void) {
    bool all = true;
    for (int i = 0; i < N; i++) all = all && in_multi(i);
    for (int i = 0; i < N; i++) {
        if (!in_multi(i)) continue;
        uint16_t *cnt = &gba(i)->sio.siocnt;
        *cnt = (uint16_t)((*cnt & ~0x000C) | (i ? 0x0004 : 0) | (all ? 0x0008 : 0));
    }
    if (g_start_req && !g_busy) {
        g_start_req = false;
        int baud = gba(0)->sio.siocnt & 3, units = 0;
        for (int i = 0; i < N; i++) {
            if (in_multi(i)) units++;
            g_words[i] = in_multi(i) ? gba(i)->memory.io[REG_SIOMLT_SEND >> 1] : 0xFFFF;
        }
        for (int i = N; i < MAXC; i++) g_words[i] = 0xFFFF;
        for (int i = 0; i < N; i++) {
            if (!in_multi(i)) continue;
            gba(i)->sio.siocnt |= 0x0080;
            for (int k = 0; k < MAXC; k++) gba(i)->memory.io[(REG_SIOMULTI0 >> 1) + k] = 0xFFFF;
        }
        g_busy = true;
        g_quanta_left = (k_cycles[baud][units ? units - 1 : 0] + QUANTUM - 1) / QUANTUM;
    } else if (g_busy && --g_quanta_left <= 0) {
        g_busy = false;
        g_transfers++;
        memcpy(g_last, g_words, sizeof g_last);
        for (int i = 0; i < N; i++) {
            if (!in_multi(i)) continue;
            struct GBA *g = gba(i);
            for (int k = 0; k < MAXC; k++) g->memory.io[(REG_SIOMULTI0 >> 1) + k] = g_words[k];
            g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~(0x0080 | 0x0030 | 0x0040)) | (i << 4));
            if (g->sio.siocnt & 0x4000) GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        }
    }
}

static void arrive(bool leaving) {
    pthread_mutex_lock(&g_mx);
    if (leaving) g_active--;
    else g_arrived++;
    if (g_arrived >= g_active) {
        if (g_bus && g_active > 0) bus_step();
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
    if (u->idx == 0 && (value & 0x0080) && !g_busy) g_start_req = true;
    value = (uint16_t)((value & ~(0x0080 | 0x000C | 0x0030)) | keep | ((g_busy || (u->idx == 0 && g_start_req)) ? 0x0080 : 0));
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
