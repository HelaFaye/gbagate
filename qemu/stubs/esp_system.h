#pragma once
/*
 * ESP-IDF's esp_system.h pulls in esp_err.h, and sources reasonably rely on
 * that rather than including both. The stub mirrors it so the same include
 * works here.
 *
 * (esp_err.h does exist in ESP-IDF, contrary to the comment that prompted this
 * switch -- but including esp_system.h is equally valid and this makes both
 * work.)
 */
#include "esp_err.h"
#include <stdint.h>

static inline void esp_restart(void) { for (;;) { } }
static inline uint32_t esp_get_free_heap_size(void) { return 0; }
