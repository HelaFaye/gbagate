/*
 * mgba_shim.c -- libmgba (the distribution's libmgba-dev) behind a flat C
 * interface for test/mgba/ (Python) to load through ctypes: load a ROM, run
 * frames with keys held, read and write the bus, take a screenshot, keep
 * and restore states.
 *
 * The ROM is the user's own: these tests take its path from the command line
 * or EMERALD_ROM and never copy it anywhere.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>

#define W 240
#define H 160

static struct mCore *g_core;
static color_t g_fb[W * H];

/* mGBA logs every DMA and SWI at debug level; keep warnings and worse,
 * where a test can read them, only when GBA_LOG is set. */
static void quiet_log(struct mLogger *l, int cat, enum mLogLevel level, const char *fmt, va_list ap) {
    (void)l; (void)cat;
    if (getenv("GBA_LOG") && (level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
}
static struct mLogger g_log = { .log = quiet_log };

int t_open(const char *rom) {
    mLogSetDefaultLogger(&g_log);
    g_core = GBACoreCreate();
    if (!g_core || !g_core->init(g_core)) return -1;
    mCoreInitConfig(g_core, NULL);
    g_core->setVideoBuffer(g_core, g_fb, W);
    if (!mCoreLoadFile(g_core, rom)) return -2;
    /* No save file: a fresh cartridge. The HLE BIOS (no BIOS file). */
    g_core->reset(g_core);
    return 0;
}

void t_run(int frames, uint32_t keys) {
    g_core->setKeys(g_core, keys);
    for (int i = 0; i < frames; i++) g_core->runFrame(g_core);
}

uint32_t t_frame(void) { return g_core->frameCounter(g_core); }

uint32_t t_r8(uint32_t a)  { return g_core->busRead8(g_core, a); }
uint32_t t_r16(uint32_t a) { return g_core->busRead16(g_core, a); }
uint32_t t_r32(uint32_t a) { return g_core->busRead32(g_core, a); }
void t_w8(uint32_t a, uint8_t v)   { g_core->busWrite8(g_core, a, v); }
void t_w16(uint32_t a, uint16_t v) { g_core->busWrite16(g_core, a, v); }
void t_w32(uint32_t a, uint32_t v) { g_core->busWrite32(g_core, a, v); }

/* The screen as 240x160 RGB bytes. */
void t_screen(uint8_t *rgb) {
    for (int i = 0; i < W * H; i++) {
        color_t c = g_fb[i];
        rgb[i * 3 + 0] = (uint8_t)(c & 0xFF);
        rgb[i * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);
        rgb[i * 3 + 2] = (uint8_t)((c >> 16) & 0xFF);
    }
}

size_t t_state_size(void) { return g_core->stateSize(g_core); }
int t_save_state(void *buf) { return g_core->saveState(g_core, buf) ? 0 : -1; }
int t_load_state(const void *buf) { return g_core->loadState(g_core, buf) ? 0 : -1; }

/* ---- a Wireless Adapter on the link port ------------------------------------
 *
 * t_rfu_attach(path) plugs the simulator's adapter (gblink_sim with
 * GBLINK_RFU_WIRE=path, running the firmware's Wireless Adapter mode) into
 * this GBA's link port, through test/gblink_sim/rfu_wire.h. This side is the
 * GBA's serial hardware: Normal 32-bit transfers it clocks (GBATEK: 2 MHz,
 * 16 us a word) take the adapter's staged word; transfers it arms with the
 * external clock wait for the adapter's; SIOCNT bit 2 shows the adapter's SI.
 *
 * The adapter is firmware with real-time waits, so the emulator is paced to
 * real time: an event every SYNC_CYCLES of emulated time waits until the
 * wall clock has caught up, and brings the wires up to date both ways.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#include "../gblink_sim/rfu_wire.h"

#define GBA_HZ       16777216.0
#define SYNC_CYCLES  256                 /* ~15 us                             */
#define XFER_2MHZ    268                 /* 32 bits at 2 MHz, in cycles        */
#define XFER_256K    2097                /* ... at 256 kHz                     */

static struct rfu_wire *RW;
static struct GBASIODriver g_rfu_drv;
static struct mTimingEvent g_sync_ev, g_done_ev;
static uint32_t g_xfer_rx, g_deliver_rx;
static bool     g_deliver;
static int64_t  g_anchor_us = -1;
static double   g_emu_us;
static uint32_t g_late_us_max;
static uint32_t g_cmds[256];         /* adapter commands the game sent, by id */
static uint32_t g_ack_words_max[256]; /* the most reply words each ack carried */
static uint32_t g_scans, g_scan_hits;  /* 0x1D polls, and those that found a peer */

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static struct GBA *gba(void) { return (struct GBA *)g_core->board; }

/* SO and SD as the GBA drives them (under the wire's lock). */
static void lines_update(struct GBA *g, int64_t now) {
    const uint16_t rcnt = g->sio.rcnt;
    bool so, sd;
    if (rcnt & 0x8000) {                       /* general-purpose mode */
        so = (rcnt & 0x80) ? (rcnt & 0x08) != 0 : true;
        sd = (rcnt & 0x20) && (rcnt & 0x02);
    } else {
        so = (g->sio.siocnt & 0x0008) != 0;
        sd = false;
    }
    RW->gba_so = so;
    if (sd && !RW->sd_level) {
        RW->sd_rise_us = now;
        RW->sd_pulse_reported = false;
    } else if (!sd && RW->sd_level && !RW->sd_pulse_reported && now - RW->sd_rise_us >= 40) {
        RW->sd_pulse_pending = true;
    }
    RW->sd_level = sd;
}

static uint16_t with_si(uint16_t cnt) {
    return (uint16_t)((cnt & ~0x0004) | (rfu_wire_adapter_si(RW) ? 0x0004 : 0));
}

static void finish(struct GBA *g, uint32_t rx, uint32_t late) {
    g->memory.io[REG_SIODATA32_LO >> 1] = (uint16_t)rx;
    g->memory.io[(REG_SIODATA32_LO >> 1) + 1] = (uint16_t)(rx >> 16);
    g->sio.siocnt &= (uint16_t)~0x0080;
    const bool irq = (g->sio.siocnt & 0x4000) != 0;
    if (irq) GBARaiseIRQ(g, GBA_IRQ_SIO, late);
    RW->busy = false;
    RW->armed = false;
    RW->serial_owed = irq;
    g->sio.siocnt = with_si(g->sio.siocnt);
}

/* A transfer the GBA clocked has shifted its 32 bits. */
static void done_cb(struct mTiming *t, void *ctx, uint32_t late) {
    (void)t; (void)ctx;
    struct GBA *g = gba();
    rfu_wire_lock(RW);
    finish(g, g_xfer_rx, late);
    if (g_deliver) {
        RW->ring[RW->ring_head % RFU_WIRE_RING] = g_deliver_rx;
        RW->ring_head++;
        g_deliver = false;
    }
    rfu_wire_unlock(RW);
}

/* Every SYNC_CYCLES: the wires both ways, then wait for the wall clock. */
static void sync_cb(struct mTiming *t, void *ctx, uint32_t late) {
    (void)ctx;
    struct GBA *g = gba();
    const int64_t now = mono_us();
    g_emu_us += (SYNC_CYCLES + late) * 1e6 / GBA_HZ;
    rfu_wire_lock(RW);
    lines_update(g, now);
    if (RW->in_pending && now >= RW->in_at) {   /* the adapter's word is in */
        RW->in_pending = false;
        finish(g, RW->in_word, 0);
    }
    g->sio.siocnt = with_si(g->sio.siocnt);
    RW->gba_us = (int64_t)g_emu_us;
    rfu_wire_unlock(RW);

    if (g_anchor_us < 0) g_anchor_us = now - (int64_t)g_emu_us;
    int64_t target = g_anchor_us + (int64_t)g_emu_us;
    if (now > target + 20000) {                 /* fell behind: do not race */
        if ((uint32_t)(now - target) > g_late_us_max) g_late_us_max = (uint32_t)(now - target);
        g_anchor_us = now - (int64_t)g_emu_us;
    } else {
        while (mono_us() < target) { }
    }
    mTimingSchedule(t, &g_sync_ev, SYNC_CYCLES);
}

static uint16_t rfu_write(struct GBASIODriver *d, uint32_t address, uint16_t value) {
    (void)d;
    struct GBA *g = gba();
    if (address == REG_SIODATA32_LO || address == REG_SIODATA32_LO + 2) {
        rfu_wire_lock(RW);
        if (address == REG_SIODATA32_LO) RW->gba_tx = (RW->gba_tx & 0xFFFF0000u) | value;
        else RW->gba_tx = (RW->gba_tx & 0xFFFFu) | ((uint32_t)value << 16);
        rfu_wire_unlock(RW);
        return value;
    }
    if (address != REG_SIOCNT) return value;
    rfu_wire_lock(RW);
    RW->serial_owed = false;
    if (RW->busy) value |= 0x0080;              /* busy reads back set */
    g->sio.siocnt = value;                      /* for lines_update    */
    lines_update(g, mono_us());
    const bool start = (value & 0x0080) != 0, internal = (value & 0x0001) != 0;
    if (!start || internal) RW->armed = false;
    if (start && !RW->busy && (value & 0x3000) == 0x1000 && !(g->sio.rcnt & 0x8000)) {
        if (!internal) {
            RW->armed = true;                   /* waits for the adapter's clock */
        } else {
            const uint32_t tx = RW->gba_tx;
            if ((tx >> 16) == 0x9966u) g_cmds[tx & 0xFF]++;   /* GBATEK: 9966ppcch */
            if (RW->enabled && !RW->adapter_master && RW->staged_valid) {
                g_xfer_rx = RW->staged;
                if ((g_xfer_rx >> 16) == 0x9966u) {         /* an ack: 9966rraah */
                    const uint32_t aa = g_xfer_rx & 0xFF, rr = (g_xfer_rx >> 8) & 0xFF;
                    if (rr > g_ack_words_max[aa]) g_ack_words_max[aa] = rr;
                    if (aa == 0x9D) { g_scans++; if (rr >= 7) g_scan_hits++; }
                }
                RW->staged_valid = false;
                g_deliver = true;
                g_deliver_rx = tx;
                RW->gba_words++;
            } else {
                /* nothing staged: the GBA reads the level SI was left at */
                g_xfer_rx = rfu_wire_adapter_si(RW) ? 0xFFFFFFFFu : 0;
                if (RW->enabled && RW->adapter_master) RW->clash++; else RW->unstaged++;
            }
            RW->busy = true;
            mTimingDeschedule(&g->timing, &g_done_ev);
            mTimingSchedule(&g->timing, &g_done_ev, (value & 0x0002) ? XFER_2MHZ : XFER_256K);
        }
    }
    value = with_si(value);
    rfu_wire_unlock(RW);
    return value;
}

int t_rfu_attach(const char *path) {
    const int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof *RW) != 0) return -1;
    RW = mmap(NULL, sizeof *RW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (RW == MAP_FAILED) return -2;
    if (RW->magic != RFU_WIRE_MAGIC) {
        memset(RW, 0, sizeof *RW);
        atomic_flag_clear(&RW->lock);
        RW->gba_so = true;
        RW->version = RFU_WIRE_VERSION;
        RW->magic = RFU_WIRE_MAGIC;
    }
    struct GBA *g = gba();
    memset(&g_rfu_drv, 0, sizeof g_rfu_drv);
    g_rfu_drv.writeRegister = rfu_write;
    GBASIOSetDriver(&g->sio, &g_rfu_drv, SIO_NORMAL_32);
    g_sync_ev = (struct mTimingEvent){ .context = NULL, .callback = sync_cb, .name = "rfu wire sync", .priority = 0x90 };
    g_done_ev = (struct mTimingEvent){ .context = NULL, .callback = done_cb, .name = "rfu wire done", .priority = 0x91 };
    mTimingSchedule(&g->timing, &g_sync_ev, SYNC_CYCLES);
    return 0;
}

/* For tests: the wire's counters, and the most the emulator fell behind. */
void t_rfu_counters(uint32_t out[8]) {
    rfu_wire_lock(RW);
    out[0] = RW->gba_words; out[1] = RW->adapter_words; out[2] = RW->unstaged;
    out[3] = RW->unarmed; out[4] = RW->clash; out[5] = RW->late_arm;
    out[6] = RW->enabled; out[7] = g_late_us_max;
    rfu_wire_unlock(RW);
}

/* For tests: how many times the game sent adapter command id. */
uint32_t t_rfu_cmd_count(int id) { return g_cmds[id & 0xFF]; }
uint32_t t_rfu_ack_words_max(int aa) { return g_ack_words_max[aa & 0xFF]; }
uint32_t t_rfu_scans(int hits) { return hits ? g_scan_hits : g_scans; }

/* ---- a board on the Multi-Player cable ---------------------------------------
 *
 * t_mp_attach(path) puts the simulator's board (gblink_sim with
 * GBLINK_MP_WIRE=path, running the firmware's link mode) on this GBA's
 * cable, through test/gblink_sim/mp_wire.h. This side is the GBA's
 * Multi-Player hardware (GBATEK): the SI terminal says child when the board
 * drives the clock and parent when it does not; the SD terminal says ready
 * while the board's link mode is on; a transfer leaves SIOMULTI0-1 holding
 * the parent's and the child's words, 2-3 FFFF, sets the ID and raises the
 * serial IRQ. Paced to real time, as the adapter's firmware is.
 */
#include "../gblink_sim/mp_wire.h"

static const int k_mp_cycles[4] = { 73003, 18251, 12167, 6075 };   /* two units, per baud (mGBA) */

static struct mp_wire *MPW;
static struct GBASIODriver g_mp_drv;
static struct mTimingEvent g_mp_sync_ev, g_mp_done_ev;
static bool     g_mp_busy, g_mp_board_clocked;
static uint16_t g_mp_words[2];
static int64_t  g_mp_anchor = -1;
static double   g_mp_emu_us;
static uint32_t g_mp_late_max;

static bool mp_gba_multi(struct GBA *g) { return g->sio.mode == SIO_MULTI; }

/* SI terminal and SD terminal, from what the board is doing (under lock). */
static void mp_terminals(struct GBA *g) {
    if (!mp_gba_multi(g)) return;
    const bool child = MPW->mode == MP_WIRE_MASTER;
    const bool ready = MPW->mode != MP_WIRE_OFF;
    g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~0x000C) | (child ? 0x0004 : 0) | (ready ? 0x0008 : 0));
}

