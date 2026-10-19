/*
 * gblink_gbspi_esp32s3.c -- the Game Boy side of the GB-Link personality:
 * 8-bit SPI as master (gbLinkLayer.h, for GB/GBC trades, the distributor and
 * GBA multiboot over Normal mode), and raw pin access for the printer, which
 * gb.cpp bit-bangs itself as the Game Boy's slave.
 *
 * Upstream runs the SPI on PIO at ~985 kHz: MSB first, clock idling high,
 * data changed on the falling edge and sampled on the rising edge (SPI mode 3,
 * which is the Game Boy's own convention). Bit-banged here, one byte inside a
 * critical section on core 1 so a WiFi interrupt cannot stretch a bit: the
 * Game Boy shifts on our edges, so a stretched bit is merely slow, but a GBA
 * in Normal mode with a short timeout is less forgiving.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_struct.h"

#include "board.h"
#include "layers/gbLinkLayer.h"
#include "zshim.h"

/* ~985 kHz like upstream: 508 ns per half-bit. */
#define HALF_BIT_NS 508u

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_half_cyc;

static inline void IRAM_ATTR wset(int pin, bool v) {
    if (v) GPIO.out_w1ts = 1u << pin; else GPIO.out_w1tc = 1u << pin;
}
static inline bool IRAM_ATTR wget(int pin) { return (GPIO.in >> pin) & 1u; }
static inline void IRAM_ATTR spin(uint32_t cyc) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (esp_cpu_get_cycle_count() - t0 < cyc) { }
}

void gb_link_init(void) {
    s_half_cyc = (uint32_t)((uint64_t)esp_clk_cpu_freq() * HALF_BIT_NS / 1000000000ull);
    gpio_reset_pin(PIN_LINK_SC);
    gpio_set_level(PIN_LINK_SC, 1);
    gpio_set_direction(PIN_LINK_SC, GPIO_MODE_INPUT_OUTPUT);
    gpio_reset_pin(PIN_LINK_SO);
    gpio_set_level(PIN_LINK_SO, 1);
    gpio_set_direction(PIN_LINK_SO, GPIO_MODE_INPUT_OUTPUT);
    gpio_reset_pin(PIN_LINK_SI);
    gpio_set_direction(PIN_LINK_SI, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_LINK_SI, GPIO_PULLUP_ONLY);
    /* Upstream leaves SD floating unless CONFIG_GB_LINK_DRIVE_SD_LOW. */
    gpio_reset_pin(PIN_LINK_SD);
    gpio_set_direction(PIN_LINK_SD, GPIO_MODE_INPUT);
}

void gb_link_deinit(void) {
    gpio_set_direction(PIN_LINK_SC, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_LINK_SC, GPIO_PULLUP_ONLY);
    gpio_set_direction(PIN_LINK_SO, GPIO_MODE_INPUT);
}

static uint8_t IRAM_ATTR xfer_byte(uint8_t out) {
    uint8_t in = 0;
    portENTER_CRITICAL(&s_mux);
    for (int b = 7; b >= 0; b--) {
        wset(PIN_LINK_SC, false);
        wset(PIN_LINK_SO, (out >> b) & 1u);
        spin(s_half_cyc);
        wset(PIN_LINK_SC, true);
        in = (uint8_t)((in << 1) | (wget(PIN_LINK_SI) ? 1u : 0u));
        spin(s_half_cyc);
    }
    portEXIT_CRITICAL(&s_mux);
    return in;
}

void gb_link_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t us_between) {
    for (uint32_t i = 0; i < len; i++) {
        rx[i] = xfer_byte(tx[i]);
        if (us_between && i + 1 < len) esp_rom_delay_us(us_between);
    }
}

/* ---- raw pins for gb.cpp's printer ------------------------------------------------ */

static int pin_of(gblink_signal_t sig) {
    switch (sig) {
    case GBLINK_SIG_SC:     return PIN_LINK_SC;
    case GBLINK_SIG_SI:     return PIN_LINK_SI;
    case GBLINK_SIG_SO:     return PIN_LINK_SO;
    case GBLINK_SIG_SD:     return PIN_LINK_SD;
    case GBLINK_SIG_SD_ALT: return BOARD_HAS_SD_ALT ? PIN_LINK_SD_ALT : -1;
    }
    return -1;
}

void gblink_pin_init(gblink_signal_t sig) {
    int p = pin_of(sig);
    if (p >= 0) gpio_reset_pin(p);
}

void gblink_pin_dir(gblink_signal_t sig, bool output) {
    int p = pin_of(sig);
    if (p >= 0) gpio_set_direction(p, output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

void IRAM_ATTR gblink_pin_put(gblink_signal_t sig, bool high) {
    int p = pin_of(sig);
    if (p >= 0) wset(p, high);
}

/*
 * gb.cpp's printer timeouts are iteration counts tuned on an RP2040, where one
 * gpio_get() loop pass costs ~64 ns (2M passes is its ~128 ms mid-packet
 * abort). A register read here is a few nanoseconds, which would cut that to
 * a few milliseconds and abort prints between bytes. Pad each read to about
 * the RP2040's cost so the vendored constants keep their meaning.
 */
#define RP2040_PASS_NS 64u

bool IRAM_ATTR gblink_pin_get(gblink_signal_t sig) {
    static uint32_t pad;
    if (!pad) pad = (uint32_t)((uint64_t)esp_clk_cpu_freq() * RP2040_PASS_NS / 1000000000ull);
    spin(pad);
    int p = pin_of(sig);
    return p >= 0 ? wget(p) : true;
}

void gblink_pin_pull(gblink_signal_t sig, bool up) {
    int p = pin_of(sig);
    if (p >= 0) gpio_set_pull_mode(p, up ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}
