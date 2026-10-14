#pragma once
/*
 * esp_clk_cpu_freq() lives behind esp_private/ in modern ESP-IDF. The PHY
 * needs it to convert microseconds to cycles, since it times link edges with
 * the cycle counter rather than a timer.
 */
static inline int esp_clk_cpu_freq(void) { return 240000000; }