static void mp_begin(struct GBA *g, uint16_t parent, uint16_t child, bool board_clocked) {
    g_mp_words[0] = parent;
    g_mp_words[1] = child;
    g_mp_busy = true;
    g_mp_board_clocked = board_clocked;
    g->sio.siocnt |= 0x0080;
    for (int k = 0; k < 4; k++) g->memory.io[(REG_SIOMULTI0 >> 1) + k] = 0xFFFF;
    mTimingDeschedule(&g->timing, &g_mp_done_ev);
    mTimingSchedule(&g->timing, &g_mp_done_ev, k_mp_cycles[g->sio.siocnt & 3]);
}

static void mp_done_cb(struct mTiming *t, void *ctx, uint32_t late) {
    (void)t; (void)ctx;
    struct GBA *g = gba();
    mp_wire_lock(MPW);
    g->memory.io[(REG_SIOMULTI0 >> 1) + 0] = g_mp_words[0];
    g->memory.io[(REG_SIOMULTI0 >> 1) + 1] = g_mp_words[1];
    g->memory.io[(REG_SIOMULTI0 >> 1) + 2] = 0xFFFF;
    g->memory.io[(REG_SIOMULTI0 >> 1) + 3] = 0xFFFF;
    const int id = g_mp_board_clocked ? 1 : 0;
    g->sio.siocnt = (uint16_t)((g->sio.siocnt & ~(0x0080 | 0x0030 | 0x0040)) | (id << 4));
    if (g->sio.siocnt & 0x4000) GBARaiseIRQ(g, GBA_IRQ_SIO, late);
    if (g_mp_board_clocked) {                      /* the board's master word: the GBA's answer back */
        MPW->rx_word = g_mp_words[1];
        MPW->rx_ready = true;
    } else {                                       /* the GBA's parent word, for the board */
        MPW->ring[MPW->ring_head % MP_WIRE_RING] = g_mp_words[0];
        MPW->ring_head++;
    }
    MPW->transfers++;
    g_mp_busy = false;
    mp_terminals(g);
    mp_wire_unlock(MPW);
}

