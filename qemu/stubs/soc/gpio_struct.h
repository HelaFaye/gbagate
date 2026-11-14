/* Stub: the GPIO register block, as far as gba_link_esp32s3.c's fast paths
 * use it (the low 32 pins' input, set/clear and enable registers). */
#pragma once
#include <stdint.h>
typedef struct {
    volatile uint32_t in;
    volatile uint32_t out_w1ts, out_w1tc;
    volatile uint32_t enable_w1ts, enable_w1tc;
    volatile uint32_t status_w1tc;
} gpio_dev_t;
extern gpio_dev_t GPIO;
