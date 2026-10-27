/*
 * ui_chipview.h -- library tabs, favourites, sort and mode for the chip screen.
 *
 * Rebuilds the Battle Chip Gate Homebrew's chip screen behaviour on top of the
 * packed database: six tabs (star, Standard, Mega, Giga, Dark, PA), sorting by
 * name, attack or element, favourites, and the Slot-In / Move mode split.
 *
 * Unavailable tabs are greyed and skipped rather than hidden. That is the
 * opposite of hw_caps, deliberately: a game with no Dark library is telling you
 * something about the game, whereas a missing nRF24 is just absent hardware.
 *
 * No display calls, so the whole thing is host-testable.
 */

#ifndef UI_CHIPVIEW_H
#define UI_CHIPVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "chipdb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIP_TAB_FAVORITES = 0,
    CHIP_TAB_STANDARD,
    CHIP_TAB_MEGA,
    CHIP_TAB_GIGA,
    CHIP_TAB_DARK,
    CHIP_TAB_PA,
    CHIP_TAB_COUNT
} chip_tab_t;

typedef enum {
    CHIP_SORT_LIBRARY = 0, /* library number, the homebrew's default */
    CHIP_SORT_NAME,
    CHIP_SORT_ATTACK,      /* damage, strongest first */
    CHIP_SORT_ELEMENT,
    CHIP_SORT_CUSTOM,      /* whatever Move mode left behind */
    CHIP_SORT_COUNT
} chip_sort_t;

typedef enum {
    CHIP_MODE_SLOT_IN = 0, /* click sends            */
    CHIP_MODE_MOVE         /* click picks and places */
} chip_mode_t;

/* One bit per record. 424 chips is 53 bytes, so this is cheap to persist. */
#define CHIPVIEW_MAX_RECORDS 512
#define CHIPVIEW_FAV_BYTES   (CHIPVIEW_MAX_RECORDS / 8)

typedef struct {
    const chipdb_t *db;

    uint16_t *view;      /* caller-owned: record slots in display order */
    uint16_t  view_cap;
    uint16_t  view_count;

    chip_tab_t  tab;
    chip_sort_t sort;
    chip_mode_t mode;

    uint8_t favorites[CHIPVIEW_FAV_BYTES];
    bool    favorites_dirty; /* set when persistence should be rewritten */

    /* Move mode: the slot picked up, or CHIPVIEW_NONE. */
    uint16_t held;

    uint32_t tab_mask; /* bit per tab: 1 = has content, so selectable */
} chipview_t;

#define CHIPVIEW_NONE 0xFFFFu

/*
 * Attach a database and a caller-owned view array. The array must hold at
 * least chipdb_count() entries; anything smaller truncates and is reported by
 * chipview_truncated().
 */
void chipview_init(chipview_t *v, const chipdb_t *db,
                   uint16_t *view_storage, uint16_t storage_cap);

/* Rebuild the view for the current tab and sort. Call after any change. */
void chipview_rebuild(chipview_t *v);

bool chipview_truncated(const chipview_t *v);

/* Is a tab worth showing as selectable? */
bool chipview_tab_available(const chipview_t *v, chip_tab_t tab);

/*
 * Move to the next or previous selectable tab, skipping greyed ones and
 * wrapping. Returns the tab landed on.
 */
chip_tab_t chipview_tab_cycle(chipview_t *v, int direction);

bool chipview_set_tab(chipview_t *v, chip_tab_t tab);
void chipview_set_sort(chipview_t *v, chip_sort_t sort);
void chipview_set_mode(chipview_t *v, chip_mode_t mode);

/* Favourites, indexed by record slot. */
bool chipview_is_favorite(const chipview_t *v, uint16_t slot);
void chipview_toggle_favorite(chipview_t *v, uint16_t slot);
uint16_t chipview_favorite_count(const chipview_t *v);

/*
 * Move mode. The first click picks up the entry at a view position, the second
 * places it there, swapping. Placing on the held position cancels; the
 * homebrew instead sends on a repeat click, which the caller can do by
 * checking chipview_held() before calling.
 */
void chipview_pick(chipview_t *v, uint16_t position);
bool chipview_place(chipview_t *v, uint16_t position);
void chipview_cancel(chipview_t *v);
uint16_t chipview_held(const chipview_t *v);

/* Record slot shown at a view position, or CHIPVIEW_NONE. */
uint16_t chipview_slot_at(const chipview_t *v, uint16_t position);

const char *chipview_tab_name(chip_tab_t tab);
const char *chipview_sort_name(chip_sort_t sort);
const char *chipview_mode_name(chip_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHIPVIEW_H */
