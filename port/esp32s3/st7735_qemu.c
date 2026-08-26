/*
 * st7735_qemu.c -- a stand-in for the ST7735 panel under QEMU.
 *
 * QEMU has no SPI device on the other end of the panel bus, so the real driver
 * (st7735_esp32s3.c) has nothing to talk to: its address windows and RAMWR
 * bursts go into a void and nothing can ever be seen. This file stands in for
 * the panel the same way transport_uart.c stands in for the GBA PHY -- it keeps
 * the framebuffer the firmware already renders into, and instead of clocking it
 * out over SPI it streams it out a spare UART that QEMU maps onto a socket.
 *
 * The framebuffer, the geometry and the RGB565 format are all exactly what the
 * firmware and the host renderer (test/test_render.c) use, so what comes out
 * here is the firmware's real screen -- not a QEMU-shaped approximation of it.
 * qemu/fbgrab.py reads the stream and writes the same P6 PPM the render test
 * does.
 *
 * Wire format, little-endian, one frame:
 *
 *     "FBUF"  magic (4 bytes)
 *     width   u16
 *     height  u16
 *     pixels  width*height*2 bytes, RGB565, row major
 *
 * Only the symbols the application actually links against are provided:
 * st7735_init / st7735_framebuffer / st7735_mark_dirty / st7735_mark_all_dirty
 * / st7735_flush. The hardware scroll path in st7735_esp32s3.c is SPI-only and
 * nothing in the app calls it, so it is simply absent here.
 */

#include <stddef.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ui_list.h"   /* UI_SCREEN_W/H, UI_FRAME_BYTES */

static const char *TAG = "st7735_qemu";

/*
 * Which UART the tap uses. UART2 by default: not UART0 (the ESP-IDF console)
 * and not UART1 (link_proto, see transport_uart.c), so nothing is multiplexed.
 * QEMU connects each UART to a -serial chardev in order, so the third -serial
 * in qemu/run.sh is this one.
 *
 * That is only true of a QEMU whose esp32s3 machine actually attaches three
 * UARTs. One that attaches two discards everything written here *silently* --
 * the chardev socket still opens and still accepts a client -- so the symptom
 * is a viewer that connects, reports no error and draws nothing forever.
 * qemu/uart2check.sh tells the two apart by capturing all three UARTs to files
 * in one boot; the top-level CMakeLists documents the fallbacks. Set
 * -D QEMU_AUX_UART=0 to share the console instead, or =1 to take link_proto's.
 *
 * Pins are irrelevant under emulation -- QEMU wires the UART peripheral, not
 * the GPIO matrix -- but they are set for form on a port of our own. When the
 * console's port is shared they are deliberately left alone; see st7735_init.
 */
#ifndef QEMU_AUX_UART
#define QEMU_AUX_UART 2
#endif

#ifdef CONFIG_ESP_CONSOLE_UART_NUM
#define FB_CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#else
#define FB_CONSOLE_UART 0
#endif

/* Sharing means another writer (the log) is on this port, which changes both
 * how the port may be configured and how a frame has to be written. */
#define FB_SHARED (QEMU_AUX_UART == FB_CONSOLE_UART)

#define FB_UART   ((uart_port_t)QEMU_AUX_UART)
#define FB_TX_PIN 16
#define FB_RX_PIN 15
#define FB_BAUD   1500000
#define FB_TXBUF  4096

/*
 * When the console is shared, route the log through the same UART driver the
 * tap uses. Both writers then go through uart_write_bytes() and the driver's
 * per-port TX mutex genuinely serialises them.
 *
 * Without this the atomicity below is a fiction: ESP-IDF's console VFS writes
 * straight into the TX FIFO by default, while the driver's ISR is separately
 * draining a frame out of its ring buffer, so a log line interleaves *inside* a
 * frame rather than between two.
 *
 * The call was renamed in ESP-IDF 5.3 (esp_vfs_dev_uart_use_driver ->
 * uart_vfs_dev_use_driver) and the old header has since been dropped, so take
 * whichever this IDF actually has. If neither is present the tap still works --
 * frames are simply corrupted whenever a log line coincides with one, and
 * viewer.py drops those and resynchronises on the next.
 */
#if FB_SHARED && defined(__has_include)
#  if __has_include("driver/uart_vfs.h")
#    include "driver/uart_vfs.h"
#    define FB_LOG_VIA_DRIVER() uart_vfs_dev_use_driver(QEMU_AUX_UART)
#  elif __has_include("esp_vfs_dev.h")
#    include "esp_vfs_dev.h"
#    define FB_LOG_VIA_DRIVER() esp_vfs_dev_uart_use_driver(QEMU_AUX_UART)
#  endif
#endif
#ifndef FB_LOG_VIA_DRIVER
#define FB_LOG_VIA_DRIVER() ((void)0)
#endif

/* How often the current framebuffer is pushed even if nothing changed, so a
 * grabber that attaches late still gets a frame promptly. */
#define FB_PERIOD_MS 400

