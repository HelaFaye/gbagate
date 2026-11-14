/*
 * buttons_esp32s3.c -- the five-way switch and its companions.
 *
 * Reading only. Debounce, repeat and long-press are in src/ui_input.c, where
 * they can be tested; this returns a raw mask and nothing else.
 *
 * Not yet run on hardware. Started by input_esp32s3.c, which samples the
 * encoder in that mode and turns everything into UI events.
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

#include "board.h"
#include "ui_input.h"

static const char *TAG = "buttons";

/*
 * Pins, from board.h. This table used to carry its own -- 10 to 21 -- which
 * on the devkit are the SD card's chip select, the display's SPI bus and its
 * DC line: pressing a switch would have corrupted the panel.
 */
static const struct { uint32_t bit; int pin; } k_map[] = {
    { BTN_UP,     PIN_BTN_UP },
    { BTN_DOWN,   PIN_BTN_DOWN },
    { BTN_LEFT,   PIN_BTN_LEFT },
    { BTN_RIGHT,  PIN_BTN_RIGHT },
    { BTN_A,      PIN_BTN_A },      /* the five-way's centre click */
    { BTN_B,      PIN_BTN_B },
    { BTN_SELECT, PIN_BTN_SELECT },
    { BTN_L,      PIN_BTN_L },      /* four-switch boards only */
    { BTN_R,      PIN_BTN_R },
    { BTN_START,  PIN_BTN_START },  /* five-switch boards only */
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

#if BOARD_HAS_ENCODER
/* In encoder mode the only switch is the encoder's own, and it is A. */
static uint32_t encoder_button_read(void *ctx) {
    (void)ctx;
    return gpio_get_level(PIN_ENC_SW) == 0 ? BTN_A : 0;
}
#endif

esp_err_t buttons_init(input_mode_t mode) {
    s_configured = 0;
    if (mode == INPUT_ENCODER) {
#if BOARD_HAS_ENCODER
        /* A, B and the switch, all pulled up; the quadrature is sampled by
         * input_esp32s3.c, the switch by ui_input through this read. */
        gpio_config_t enc = {
            .pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B) |
                            (1ULL << PIN_ENC_SW),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&enc);
        if (e != ESP_OK) {
            return e;
        }
        ui_input_init(&s_input, mode, encoder_button_read, NULL);
        ESP_LOGI(TAG, "encoder on A=%d B=%d SW=%d", PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
        return ESP_OK;
#else
        return ESP_ERR_NOT_SUPPORTED;   /* the caller falls back to switches */
#endif
    }
    ui_input_init(&s_input, mode, buttons_read, NULL);

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
