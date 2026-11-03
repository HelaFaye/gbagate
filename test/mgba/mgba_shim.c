/*
 * mgba_shim.c -- libmgba (the distribution's libmgba-dev) behind a flat C
 * interface for test/mgba/*.py to load through ctypes: load a ROM, run
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
