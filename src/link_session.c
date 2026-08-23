/*
 * link_session.c -- see link_session.h
 */

#include <string.h>

#include "link_session.h"

/* Signed distance from a to b in 16-bit sequence space. */
static int16_t seq_delta(uint16_t b, uint16_t a) {
    return (int16_t)(uint16_t)(b - a);
}

void ls_init(link_session_t *ls, uint32_t stall_budget_us) {
    memset(ls, 0, sizeof *ls);
    ls->stall_budget_us = stall_budget_us ? stall_budget_us : LS_DEFAULT_STALL_US;
}

void ls_reset(link_session_t *ls, uint16_t seq) {
    ls->seq = seq;
    memset(ls->have, 0, sizeof ls->have);
    memset(ls->word, 0, sizeof ls->word);
}

bool ls_push_word(link_session_t *ls, uint16_t seq, uint16_t word) {
    int16_t d = seq_delta(seq, ls->seq);

    if (d < 0) {
        /* The peer is behind us: this word is for a transfer that already went
         * out. Nothing to do but count it -- persistent lateness here is the
         * signal to raise the stall budget or drop to a faster transport. */
        ls->late++;
        return false;
    }
    if (d >= LS_DEPTH) {
        /* The peer is running further ahead than the window allows. Dropping
         * is correct: accepting it would alias onto a slot still in use. */
        return false;
    }

    unsigned i = (unsigned)(seq & LS_MASK);
    if (ls->have[i]) {
        ls->dupes++;
        return false;
    }
    ls->word[i] = word;
    ls->have[i] = true;
    return true;
}

bool ls_ready(const link_session_t *ls) {
    return ls->have[ls->seq & LS_MASK];
}

uint16_t ls_take_word(link_session_t *ls, uint32_t waited_us, bool *underran) {
    unsigned i = (unsigned)(ls->seq & LS_MASK);

    if (waited_us > 0) {
        ls->stalls++;
        if (waited_us > ls->peak_wait_us) {
            ls->peak_wait_us = waited_us;
        }
    }

    if (ls->have[i]) {
        if (underran) {
            *underran = false;
        }
        return ls->word[i];
    }

    ls->underruns++;
    if (underran) {
        *underran = true;
    }
    return GBA_WORD_EMPTY;
}

void ls_complete(link_session_t *ls) {
    ls->have[ls->seq & LS_MASK] = false;
    ls->seq++;
    ls->transfers++;
}

unsigned ls_buffered(const link_session_t *ls) {
    unsigned n = 0;
    for (unsigned k = 0; k < LS_DEPTH; k++) {
        if (!ls->have[(ls->seq + k) & LS_MASK]) {
            break;
        }
        n++;
    }
    return n;
}
