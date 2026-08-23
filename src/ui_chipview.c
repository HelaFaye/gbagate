/*
 * ui_chipview.c -- see ui_chipview.h
 */

#include <string.h>

#include "ui_chipview.h"

/* Tab to library, for the tabs that map directly. */
static uint8_t tab_library(chip_tab_t tab) {
    switch (tab) {
    case CHIP_TAB_STANDARD: return CHIP_LIB_STANDARD;
    case CHIP_TAB_MEGA:     return CHIP_LIB_MEGA;
    case CHIP_TAB_GIGA:     return CHIP_LIB_GIGA;
    case CHIP_TAB_DARK:     return CHIP_LIB_DARK;
    case CHIP_TAB_PA:       return CHIP_LIB_PA;
    default:                return CHIP_LIB_NONE;
    }
}

static bool fav_get(const chipview_t *v, uint16_t slot) {
    if (slot >= CHIPVIEW_MAX_RECORDS) {
        return false;
    }
    return (v->favorites[slot >> 3] & (uint8_t)(1u << (slot & 7))) != 0;
}

static void fav_set(chipview_t *v, uint16_t slot, bool on) {
    if (slot >= CHIPVIEW_MAX_RECORDS) {
        return;
    }
    uint8_t mask = (uint8_t)(1u << (slot & 7));
    if (on) {
        v->favorites[slot >> 3] |= mask;
    } else {
        v->favorites[slot >> 3] &= (uint8_t)~mask;
    }
    v->favorites_dirty = true;
}

/* Does a record belong in a tab? */
static bool in_tab(const chipview_t *v, uint16_t slot, const chip_record_t *r,
                   chip_tab_t tab) {
    if (tab == CHIP_TAB_FAVORITES) {
        return fav_get(v, slot);
    }
    return r->library == tab_library(tab);
}

/* ---- sorting ---------------------------------------------------------- */

static int cmp_name(const chipview_t *v, uint16_t a, uint16_t b) {
    const char *na = chipdb_name(v->db, a);
    const char *nb = chipdb_name(v->db, b);
    if (!na || !nb) {
        return 0;
    }
    return strcmp(na, nb);
}

static int cmp_records(const chipview_t *v, uint16_t a, uint16_t b) {
    chip_record_t ra, rb;
    if (!chipdb_get(v->db, a, &ra) || !chipdb_get(v->db, b, &rb)) {
        return 0;
    }

    switch (v->sort) {
    case CHIP_SORT_NAME:
        return cmp_name(v, a, b);

    case CHIP_SORT_ATTACK: {
        /* Strongest first, which is what "sort by Attack" means in a game
         * where you are picking something to hit with. */
        if (ra.damage != rb.damage) {
            return ra.damage > rb.damage ? -1 : 1;
        }
        return cmp_name(v, a, b);
    }

    case CHIP_SORT_ELEMENT:
        if (ra.element != rb.element) {
            return ra.element < rb.element ? -1 : 1;
        }
        return cmp_name(v, a, b);

    case CHIP_SORT_LIBRARY:
    default:
        if (ra.lib_number != rb.lib_number) {
            return ra.lib_number < rb.lib_number ? -1 : 1;
        }
        return ra.index < rb.index ? -1 : (ra.index > rb.index ? 1 : 0);
    }
}

/*
 * Insertion sort. n is at most 232 for the largest tab and the list is nearly
 * ordered already in the common case, so this beats dragging in qsort and a
 * comparison context, and it is stable.
 */
static void sort_view(chipview_t *v) {
    if (v->sort == CHIP_SORT_CUSTOM) {
        return; /* the user's arrangement is the order */
    }
    for (uint16_t i = 1; i < v->view_count; i++) {
        uint16_t key = v->view[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && cmp_records(v, v->view[j], key) > 0) {
            v->view[j + 1] = v->view[j];
            j--;
        }
        v->view[j + 1] = key;
    }
}

/* ---- lifecycle -------------------------------------------------------- */

