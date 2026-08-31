/*
 * ui_settings.h -- the settings screen as a list of editable fields.
 *
 * The first version adjusted one value, the panel row offset, because that was
 * the one that could not be guessed. Everything else was displayed and could
 * not be changed, which is worse than not showing it: a screen that lists five
 * settings and only responds to one teaches you not to trust it.
 *
 * A field table rather than a switch per setting, so adding one is a row and
 * the cursor, clamping and formatting come for free. No display calls, so all
 * of that is host-testable.
 */

#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SETFIELD_COLSTART = 0,
    SETFIELD_ROWSTART,
    SETFIELD_COLOUR,
    SETFIELD_GAME,
    SETFIELD_ACCEL,
    SETFIELD_WRAP,
    SETFIELD_COUNT
} setfield_t;

typedef struct {
    settings_t *s;
    uint8_t     cursor;
    bool        editing;    /* turning changes the value, not the selection */
    char        scratch[40];
} ui_settings_t;

void uiset_init(ui_settings_t *v, settings_t *s);

uint8_t uiset_count(void);

/* "name  value", for ui_render_menu(). NULL past the end. */
const char *uiset_label(void *ctx, uint8_t index);

/*
 * Move. Outside edit mode this moves the cursor; inside it changes the
 * selected value. One encoder, two jobs -- the button switches between them,
 * which is the usual arrangement when there is nothing else to press.
 */
void uiset_move(ui_settings_t *v, int32_t delta);

/* The button: enter or leave edit mode. Returns true if now editing. */
bool uiset_toggle_edit(ui_settings_t *v);

/* Did the last move actually change a stored value? */
bool uiset_dirty(const ui_settings_t *v);
void uiset_clear_dirty(ui_settings_t *v);

/* Which settings need the display reprogrammed to take effect. */
bool uiset_display_affected(const ui_settings_t *v);

const char *uiset_footer(const ui_settings_t *v);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETTINGS_H */
