/*
 * printer_mode.c -- see printer_mode.h.
 */
#include "printer_mode.h"

#include <stdio.h>
#include <string.h>


void printer_mode_init(printer_mode_t *m, const printer_io_t *io, uint32_t now_ms) {
    memset(m, 0, sizeof *m);
    gbp_init(&m->gbp);
    m->io = *io;
    m->last_ms = now_ms;
}

static void save(printer_mode_t *m) {
    const size_t n = gbp_png(&m->gbp, m->png, sizeof m->png);
    char name[GBP_NAME_MAX + 8] = "";
    if (n && m->io.save && m->io.save(m->io.ctx, m->png, n, name, sizeof name)) {
        m->saves++;
        snprintf(m->note, sizeof m->note, "saved %s", name);
    } else {
        m->save_failures++;
        snprintf(m->note, sizeof m->note, "not saved: no drive");
    }
}

bool printer_mode_tick(printer_mode_t *m, uint32_t now_ms) {
    bool changed = false;
    uint8_t chunk[256];
    size_t got;
    while (m->io.read && (got = m->io.read(m->io.ctx, chunk, sizeof chunk)) > 0) {
        const int rows = gbp_rows(&m->gbp);
        const uint32_t committed = m->gbp.committed;
        for (size_t i = 0; i < got; i++) {
            gbp_feed_stream(&m->gbp, chunk + i, 1);
            if (m->gbp.pictures != m->saved) {   /* saved before a next one can start */
                m->saved = m->gbp.pictures;
                save(m);
                changed = true;
            }
        }
        m->last_ms = now_ms;
        changed |= rows != gbp_rows(&m->gbp) || committed != m->gbp.committed;
    }
    /* a game that prints without ever feeding the paper out */
    if (!m->gbp.finished && m->gbp.committed && (uint32_t)(now_ms - m->last_ms) >= PRINTER_IDLE_MS) {
        changed |= gbp_finish(&m->gbp);
    }
    if (m->gbp.pictures != m->saved) {
        m->saved = m->gbp.pictures;
        save(m);
        changed = true;
    }
    return changed;
}
