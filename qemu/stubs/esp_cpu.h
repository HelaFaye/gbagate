#pragma once
/*
 * Cycle counter. The Multi-Play PHY times clock edges with it rather than
 * esp_timer, because at 115200 a word is 156 us and the edges inside it are
 * 8.68 us apart -- close enough to the timer's own overhead to matter.
 */
#include <stdint.h>
static inline uint32_t esp_cpu_get_cycle_count(void) { return 0; }
static inline void esp_cpu_set_cycle_count(uint32_t c) { (void)c; }
