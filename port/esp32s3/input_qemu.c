/*
 * input_qemu.c -- keyboard input from the host, standing in for the encoder.
 *
 * There is no rotary encoder under emulation, and QEMU wires UART peripherals
 * rather than the GPIO matrix, so nothing the encoder driver reads will ever
 * change. This reads single-byte commands from the same UART2 socket that
 * st7735_qemu.c streams the framebuffer out of -- a QEMU tcp chardev is
 * bidirectional, so the read direction costs nothing and needs no extra port.
 *
 * The commands are deliberately one byte and human-typable, so the channel can
 * be driven by qemu/viewer.py, by a test script, or by hand with netcat when
 * something is behaving oddly:
 *
 *     u / A    encoder one detent anticlockwise   (up the list)
 *     d / B    encoder one detent clockwise       (down the list)
 *     U / D    ten detents, as an accelerated spin
 *     \r \n s  the encoder button: select
 *     b        back
 *     t        next library tab
 *     f        toggle favourite
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static void input_task(void *arg) {
    (void)arg;
    uint8_t buf[32];

    /*
     * st7735_qemu.c owns the port and installs the driver; if that failed it
     * took its degraded path and there is nothing to read from. Reading anyway
     * would return an error immediately every time and spin this task at full
     * tilt against a 50 ms timeout that never elapses, so check first and stand
     * down instead. The screen is already lost in that case; silently pinning a
     * core as well would only make the cause harder to see.
     */
    if (!uart_is_driver_installed(IN_UART)) {
        ESP_LOGW(TAG, "no UART%d driver; keyboard input disabled", IN_UART);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "keyboard input on UART%d: u/d step, U/D spin, enter select",
             IN_UART);

    for (;;) {
        int n = uart_read_bytes(IN_UART, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (n < 0) {
            /* Transient driver error rather than "no bytes" (which is 0). Back
             * off rather than retrying flat out. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (int i = 0; i < n; i++) {
            switch (buf[i]) {
            case 'u': case 'A': post(UI_EVENT_ENCODER, -1);  break;
            case 'd': case 'B': post(UI_EVENT_ENCODER, +1);  break;
            case 'U':           post(UI_EVENT_ENCODER, -10); break;
            case 'D':           post(UI_EVENT_ENCODER, +10); break;

            case '\r': case '\n': case 's':
                post(UI_EVENT_CHIP_SENT, 0);
                break;

            case 'b': post(UI_EVENT_CHIP_OUT, 0); break;
            case 't': post(UI_EVENT_ENCODER, 0);  break;  /* tab: handled in ui */
            case 'f': post(UI_EVENT_CHIP_IN, 0);  break;

            default:
                /* Ignore anything else in silence. The viewer sends arrow keys
                 * as escape sequences and only the final letter matters; the
                 * ESC and '[' land here and must not do anything. */
                break;
            }
        }
    }
}

void input_qemu_start(void) {
    /*
     * The UART is already installed by st7735_qemu.c -- it owns the port and
     * the framebuffer is the higher-traffic direction. Starting a second
     * driver on the same port would fail, so this only adds a reader.
     */
    xTaskCreatePinnedToCore(input_task, "input_qemu", 3072, NULL, 4, NULL, 0);
}