static void recompute_tab_mask(chipview_t *v) {
    v->tab_mask = 0;
    if (!v->db) {
        return;
    }
    if (chipview_favorite_count(v) > 0) {
        v->tab_mask |= 1u << CHIP_TAB_FAVORITES;
    }
    for (uint16_t s = 0; s < chipdb_count(v->db); s++) {
        chip_record_t r;
        if (!chipdb_get(v->db, s, &r)) {
            continue;
        }
        for (int t = CHIP_TAB_STANDARD; t < CHIP_TAB_COUNT; t++) {
            if (r.library == tab_library((chip_tab_t)t)) {
                v->tab_mask |= 1u << t;
            }
        }
    }
}

void chipview_init(chipview_t *v, const chipdb_t *db,
                   uint16_t *view_storage, uint16_t storage_cap) {
    memset(v, 0, sizeof *v);
    v->db       = db;
    v->view     = view_storage;
    v->view_cap = storage_cap;
    v->tab      = CHIP_TAB_STANDARD;
    v->sort     = CHIP_SORT_LIBRARY;
    v->mode     = CHIP_MODE_SLOT_IN;
    v->held     = CHIPVIEW_NONE;
    recompute_tab_mask(v);
    chipview_rebuild(v);
}

void chipview_rebuild(chipview_t *v) {
    if (!v->db || !v->view) {
        v->view_count = 0;
        return;
    }

    /*
     * A custom order is the user's work and must survive a rebuild. Keep the
     * entries that still qualify, in the order they are in, and append anything
     * newly qualifying at the end. Regenerating from database order instead
     * would silently discard a hand-made arrangement.
     */
    uint16_t kept[CHIPVIEW_MAX_RECORDS];
    uint16_t kept_count = 0;
    if (v->sort == CHIP_SORT_CUSTOM) {
        for (uint16_t i = 0; i < v->view_count && kept_count < CHIPVIEW_MAX_RECORDS; i++) {
            uint16_t slot = v->view[i];
            chip_record_t r;
            if (!chipdb_get(v->db, slot, &r)) {
                continue;
            }
            if (in_tab(v, slot, &r, v->tab)) {
                kept[kept_count++] = slot;
            }
        }
    }

    v->view_count = 0;

    for (uint16_t i = 0; i < kept_count && v->view_count < v->view_cap; i++) {
        v->view[v->view_count++] = kept[i];
    }

    for (uint16_t s = 0; s < chipdb_count(v->db); s++) {
        chip_record_t r;
        if (!chipdb_get(v->db, s, &r)) {
            continue;
        }
        /* The homebrew only lists chips the game itself shows in its library. */
        if (!(r.flags & CHIP_FLAG_IN_LIBRARY) && v->tab != CHIP_TAB_FAVORITES) {
            continue;
        }
        if (!in_tab(v, s, &r, v->tab)) {
            continue;
        }

        bool already = false;
        for (uint16_t k = 0; k < kept_count; k++) {
            if (kept[k] == s) {
                already = true;
                break;
            }
        }
        if (already || v->view_count >= v->view_cap) {
            continue;
        }
        v->view[v->view_count++] = s;
    }

    sort_view(v);
    v->held = CHIPVIEW_NONE;
}

bool chipview_truncated(const chipview_t *v) {
    return v->db && v->view_cap < chipdb_count(v->db);
}

/* ---- tabs ------------------------------------------------------------- */

bool chipview_tab_available(const chipview_t *v, chip_tab_t tab) {
    if (tab >= CHIP_TAB_COUNT) {
        return false;
    }
    return (v->tab_mask & (1u << tab)) != 0;
}

bool chipview_set_tab(chipview_t *v, chip_tab_t tab) {
    if (!chipview_tab_available(v, tab)) {
        return false;
    }
    v->tab = tab;
    chipview_rebuild(v);
    return true;
}

chip_tab_t chipview_tab_cycle(chipview_t *v, int direction) {
    if (v->tab_mask == 0) {
        return v->tab;
    }
    int step = direction >= 0 ? 1 : -1;
    int cur = (int)v->tab;

    /* Walk past greyed tabs. At most one full lap, so an all-grey mask cannot
     * spin forever. */
    for (int n = 0; n < CHIP_TAB_COUNT; n++) {
        cur += step;
        if (cur < 0) {
            cur = CHIP_TAB_COUNT - 1;
        } else if (cur >= CHIP_TAB_COUNT) {
            cur = 0;
        }
        if (chipview_tab_available(v, (chip_tab_t)cur)) {
            v->tab = (chip_tab_t)cur;
            chipview_rebuild(v);
            break;
        }
    }
    return v->tab;
}

