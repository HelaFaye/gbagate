/*
 * input_esp32s3.c -- the buttons and the encoder, on hardware, to UI events.
 *
 * Nothing did this. buttons_init() and buttons_mcp_init() had no caller, no
 * task polled ui_input, and the encoder's pins were never assigned, so on a
 * real board the menu drew and nothing could move it. Only the QEMU image
 * had an input loop (input_qemu.c).
 *
 * One task, on core 0, every millisecond:
 *   - in encoder mode, sample A and B through input_encoder.c, which
 *     debounces structurally and accelerates, and post its turns as moves;
 *   - poll ui_input (the switches, or the encoder's own button) through the
 *     controls layer and post what it means -- the same mapping QEMU uses
 *     (src/input_events.c), so emulator and board cannot disagree.
 *
 * The expander is read over I2C, so on the Supermini the controls are polled
 * every 5 ms rather than every 1: a two-byte read is ~60 us, and 12 ms of
 * debounce does not need more.
 */

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controls.h"
#include "input_encoder.h"
#include "input_events.h"
#include "ui_input.h"

static const char *TAG = "input";

extern ui_input_t *input_active(void);
extern bool        tasks_post_ui_event(const ui_event_t *e);
#if BOARD_BUTTONS_ON_EXPANDER
extern esp_err_t buttons_mcp_init(input_mode_t mode);
#define START_SWITCHES buttons_mcp_init
#define POLL_EVERY_MS  5
#else
extern esp_err_t buttons_init(input_mode_t mode);
#define START_SWITCHES buttons_init
#define POLL_EVERY_MS  1
#endif

static controls_t   s_controls;
static encoder_t    s_enc;
static bool         s_encoder;
static input_mode_t s_mode;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void input_task(void *arg) {
    (void)arg;
    uint32_t last_poll = 0;
    for (;;) {
        uint32_t t = now_ms();
#if BOARD_HAS_ENCODER
        if (s_encoder) {
            /* The switch is ui_input's; tell the decoder it is at rest. */
            encoder_event_t ev = encoder_poll(&s_enc, gpio_get_level(PIN_ENC_A),
                                              gpio_get_level(PIN_ENC_B), true, t);
            if (ev.delta) {
                ui_event_t e = { .type = UI_EVENT_ENCODER, .a = ev.delta };
                tasks_post_ui_event(&e);
            }
        }
#endif
        if ((uint32_t)(t - last_poll) >= POLL_EVERY_MS) {
            last_poll = t;
            input_drain(&s_controls, t, tasks_post_ui_event);
        }
        vTaskDelay(1);   /* CONFIG_FREERTOS_HZ is 1000 */
    }
}

static TaskHandle_t s_task;

/*
 * Bring input up in a mode, or the nearest one this board has: from boot, and
 * again whenever Settings changes it, so a new mode applies at once rather
 * than after a restart nobody was told about. Returns the mode in use.
 */
int input_hw_set_mode(int mode_in) {
    s_encoder = false;              /* the task stops sampling while we rewire */
    s_mode = (input_mode_t)mode_in;
    if (s_mode >= INPUT_MODE_COUNT) {
        s_mode = INPUT_ENCODER;
    }
    esp_err_t err = START_SWITCHES(s_mode);
    if (err == ESP_ERR_NOT_SUPPORTED && s_mode == INPUT_ENCODER) {
        /* Encoder chosen on a board without one: the switches, rather than
         * a device nobody can operate. */
        ESP_LOGW(TAG, "no encoder on this board; using the five-way");
        s_mode = INPUT_DPAD2;
        err = START_SWITCHES(s_mode);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input failed to start (%s): nothing can be pressed",
                 esp_err_to_name(err));
        return -1;
    }
    s_encoder = (s_mode == INPUT_ENCODER);
    if (s_encoder) {
        encoder_init(&s_enc, true);
    }
    controls_bind(&s_controls, input_active(), INPUT_WANTED);
    ESP_LOGI(TAG, "%s", input_mode_name(s_mode));
    return (int)s_mode;
}

int input_hw_start(int mode_in) {
    int m = input_hw_set_mode(mode_in);
    if (m >= 0 && !s_task) {
        xTaskCreatePinnedToCore(input_task, "input", 3072, NULL, 5, &s_task, 0);
    }
    return m;
}
