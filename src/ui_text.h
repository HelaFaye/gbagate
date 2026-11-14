/*
 * ui_text.h -- the small text rules every screen shares.
 *
 * Header-only on purpose: the settings editor and the program browser use
 * these, and are tested without linking a renderer.
 *
 *   - Keys. A footer names what this board's keys do on this screen ("click
 *     lend", "A lend"), so it never claims an action the screen does not
 *     have. ui_keys_for() gives the short key names for an input mode.
 *   - Two-part footers. "left\tright": ui_render_menu() and pages draw the
 *     right part right-aligned, dim, after the left. Back goes on the right.
 *   - Two-column rows. A menu row shows UI_MENU_COLS glyphs; ui_two_col()
 *     keeps the right column whole and shortens the left, so a value is never
 *     the part cut off.
 */

#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Glyphs a menu row shows: x 12 to the scrollbar, 6px each. */
#define UI_MENU_COLS 18

typedef struct {
    const char *confirm;   /* "click", "A"      */
    const char *back;      /* "hold", "B"       */
    const char *move;      /* "turn", "up/dn"   */
} ui_keys_t;

/* ui_input.h numbers the modes; INPUT_ENCODER is 0, every other has A and B. */
static inline ui_keys_t ui_keys_for(int input_mode) {
    if (input_mode == 0) {
        return (ui_keys_t){ "click", "hold", "turn" };
    }
    return (ui_keys_t){ "A", "B", "up/dn" };
}

/* Append s at *n, cutting at cap; out stays terminated. */
static inline void ui_append(char *out, size_t cap, size_t *n, const char *s) {
    while (s && *s && *n + 1 < cap) {
        out[(*n)++] = *s++;
    }
    if (cap) {
        out[*n < cap ? *n : cap - 1] = '\0';
    }
}

/*
 * "left\tright", either half optional. Cut to cap on purpose: an overlong
 * footer is caught by its test and clipped at render time. (Not snprintf:
 * GCC's -Wformat-truncation, an error in the firmware's -Os build, cannot
 * tell a deliberate cut from an accident.)
 */
static inline void ui_footer_join(char *out, size_t cap, const char *left,
                                  const char *right) {
    size_t n = 0;
    if (cap) out[0] = '\0';
    ui_append(out, cap, &n, left);
    if (right && right[0]) {
        ui_append(out, cap, &n, "\t");
        ui_append(out, cap, &n, right);
    }
}

/* "<key> <verb>", or "" without a verb. */
static inline void ui_key_verb(char *out, size_t cap, const char *key, const char *verb) {
    if (key && verb) {
        snprintf(out, cap, "%s %s", key, verb);
    } else if (cap) {
        out[0] = '\0';
    }
}

/*
 * left, padded, then right ending at column cols. When both do not fit the
 * left is cut and ends in '.', so "pokemon-link.mb  32K" becomes
 * "pokemon-lin.  32K": the size is what changes the decision.
 */
static inline void ui_two_col(char *out, size_t cap, const char *left,
                              const char *right, size_t cols) {
    size_t r = right ? strlen(right) : 0;
    size_t room = cols > r + 1 ? cols - r - 1 : 0;   /* left, plus one space */
    size_t l = left ? strlen(left) : 0;
    char cut[64];
    if (l > room) {
        size_t keep = room > 1 ? room - 1 : 0;
        if (keep > sizeof cut - 2) keep = sizeof cut - 2;
        memcpy(cut, left, keep);
        cut[keep] = room ? '.' : '\0';
        cut[keep + (room ? 1 : 0)] = '\0';
        left = cut;
        l = strlen(cut);
    }
    size_t pad = cols > l + r ? cols - l - r : 1;
    size_t n = 0;
    if (cap) out[0] = '\0';
    ui_append(out, cap, &n, left);
    for (size_t i = 0; i < pad && n + 1 < cap; i++) {
        out[n++] = ' ';
    }
    if (cap) out[n] = '\0';
    ui_append(out, cap, &n, right);   /* cut to cap, as ui_footer_join */
}

#ifdef __cplusplus
}
#endif

#endif /* UI_TEXT_H */
