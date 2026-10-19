/*
 * hardware/gpio.h -- the Pico SDK GPIO calls module/gb.cpp makes for the GB
 * printer, mapped onto the gbagate link port.
 *
 * gb.cpp names pins by their RP2040 GPIO numbers: 0 SC, 1 SI, 2 SO, 3 SD.
 * Those are link-port signals, not chip pins, so the shim treats the numbers as
 * signal names and the port resolves them to whatever this board wired (see
 * gblink_pin_* in port/gblink/zshim.h). Anything outside 0..4 is a vendored
 * change worth noticing, so it asserts rather than guessing.
 */
#ifndef GBAGATE_ZSHIM_PICO_GPIO_H
#define GBAGATE_ZSHIM_PICO_GPIO_H
#include <stdbool.h>
#include <stdint.h>
#include "zshim.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GPIO_IN  false
#define GPIO_OUT true
static inline void gpio_init(unsigned pin)                { gblink_pin_init((gblink_signal_t)pin); }
static inline void gpio_set_dir(unsigned pin, bool out)   { gblink_pin_dir((gblink_signal_t)pin, out); }
static inline void gpio_put(unsigned pin, bool value)     { gblink_pin_put((gblink_signal_t)pin, value); }
static inline bool gpio_get(unsigned pin)                 { return gblink_pin_get((gblink_signal_t)pin); }
static inline void gpio_pull_up(unsigned pin)             { gblink_pin_pull((gblink_signal_t)pin, true); }
static inline void gpio_disable_pulls(unsigned pin)       { gblink_pin_pull((gblink_signal_t)pin, false); }
#ifdef __cplusplus
}
#endif
#endif
