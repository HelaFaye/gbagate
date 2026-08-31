/*
 * ui_romview.h -- the Send Program browser.
 *
 * rom_catalog.c decides what is on the drive and whether each file is usable.
 * This decides what the user sees and what they are allowed to pick, which is
 * not the same thing: an unplayable file must still be listed, with its reason,
 * because someone who dragged a file across needs to know why it will not run.
 *
 * No display calls, so the labels, the selectability rules and the empty states
 * are host-testable. gate_app.c renders the result with ui_render_menu().
 */

#ifndef UI_ROMVIEW_H
#define UI_ROMVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rom_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why the list might have nothing to show. Each needs different words: "no
 * programs" is a thing the user can fix, "connected to PC" is not a fault. */
typedef enum {
    ROMVIEW_OK = 0,
    ROMVIEW_HOST_OWNS,    /* a PC has the volume mounted */
    ROMVIEW_NO_STORAGE,   /* the partition would not mount */
    ROMVIEW_EMPTY         /* mounted and readable, nothing on it */
} romview_state_t;

typedef struct {
    const rom_catalog_t *cat;
    romview_state_t      state;
    uint16_t             cursor;
    /*
     * Scratch for label_at(). Wide enough for the longest name plus the widest
     * status: ROM_NAME_MAX is 40, and "bad logo - repair on PC" is 23. The
     * display clips it long before this does, but truncating here would cut a
     * status mid-word and make it unreadable rather than merely short.
     */
    char                 label[72];
} romview_t;

void romview_init(romview_t *v);

/* Point at a scan result, or report why there is not one. */
void romview_set(romview_t *v, const rom_catalog_t *cat, romview_state_t state);

uint8_t romview_count(const romview_t *v);

/*
 * Row text: "name  size  reason". Returns NULL past the end, which is how
 * ui_render_menu discovers the length.
 */
const char *romview_label(void *ctx, uint8_t index);

/* Can this entry actually be sent? Unplayable ones are shown but not sendable. */
bool romview_selectable(const romview_t *v, uint16_t index);

/* The entry under the cursor, or NULL. */
const rom_entry_t *romview_current(const romview_t *v);

void romview_move(romview_t *v, int32_t delta);

/* One line explaining an empty list, or NULL when there are entries. */
const char *romview_empty_text(const romview_t *v);

/* Footer: what the button will do here. */
const char *romview_hint(const romview_t *v);

#ifdef __cplusplus
}
#endif

#endif /* UI_ROMVIEW_H */
