/*
 * link_source.h -- where a multiplayer transfer comes from.
 *
 * On hardware it is the Multi-Play PHY: real clock edges on real pins. Under
 * QEMU there are no pins, so the game's word arrives over the UART bridge from
 * mGBA instead.
 *
 * The gate does not care which. It takes the word the game sent and returns
 * the word to answer with, and that loop is byte-identical in both cases --
 * which is the whole point. The QEMU path exercises the real chipgate.c, the
 * real handshake and the real 9-slot rotation, not a stand-in.
 *
 * What it does not exercise is timing, because that is exactly what QEMU
 * cannot model. See qemu/README.md.
 */

#ifndef LINK_SOURCE_H
#define LINK_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "link_proto.h"

typedef struct {
    const char *name;

    /* Block briefly for the next transfer. True if one happened, with the
     * word the parent sent in *from_game. False on timeout, which is normal
     * when no console is attached. */
    bool (*poll)(uint16_t *from_game, uint32_t timeout_ms);

    /* Stage the word to answer with. On hardware this must land in
     * SIOMLT_SEND before the parent clocks our slot. */
    void (*stage)(uint16_t word);

    /* Full transfer as everyone saw it, for reporting upstream. */
    void (*last_transfer)(gba_transfer_t *out);
} link_source_t;

/* The Multi-Play PHY. NULL if this image was built without it. */
const link_source_t *link_source_hardware(void);

/*
 * The bridge. The host sends LP_WORD carrying what the game put on the wire;
 * we answer LP_RESULT carrying the whole transfer including our slot.
 */
const link_source_t *link_source_bridge(void);

/* Whichever this build should use. */
const link_source_t *link_source_active(void);

#endif /* LINK_SOURCE_H */