static void mp_sync_cb(struct mTiming *t, void *ctx, uint32_t late) {
    (void)ctx;
    struct GBA *g = gba();
    const int64_t now = mono_us();
    g_mp_emu_us += (SYNC_CYCLES + late) * 1e6 / GBA_HZ;
    mp_wire_lock(MPW);
    MPW->gba_multi = mp_gba_multi(g);
    mp_terminals(g);
    if (MPW->mode == MP_WIRE_MASTER && MPW->req_pending && !g_mp_busy) {
        MPW->req_pending = false;
        if (mp_gba_multi(g)) {
            mp_begin(g, MPW->req_word, g->memory.io[REG_SIOMLT_SEND >> 1], true);
        } else {                                   /* nobody on the cable in Multi-Player mode */
            MPW->rx_word = 0xFFFF;
            MPW->rx_ready = true;
            MPW->not_multi++;
        }
    }
    MPW->gba_us = (int64_t)g_mp_emu_us;
    mp_wire_unlock(MPW);

    if (g_mp_anchor < 0) g_mp_anchor = now - (int64_t)g_mp_emu_us;
    const int64_t target = g_mp_anchor + (int64_t)g_mp_emu_us;
    if (now > target + 20000) {
        if ((uint32_t)(now - target) > g_mp_late_max) g_mp_late_max = (uint32_t)(now - target);
        g_mp_anchor = now - (int64_t)g_mp_emu_us;
    } else {
        while (mono_us() < target) { }
    }
    mTimingSchedule(t, &g_mp_sync_ev, SYNC_CYCLES);
}

