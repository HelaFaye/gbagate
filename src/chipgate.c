/*
 * chipgate.c -- Battle Chip Gate link-layer responder.
 * See chipgate.h for provenance of the constants and the slot rotation.
 */

#include "chipgate.h"

/* ---- identity table -------------------------------------------------- */

typedef struct {
    uint16_t    gate_id;
    uint16_t    ack_word;
    const char *name;
} cg_identity_t;

/*
 * ack_word is the word the game sends back to confirm the gate, and sends
 * again later to tear the session down. In the homebrew it lives beside the
 * gate ID in a per-game table, and for every gate observed so far it equals
 * the gate ID itself. If you meet a build where the handshake never completes,
 * this is the first field to instrument -- log every non-0x8FFF word arriving
 * in CG_LINK_SYNCED and you will see the real value immediately.
 */
static const cg_identity_t k_identity[CG_GAME_COUNT] = {
    [CG_GAME_BN4]    = { CG_ID_BATTLECHIP,    CG_ID_BATTLECHIP,    "BN4 / EXE4 / 4.5"   },
    [CG_GAME_BN5]    = { CG_ID_PROGRESS,      CG_ID_PROGRESS,      "BN5 / EXE5"          },
    [CG_GAME_BN6_JP] = { CG_ID_BEAST_LINK,    CG_ID_BEAST_LINK,    "EXE6 (JP)"           },
    [CG_GAME_BN6_US] = { CG_ID_BEAST_LINK_US, CG_ID_BEAST_LINK_US, "MMBN6 (US/EU)"       },
};

/*
 * Seeds for the two rolling counters, lifted from the homebrew's init at
 * 0x080030EC. The games do not appear to validate them -- mGBA gets away with
 * different values -- but matching real hardware costs nothing.
 */
#define CG_DATA0_SEED 0x00FEu
#define CG_DATA1_SEED 0xFFFEu

/* ---- lifecycle ------------------------------------------------------- */

void chipgate_init(chipgate_t *cg, cg_game_t game) {
    if (game < 0 || game >= CG_GAME_COUNT) {
        game = CG_GAME_BN6_US;
    }

    cg->gate_id  = k_identity[game].gate_id;
    cg->ack_word = k_identity[game].ack_word;
    cg->enabled  = true;

    cg->state         = CG_LINK_IDLE;
    cg->slot          = CG_SLOT_COMMAND;
    cg->chip_id       = CG_CHIP_NONE;
    cg->chip_reps     = 0;
    cg->data0         = CG_DATA0_SEED;
    cg->data1         = CG_DATA1_SEED;
    cg->chip_consumed = false;
    cg->frames        = 0;
    cg->sessions      = 0;
}

void chipgate_set_identity(chipgate_t *cg, uint16_t gate_id, uint16_t ack_word) {
    cg->gate_id  = gate_id;
    cg->ack_word = ack_word;
    chipgate_reset_link(cg);
}

void chipgate_reset_link(chipgate_t *cg) {
    cg->state = CG_LINK_IDLE;
    cg->slot  = CG_SLOT_COMMAND;
    cg->data0 = CG_DATA0_SEED;
    cg->data1 = CG_DATA1_SEED;
}

/* ---- chip slot ------------------------------------------------------- */

bool chipgate_load(chipgate_t *cg, uint16_t chip_id) {
    if (chip_id == CG_CHIP_NONE) {
        return false;
    }
    if (cg->chip_id != CG_CHIP_NONE) {
        return false; /* one chip at a time, like the real gate */
    }
    cg->chip_id       = chip_id;
    cg->chip_reps     = 0;
    cg->chip_consumed = false;
    return true;
}

void chipgate_clear(chipgate_t *cg) {
    cg->chip_id   = CG_CHIP_NONE;
    cg->chip_reps = 0;
}

bool chipgate_take_consumed(chipgate_t *cg) {
    bool was = cg->chip_consumed;
    cg->chip_consumed = false;
    return was;
}

/* ---- response rotation ----------------------------------------------- */

static uint16_t cg_word_data0(chipgate_t *cg) {
    uint16_t v = cg->data0;
    cg->data0 = (uint16_t)((v + 3u) & 0x00FFu); /* 8-bit counter, step +3 */
    return v;
}

static uint16_t cg_word_data1(chipgate_t *cg) {
    uint16_t v = cg->data1;
    cg->data1 = (uint16_t)((v - 3u) & 0xFC00u); /* top 6 bits, step -1 */
    return v;
}

static uint16_t cg_word_id(chipgate_t *cg) {
    uint16_t id = cg->chip_id;
    if (id == CG_CHIP_NONE) {
        return CG_CHIP_NONE;
    }
    if (++cg->chip_reps >= CG_CHIP_REPEATS) {
        cg->chip_reps     = 0;
        cg->chip_id       = CG_CHIP_NONE;
        cg->chip_consumed = true;
    }
    return id;
}

static uint16_t cg_word_for_slot(chipgate_t *cg, uint8_t slot) {
    switch (slot) {
    case CG_SLOT_COMMAND: return cg->gate_id;
    case CG_SLOT_UNK_0:   /* fall through */
    case CG_SLOT_UNK_1:   return CG_WORD_CONTINUE;
    case CG_SLOT_DATA_0:  return cg_word_data0(cg);
    case CG_SLOT_DATA_1:  return cg_word_data1(cg);
    case CG_SLOT_ID:      return cg_word_id(cg);
    default:              return 0x0000u; /* UNK_2, UNK_3, END */
    }
}

/* ---- the frame handler ----------------------------------------------- */

uint16_t chipgate_step(chipgate_t *cg, uint16_t from_game) {
    cg->frames++;

    if (!cg->enabled) {
        return CG_WORD_CONTINUE;
    }

    switch (cg->state) {
    case CG_LINK_IDLE:
        /* Hold our identity on the wire continuously. BN5 and BN6 only probe
         * once, on the title screen, so the answer has to already be there. */
        if (from_game == CG_WORD_SYNC) {
            cg->state = CG_LINK_SYNCED;
        }
        return cg->gate_id;

    case CG_LINK_SYNCED:
        if (from_game == cg->ack_word) {
            cg->state = CG_LINK_ACTIVE;
            cg->sessions++;
            /* The handshake already burned COMMAND and UNK_0, so the rotation
             * resumes at UNK_1 rather than restarting. */
            cg->slot = CG_SLOT_UNK_1;
        } else if (from_game != CG_WORD_SYNC) {
            cg->state = CG_LINK_IDLE; /* lost the thread, start over */
        }
        return cg->gate_id;

    case CG_LINK_ACTIVE:
    default:
        /* The game re-sends its acknowledge to close the session. */
        if (from_game == cg->ack_word) {
            chipgate_reset_link(cg);
            return cg->gate_id;
        }
        {
            uint16_t tx = cg_word_for_slot(cg, cg->slot);
            cg->slot = (uint8_t)(cg->slot + 1u);
            if (cg->slot > CG_SLOT_END) {
                cg->slot = CG_SLOT_COMMAND;
            }
            return tx;
        }
    }
}

/* ---- pretty printing ------------------------------------------------- */

const char *chipgate_game_name(cg_game_t game) {
    if (game < 0 || game >= CG_GAME_COUNT) {
        return "?";
    }
    return k_identity[game].name;
}

const char *chipgate_state_name(cg_link_state_t state) {
    switch (state) {
    case CG_LINK_IDLE:   return "waiting";
    case CG_LINK_SYNCED: return "syncing";
    case CG_LINK_ACTIVE: return "linked";
    default:             return "?";
    }
}