void chipview_set_sort(chipview_t *v, chip_sort_t sort) {
    if (sort >= CHIP_SORT_COUNT) {
        return;
    }
    v->sort = sort;
    chipview_rebuild(v);
}

void chipview_set_mode(chipview_t *v, chip_mode_t mode) {
    v->mode = mode;
    v->held = CHIPVIEW_NONE;
}

/* ---- favourites ------------------------------------------------------- */

bool chipview_is_favorite(const chipview_t *v, uint16_t slot) {
    return fav_get(v, slot);
}

uint16_t chipview_favorite_count(const chipview_t *v) {
    uint16_t n = 0;
    for (size_t i = 0; i < sizeof v->favorites; i++) {
        uint8_t b = v->favorites[i];
        while (b) {
            n += (b & 1u);
            b >>= 1;
        }
    }
    return n;
}

void chipview_toggle_favorite(chipview_t *v, uint16_t slot) {
    bool now = !fav_get(v, slot);
    fav_set(v, slot, now);
    recompute_tab_mask(v);

    /*
     * Un-starring while looking at the favourites tab has to drop the row, or
     * the list shows something that is no longer there. Every other tab is
     * unaffected, so leave it alone and keep the cursor where it was.
     */
    if (v->tab == CHIP_TAB_FAVORITES) {
        if (!chipview_tab_available(v, CHIP_TAB_FAVORITES)) {
            chipview_tab_cycle(v, 1);
        } else {
            chipview_rebuild(v);
        }
    }
}

/* ---- move mode -------------------------------------------------------- */

uint16_t chipview_slot_at(const chipview_t *v, uint16_t position) {
    if (position >= v->view_count) {
        return CHIPVIEW_NONE;
    }
    return v->view[position];
}

void chipview_pick(chipview_t *v, uint16_t position) {
    if (v->mode != CHIP_MODE_MOVE || position >= v->view_count) {
        return;
    }
    v->held = position;
}

bool chipview_place(chipview_t *v, uint16_t position) {
    if (v->held == CHIPVIEW_NONE || position >= v->view_count) {
        return false;
    }
    if (position == v->held) {
        v->held = CHIPVIEW_NONE;
        return false; /* placed on itself: the caller may treat this as a send */
    }

    uint16_t tmp = v->view[v->held];
    v->view[v->held] = v->view[position];
    v->view[position] = tmp;
    v->held = CHIPVIEW_NONE;

    /* A hand-made arrangement is not any of the sorts, so say so rather than
     * silently losing it on the next rebuild. */
    v->sort = CHIP_SORT_CUSTOM;
    return true;
}

void chipview_cancel(chipview_t *v) {
    v->held = CHIPVIEW_NONE;
}

uint16_t chipview_held(const chipview_t *v) {
    return v->held;
}

/* ---- names ------------------------------------------------------------ */

const char *chipview_tab_name(chip_tab_t tab) {
    switch (tab) {
    case CHIP_TAB_FAVORITES: return "*";
    case CHIP_TAB_STANDARD:  return "Standrd";
    case CHIP_TAB_MEGA:      return "Mega";
    case CHIP_TAB_GIGA:      return "Giga";
    case CHIP_TAB_DARK:      return "Dark";
    case CHIP_TAB_PA:        return "PA";
    default:                 return "?";
    }
}

const char *chipview_sort_name(chip_sort_t sort) {
    switch (sort) {
    case CHIP_SORT_LIBRARY: return "Library";
    case CHIP_SORT_NAME:    return "Name";
    case CHIP_SORT_ATTACK:  return "Attack";
    case CHIP_SORT_ELEMENT: return "Element";
    case CHIP_SORT_CUSTOM:  return "Custom";
    default:                return "?";
    }
}

const char *chipview_mode_name(chip_mode_t mode) {
    return mode == CHIP_MODE_MOVE ? "MOVE" : "SLOT-IN";
}
