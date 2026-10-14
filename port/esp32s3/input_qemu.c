/*
 * input_qemu.c -- keyboard input from the host, standing in for the encoder.
 *
 * There is no rotary encoder under emulation, and QEMU wires UART peripherals
 * rather than the GPIO matrix, so nothing the encoder driver reads will ever
 * change. This reads single-byte commands from the same UART2 socket that
 * st7735_qemu.c streams the framebuffer out of -- a QEMU tcp chardev is
 * bidirectional, so the read direction costs nothing and needs no extra port.
 *
 * A virtual switch bank, not a shortcut
 * -------------------------------------
 * This used to post encoder events straight into the UI queue, which meant the
 * whole of ui_input.c -- debounce, repeat, long press, and every mode except
 * the encoder -- never ran under emulation. Three of the four input modes were
 * unreachable, so a matrix run across them would have tested one mode four
 * times.
 *
 * Now a keystroke sets a bit in a virtual button mask and ui_input.c reads it
 * exactly as it reads real pins. A tap holds the bit briefly; a shifted key
 * holds it past the long-press threshold. Everything downstream is the real
 * path.
 *
 * The commands are deliberately one byte and human-typable, so the channel can
 * be driven by qemu/viewer.py, by a test script, or by hand with netcat when
 * something is behaving oddly:
 *
 *     u d l r   the five-way's directions
 *     s / \r    the five-way's click, or the encoder button (A)
 *     S         the same, held past the long-press threshold
 *     b         B
 *     f         Select
 *     [ ]       L and R, on four- and five-switch boards
 *     m         Start, on five-switch boards
 *     M         Start, held
 *
 * Events go through the same queue the real encoder would use, so ui_service()
 * cannot tell the difference and no UI code is emulation-aware.
 *
 * Only built for QEMU images. On hardware, input_encoder.c reads real pins.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controls.h"
#include "ui_input.h"

static const char *TAG = "input_qemu";

/*
 * The same port st7735_qemu.c streams the framebuffer out of, whichever that
 * is -- the two must agree, so both take it from QEMU_AUX_UART and the
 * top-level CMakeLists is the single place it is set. It is 2 by default; a
 * QEMU whose esp32s3 machine only wires two UARTs silently discards everything
 * on UART2, which kills the encoder as surely as it kills the screen, and
 * building with -D QEMU_AUX_UART=0 moves both onto the console instead.
 */
#ifndef QEMU_AUX_UART
#define QEMU_AUX_UART 2
#endif

#define IN_UART ((uart_port_t)QEMU_AUX_UART)

/* Mirrors ui_event_t in pio/src/tasks.c. Kept in step by the compile-time
 * check at the bottom of that file. */
typedef enum {
    UI_EVENT_ENCODER = 0, UI_EVENT_CHIP_IN, UI_EVENT_CHIP_OUT,
    UI_EVENT_LINK_UP, UI_EVENT_LINK_DOWN, UI_EVENT_CHIP_SENT,
    UI_EVENT_USB_ATTACHED, UI_EVENT_USB_DETACHED
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int32_t         a;
    uint16_t        b;
} ui_event_t;

extern bool tasks_post_ui_event(const ui_event_t *e);

static void post(ui_event_type_t type, int32_t a) {
    ui_event_t e = { .type = type, .a = a, .b = 0 };
    tasks_post_ui_event(&e);
}

/*
 * The virtual bank. A key sets a bit and a deadline; the reader clears bits
 * whose deadline has passed, so a keystroke behaves like a press and release
 * rather than a level that stays stuck.
 */
#define TAP_MS  90     /* comfortably past UI_IN_DEBOUNCE_MS, short of a hold */
#define HOLD_MS 900    /* comfortably past UI_IN_LONG_MS                      */

static volatile uint32_t s_bits;
static volatile uint32_t s_until_ms[10];

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void tap(uint32_t bit, uint32_t ms) {
    for (int i = 0; i < 10; i++) {
        if (bit == (1u << i)) {
            s_until_ms[i] = now_ms() + ms;
            s_bits |= bit;
            return;
        }
    }
}

/* The read function ui_input.c polls -- the same shape buttons_esp32s3.c
 * provides on real hardware, so the path under test is identical. */