/* The GBA as parent: its Start runs a transfer with the board's staged word. */
static uint16_t mp_write(struct GBASIODriver *d, uint32_t address, uint16_t value) {
    (void)d;
    struct GBA *g = gba();
    if (address != REG_SIOCNT) return value;
    mp_wire_lock(MPW);
    const uint16_t keep = g->sio.siocnt & (0x0004 | 0x0008 | 0x0030);
    const bool parent = MPW->mode != MP_WIRE_MASTER;
    if (parent && (value & 0x0080) && !g_mp_busy && MPW->mode == MP_WIRE_SLAVE && !MPW->staged_valid) {
        /* A real board stages its answer before the parent's next word (the
         * PHY asks for it right after the last one). Here the emulator can
         * run ahead of real time in a burst while catching up, faster than
         * the simulator's thread stages: give it the moment a real board
         * would have had, up to 5 ms, rather than answer FFFF. */
        const int64_t give_up = mono_us() + 5000;
        do {
            mp_wire_unlock(MPW);
            mp_wire_lock(MPW);
        } while (!MPW->staged_valid && MPW->mode == MP_WIRE_SLAVE && mono_us() < give_up);
    }
    if (parent && (value & 0x0080) && !g_mp_busy) {
        uint16_t answer = 0xFFFF;                  /* nobody answered */
        if (MPW->mode == MP_WIRE_SLAVE && MPW->staged_valid) {
            answer = MPW->staged;
            MPW->staged_valid = false;
        } else {
            MPW->unanswered++;
        }
        mp_begin(g, g->memory.io[REG_SIOMLT_SEND >> 1], answer, false);
    }
    value = (uint16_t)((value & ~(0x0080 | 0x000C | 0x0030)) | keep | (g_mp_busy ? 0x0080 : 0));
    mp_wire_unlock(MPW);
    return value;
}

