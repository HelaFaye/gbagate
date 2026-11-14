/*
 * ui_theme.h -- colour palettes.
 *
 * Every colour the screens use is a role in the current theme rather than a
 * constant, so a theme changes all of them at once and no screen can be left
 * in the old colours. ui_render.h keeps the COL_* names the renderers were
 * written with; each now reads the current theme.
 *
 * Contrast is checked, not hoped for: test_screens computes the WCAG contrast
 * ratio of every pair a screen actually draws (text on the background, on the
 * alternate row band and on the header, status colours on the background),
 * for every theme, and fails one that is hard to read on a small panel.
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_THEME_CLASSIC = 0,  /* dark navy and orange: the original look        */
    UI_THEME_NAVI,         /* PET blue                                       */
    UI_THEME_DMG,          /* the original Game Boy's four greens            */
    UI_THEME_CONTRAST,     /* black, white and yellow, for bright light      */
    UI_THEME_DAYLIGHT,     /* light background                               */
    UI_THEME_COUNT
} ui_theme_id_t;

typedef struct {
    const char *name;
    uint16_t bg;         /* screen background                              */
    uint16_t row_alt;    /* alternate list band, footer                    */
    uint16_t header;     /* title bar, selected row, rules                 */
    uint16_t on_header;  /* text on the header colour and on held rows     */
    uint16_t text;       /* body text                                      */
    uint16_t text_dim;   /* secondary text, hints                          */
    uint16_t accent;     /* the chip screen's mode label, send status      */
    uint16_t star_on;
    uint16_t star_off;
    uint16_t bar_track;
    uint16_t bar_thumb;
    uint16_t held;       /* a chip picked up in Move mode                  */
    uint16_t good;       /* status: working                                */
    uint16_t warn;       /* status: waiting, or needs attention            */
    uint16_t bad;        /* status: failed                                 */
} ui_theme_t;

/* The theme every renderer draws with. Never NULL. */
const ui_theme_t *ui_theme(void);

/* Switch theme; an id out of range selects Classic. */
void ui_theme_set(ui_theme_id_t id);
ui_theme_id_t ui_theme_current(void);

const ui_theme_t *ui_theme_get(ui_theme_id_t id);
const char       *ui_theme_name(ui_theme_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