static uint32_t qemu_buttons_read(void *ctx) {
    (void)ctx;
    uint32_t t = now_ms();
    for (int i = 0; i < 10; i++) {
        if ((s_bits & (1u << i)) && (int32_t)(t - s_until_ms[i]) >= 0) {
            s_bits &= ~(1u << i);
        }
    }
    return s_bits;
}

static ui_input_t  s_input;

/* The controls layer binds against this, so screens describe the emulated
 * hardware rather than a keyboard. */
ui_input_t *input_active(void) { return &s_input; }
static controls_t  s_controls;

/*
 * Everything a screen might use. gate_app.c will narrow this per screen once
 * it is converted; until then the emulated bank offers the lot so a matrix run
 * can exercise every binding.
 */
#define QEMU_WANTED (CTRL_BIT(CTRL_PREV) | CTRL_BIT(CTRL_NEXT) | \
                     CTRL_BIT(CTRL_PAGE_PREV) | CTRL_BIT(CTRL_PAGE_NEXT) | \
                     CTRL_BIT(CTRL_TAB_PREV) | CTRL_BIT(CTRL_TAB_NEXT) | \
                     CTRL_BIT(CTRL_CONFIRM) | CTRL_BIT(CTRL_CANCEL) | \
                     CTRL_BIT(CTRL_MARK) | CTRL_BIT(CTRL_MENU))

static void input_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "virtual switches on UART%d, mode %s",
             IN_UART, input_mode_name(s_input.mode));

    uint8_t buf[32];
    for (;;) {
        int n = uart_read_bytes(IN_UART, buf, sizeof buf, pdMS_TO_TICKS(10));
        for (int i = 0; i < n; i++) {
            switch (buf[i]) {
            case 'u': tap(BTN_UP, TAP_MS); break;
            case 'd': tap(BTN_DOWN, TAP_MS); break;
            case 'l': tap(BTN_LEFT, TAP_MS); break;
            case 'r': tap(BTN_RIGHT, TAP_MS); break;
            case 's': case '\r': case '\n': tap(BTN_A, TAP_MS); break;
            case 'S': tap(BTN_A, HOLD_MS); break;
            case 'b': tap(BTN_B, TAP_MS); break;
            case 'f': tap(BTN_SELECT, TAP_MS); break;
            case '[': tap(BTN_L, TAP_MS); break;
            case ']': tap(BTN_R, TAP_MS); break;
            case 'm': tap(BTN_START, TAP_MS); break;
            case 'M': tap(BTN_START, HOLD_MS); break;
            default: break;   /* escape sequences land here harmlessly */
            }
        }

        /* Drain: one poll can produce a press and a repeat. */
        for (;;) {
            int32_t v = 0;
            ctrl_action_t a = controls_next(&s_controls, now_ms(), &v);
            if (a == CTRL_NONE) {
                break;
            }
            switch (a) {
            case CTRL_PREV:      post(UI_EVENT_ENCODER, -1); break;
            case CTRL_NEXT:      post(UI_EVENT_ENCODER, +1); break;
            case CTRL_PAGE_PREV: post(UI_EVENT_ENCODER, v);  break;
            case CTRL_PAGE_NEXT: post(UI_EVENT_ENCODER, v);  break;
            case CTRL_CONFIRM:   post(UI_EVENT_CHIP_SENT, 0); break;
            case CTRL_CANCEL:    post(UI_EVENT_CHIP_OUT, 0);  break;
            case CTRL_MARK:      post(UI_EVENT_CHIP_IN, 0);   break;
            default: break;   /* tabs and the menu once gate_app takes them */
            }
        }
    }
}

void input_qemu_start(int mode_in) {
    input_mode_t mode = (input_mode_t)mode_in;
    ui_input_init(&s_input, mode, qemu_buttons_read, NULL);
    controls_bind(&s_controls, &s_input, QEMU_WANTED);

    /*
     * The UART is already installed by st7735_qemu.c -- it owns the port and
     * the framebuffer is the higher-traffic direction. Starting a second
     * driver on the same port would fail, so this only adds a reader.
     */
    xTaskCreatePinnedToCore(input_task, "input_qemu", 3072, NULL, 4, NULL, 0);
}