/*
 * The header lives immediately in front of the pixels rather than in a local,
 * so a whole frame leaves in a single uart_write_bytes() call.
 *
 * That is what makes QEMU_AUX_UART=0 usable. uart_write_bytes() takes the
 * driver's per-port TX mutex for the duration of the call, so one call is
 * atomic with respect to any other task writing to the same port: a log line
 * can be emitted before a frame or after it, but not spliced into the middle of
 * one. Frames therefore stay intact and only the gaps between them carry junk,
 * which is exactly the case viewer.py's resynchroniser already handles.
 *
 * hdr is 8 bytes and px needs only 2-byte alignment, so the compiler cannot
 * insert padding between them; the assertions below hold that guarantee.
 */
typedef struct {
    uint8_t  hdr[8];
    uint16_t px[UI_SCREEN_W * UI_SCREEN_H];
} fb_frame_t;

_Static_assert(offsetof(fb_frame_t, px) == 8,
               "FBUF header must sit directly in front of the pixels");
_Static_assert(sizeof(fb_frame_t) == 8 + UI_FRAME_BYTES,
               "frame buffer must be exactly header + pixels, no padding");

static fb_frame_t s_frame;

static SemaphoreHandle_t s_lock;
static bool             s_uart_up;

/* ---- the tap ----------------------------------------------------------- */

static void fb_send_locked(void) {
    /* One call: see the note on s_frame. */
    uart_write_bytes(FB_UART, (const char *)&s_frame, sizeof s_frame);
}

static void fb_tap_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_uart_up && s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            fb_send_locked();
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(FB_PERIOD_MS));
    }
}

/* ---- panel API (the subset the app uses) ------------------------------- */

esp_err_t st7735_init(int panel) {
    (void)panel;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* The header never changes, so write it once and let every frame go out as
     * one buffer from then on. */
    s_frame.hdr[0] = 'F'; s_frame.hdr[1] = 'B';
    s_frame.hdr[2] = 'U'; s_frame.hdr[3] = 'F';
    s_frame.hdr[4] = (uint8_t)(UI_SCREEN_W & 0xFF);
    s_frame.hdr[5] = (uint8_t)(UI_SCREEN_W >> 8);
    s_frame.hdr[6] = (uint8_t)(UI_SCREEN_H & 0xFF);
    s_frame.hdr[7] = (uint8_t)(UI_SCREEN_H >> 8);
    memset(s_frame.px, 0, sizeof s_frame.px);

    /* Someone may already own the port -- the console always does when it is
     * shared, and input_qemu.c is deliberately a reader only -- so adopt an
     * existing driver rather than failing on a second install. */
    esp_err_t err = ESP_OK;
    if (!uart_is_driver_installed(FB_UART)) {
        err = uart_driver_install(FB_UART, FB_TXBUF, FB_TXBUF, 0, NULL, 0);
    }

#if !FB_SHARED
    /* Only safe on a port of our own. Re-running param_config/set_pin on the
     * console's UART would reset its baud out from under the log output and
     * remap its pins, which on real silicon is how you lose the console for
     * good -- and it buys nothing, since QEMU models neither. */
    if (err == ESP_OK) {
        const uart_config_t cfg = {
            .baud_rate  = FB_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        err = uart_param_config(FB_UART, &cfg);
        if (err == ESP_OK) err = uart_set_pin(FB_UART, FB_TX_PIN, FB_RX_PIN,
                                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
#endif

    if (err != ESP_OK) {
        /* No display tap, but the firmware must still run: it renders into the
         * framebuffer regardless, and the rest of the gate does not depend on
         * the panel. Report success so app_main keeps the display feature. */
        ESP_LOGW(TAG, "UART%d tap unavailable (%s); framebuffer kept, not streamed",
                 FB_UART, esp_err_to_name(err));
        return ESP_OK;
    }

    /* Must come after the install -- the console VFS can only be pointed at a
     * driver that exists. No-op unless the console is the shared port. */
    FB_LOG_VIA_DRIVER();

    s_uart_up = true;
    xTaskCreatePinnedToCore(fb_tap_task, "fbtap", 4096, NULL, 3, NULL, 0);
    /* The port is named in the log on purpose: it is the only way to confirm
     * from outside that the image was built for the -serial layout the runner
     * set up. qemu/uart2check.sh reads this line. */
    ESP_LOGI(TAG, "framebuffer tap on UART%d (%dx%d RGB565)%s; qemu/fbgrab.py reads it",
             FB_UART, UI_SCREEN_W, UI_SCREEN_H,
             FB_SHARED ? ", shared with the console" : "");
    return ESP_OK;
}

uint16_t *st7735_framebuffer(void) { return s_frame.px; }

/* Dirty tracking is a no-op here: there is no SPI cost to save, and the tap
 * always sends the whole frame. Kept so gate_app.c links unchanged. */
void st7735_mark_dirty(uint8_t y, uint8_t h) { (void)y; (void)h; }
void st7735_mark_all_dirty(void) { }

size_t st7735_flush(void) {
    if (!s_uart_up || !s_lock) {
        return 0;
    }
    /* Push immediately so a change shows without waiting for the periodic tap. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fb_send_locked();
    xSemaphoreGive(s_lock);
    return UI_FRAME_BYTES;
}
