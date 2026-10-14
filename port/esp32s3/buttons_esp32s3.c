/*
 * buttons_esp32s3.c -- the five-way switch and its companions.
 *
 * Reading only. Debounce, repeat and long-press are in src/ui_input.c, where
 * they can be tested; this returns a raw mask and nothing else.
 *
 * Not yet run on hardware.
 *
 * Wiring
 * ------
 * Every switch shorts its pin to ground, with the internal pull-up holding it
 * high at rest, so a pressed switch reads low and this inverts. That is the
 * usual arrangement for a five-way navigation switch, which is a common ground
 * with five contacts around it.
 *
 * Which switches exist depends on the mode: two adds B and Select, four adds
 * L and R, five adds Start. Pins for switches a board does not have are left
 * unconfigured, and ui_input.c masks their bits off as well -- belt and
 * braces, because a floating pin read as pressed looks like a stuck button
 * rather than a missing one.
 */

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "ui_input.h"

static const char *TAG = "buttons";

/*
 * Pins. The five-way takes five, the extra switches two or four. These are
 * the pins left after the link port, the display, I2C and the UARTs; check
 * them against the pin budget in README.md before building a board.
 */
static const struct { uint32_t bit; int pin; } k_map[] = {
    { BTN_UP,     10 },
    { BTN_DOWN,   11 },
    { BTN_LEFT,   12 },
    { BTN_RIGHT,  13 },
    { BTN_A,      14 },   /* the five-way's centre click */
    { BTN_B,      15 },
    { BTN_SELECT, 16 },
    { BTN_L,      17 },   /* four-switch boards only */
    { BTN_R,      18 },
    { BTN_START,  21 },   /* five-switch boards only */
};
#define MAP_COUNT (sizeof k_map / sizeof k_map[0])

static uint32_t s_configured;   /* which bits we actually set up */
static ui_input_t s_input;

ui_input_t *input_active(void) { return &s_input; }

/*
 * The raw mask ui_input.c polls. Only configured pins are read: an
 * unconfigured input on the S3 floats, and a floating pin sampled as pressed
 * would look like a stuck button rather than a missing one.
 */
uint32_t buttons_read(void *ctx) {
    (void)ctx;
    uint32_t mask = 0;
    for (unsigned i = 0; i < MAP_COUNT; i++) {
        if (!(s_configured & k_map[i].bit)) {
            continue;
        }
        if (gpio_get_level(k_map[i].pin) == 0) {   /* active low */
            mask |= k_map[i].bit;
        }
    }
    return mask;
}

esp_err_t buttons_init(input_mode_t mode) {
    ui_input_init(&s_input, mode, buttons_read, NULL);
    s_configured = 0;
    if (mode == INPUT_ENCODER) {
        /* input_encoder.c owns the pins in that mode; nothing to do here. */
        ESP_LOGI(TAG, "encoder mode: no navigation switches configured");
        return ESP_OK;
    }

    uint64_t pins = 0;
    for (unsigned i = 0; i < MAP_COUNT; i++) {
        bool wanted = true;
        /*
         * Only configure what the board has. An unconfigured input floats, and
         * a floating pin read as pressed looks like a stuck button rather than
         * a missing one -- so the pins that are not there are simply never set
         * up, and ui_input.c masks their bits off as well.
         */
        if (k_map[i].bit == BTN_L || k_map[i].bit == BTN_R) {
            wanted = (mode == INPUT_DPAD4 || mode == INPUT_DPAD5);
        } else if (k_map[i].bit == BTN_START) {
            wanted = (mode == INPUT_DPAD5);
        }
        if (!wanted) {
            continue;
        }
        pins |= 1ULL << k_map[i].pin;
        s_configured |= k_map[i].bit;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s: %u switches configured",
             input_mode_name(mode), input_mode_switches(mode));
    return ESP_OK;
}

/*
 * Is anything actually there? With pull-ups, an absent five-way reads as all
 * released, which is indistinguishable from one nobody is touching -- so this
 * cannot detect absence, and deliberately does not pretend to. It reports
 * whether the pins were configured, which is what the diagnostics screen can
 * honestly say.
 */
bool buttons_present(void) {
    return s_configured != 0;
}
