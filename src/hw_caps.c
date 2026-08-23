/*
 * hw_caps.c -- see hw_caps.h
 */

#include <string.h>

#include "hw_caps.h"

uint32_t hw_menu_missing(const menu_entry_t *entry, uint32_t caps) {
    return entry->requires & ~caps;
}

size_t hw_menu_filter(const menu_entry_t *all, size_t count, uint32_t caps,
                      const menu_entry_t **out, size_t out_cap) {
    size_t n = 0;
    for (size_t i = 0; i < count && n < out_cap; i++) {
        if (hw_menu_missing(&all[i], caps) == 0) {
            out[n++] = &all[i];
        }
    }
    return n;
}

size_t hw_menu_hidden(const menu_entry_t *all, size_t count, uint32_t caps,
                      const menu_entry_t **out, size_t out_cap) {
    size_t n = 0;
    for (size_t i = 0; i < count && n < out_cap; i++) {
        if (hw_menu_missing(&all[i], caps) != 0) {
            out[n++] = &all[i];
        }
    }
    return n;
}

const char *hw_cap_name(hw_cap_t cap) {
    switch (cap) {
    case HW_CAP_DISPLAY:     return "display";
    case HW_CAP_CHIP_READER: return "chip reader";
    case HW_CAP_GPIO_EXP:    return "GPIO expander";
    case HW_CAP_NRF24:       return "nRF24 radio";
    case HW_CAP_NRF24_PLUS:  return "nRF24L01+";
    case HW_CAP_WIFI:        return "WiFi";
    case HW_CAP_BLE:         return "Bluetooth";
    case HW_CAP_USB:         return "USB";
    case HW_CAP_LINK_PORT:   return "link cable";
    default:                 return "?";
    }
}

size_t hw_caps_describe(uint32_t mask, char *buf, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';

    size_t used = 0;
    bool first = true;
    for (unsigned bit = 0; bit < 32; bit++) {
        uint32_t m = 1u << bit;
        if (!(mask & m)) {
            continue;
        }
        const char *name = hw_cap_name((hw_cap_t)m);
        size_t need = strlen(name) + (first ? 0u : 2u);
        if (used + need + 1 > cap) {
            break;
        }
        if (!first) {
            memcpy(buf + used, ", ", 2);
            used += 2;
        }
        memcpy(buf + used, name, strlen(name));
        used += strlen(name);
        buf[used] = '\0';
        first = false;
    }
    return used;
}
