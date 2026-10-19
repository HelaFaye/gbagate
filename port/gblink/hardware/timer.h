/* hardware/timer.h -- Pico SDK busy_wait_us, used by gb.cpp between SPI chunks. */
#ifndef GBAGATE_ZSHIM_PICO_TIMER_H
#define GBAGATE_ZSHIM_PICO_TIMER_H
#include <stdint.h>
#include <zephyr/kernel.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline void busy_wait_us(uint64_t us) { k_busy_wait((uint32_t)us); }
static inline void busy_wait_us_32(uint32_t us) { k_busy_wait(us); }
#ifdef __cplusplus
}
#endif
#endif