int t_mp_attach(const char *path) {
    const int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof *MPW) != 0) return -1;
    MPW = mmap(NULL, sizeof *MPW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (MPW == MAP_FAILED) return -2;
    if (MPW->magic != MP_WIRE_MAGIC) {
        memset(MPW, 0, sizeof *MPW);
        atomic_flag_clear(&MPW->lock);
        MPW->version = MP_WIRE_VERSION;
        MPW->magic = MP_WIRE_MAGIC;
    }
    struct GBA *g = gba();
    memset(&g_mp_drv, 0, sizeof g_mp_drv);
    g_mp_drv.writeRegister = mp_write;
    GBASIOSetDriver(&g->sio, &g_mp_drv, SIO_MULTI);
    g_mp_sync_ev = (struct mTimingEvent){ .context = NULL, .callback = mp_sync_cb, .name = "mp wire sync", .priority = 0x90 };
    g_mp_done_ev = (struct mTimingEvent){ .context = NULL, .callback = mp_done_cb, .name = "mp wire done", .priority = 0x91 };
    mTimingSchedule(&g->timing, &g_mp_sync_ev, SYNC_CYCLES);
    return 0;
}

/* transfers, unanswered, not in Multi-Player mode, the most the emulator
 * fell behind (us), the board's mode, the cable busy */
void t_mp_counters(uint32_t out[6]) {
    mp_wire_lock(MPW);
    out[0] = MPW->transfers; out[1] = MPW->unanswered; out[2] = MPW->not_multi; out[3] = g_mp_late_max;
    out[4] = MPW->mode; out[5] = g_mp_busy;
    mp_wire_unlock(MPW);
}
