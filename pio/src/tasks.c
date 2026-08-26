/*
 * tasks.c -- the FreeRTOS layout.
 *
 * Two cores, and the split is the whole point:
 *
 *   core 1   the link. One task, top priority, IRAM resident, never blocks on
 *            anything that can be slow. It busy-waits on clock edges.
 *   core 0   everything else: WiFi, BLE, USB, filesystem, display, UI.
 *
 * Nothing crosses between them except link_session's ring and one queue of UI
 * events. That is deliberate: every additional shared structure is another
 * chance for core 0 to make core 1 wait, and core 1 waiting is a corrupt frame.
 *
 * Not yet run on hardware.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "link_bridge.h"
#include "variant.h"

static const char *TAG = "tasks";

#define CORE_LINK 1
#define CORE_APP  0

/*
 * Priorities. The link task sits above everything because a missed slot cannot
 * be recovered, while a late frame or a late packet can.
 */
#define PRIO_LINK      (configMAX_PRIORITIES - 1)
#define PRIO_UI        5
#define PRIO_TRANSPORT 4
#define PRIO_STORAGE   3

/*
 * The link task busy-waits, so it must yield often enough to feed the task
 * watchdog without giving up a transfer. One tick between transfers is plenty
 * at 1000 Hz and costs nothing: transfers are ~1 ms apart at best.
 */
#define LINK_IDLE_YIELD_TICKS 1

typedef enum {
    UI_EVENT_ENCODER = 0,
    UI_EVENT_CHIP_IN,
    UI_EVENT_CHIP_OUT,
    UI_EVENT_LINK_UP,
    UI_EVENT_LINK_DOWN,
    UI_EVENT_CHIP_SENT,
    UI_EVENT_USB_ATTACHED,
    UI_EVENT_USB_DETACHED
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int32_t         a;
    uint16_t        b;
} ui_event_t;

static QueueHandle_t s_ui_events;
static link_bridge_t s_bridge;

#ifdef GBAGATE_QEMU
/*
 * Under emulation there is no PHY and no USB host. The game's words arrive over
 * UART1 through the bridge instead, so these three pieces -- the UART transport,
 * the word delivery into the gate's link_source, and an open database to give
 * the gate an identity -- are wired here. On hardware the PHY and the UI do all
 * of this, which is why it is compiled in only for the QEMU images.
 */
extern esp_err_t transport_uart_start(link_bridge_t *bridge);
extern void      link_source_feed(uint16_t seq, uint16_t game_word);
extern bool      gate_open_database(const char *label);

/* Hand every inbound word from the bridge to the bridge-backed link_source so
 * gba_link_service() can poll it. Without this the gate's queue never fills and
 * no RESULT is ever produced under emulation. */
static void qemu_feed_word(void *ctx, uint16_t seq, uint16_t word) {
    (void)ctx;
    link_source_feed(seq, word);
}
#endif

/*
 * Posting from core 1 must never block: if the UI is busy the event is dropped
 * rather than stalling a transfer. Dropped counts are surfaced on the
 * diagnostics screen so a persistently full queue is visible rather than felt.
 */
static volatile uint32_t s_ui_events_dropped;

/*
 * From a task, not an ISR. Blocks briefly rather than dropping: a keypress the
 * user made must not vanish because the UI was mid-redraw, and the caller is
 * never the link task.
 */
bool tasks_post_ui_event(const ui_event_t *e) {
    if (!s_ui_events) {
        return false;
    }
    if (xQueueSend(s_ui_events, e, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_ui_events_dropped++;
        return false;
    }
    return true;
}

bool tasks_post_ui_event_from_link(const ui_event_t *e) {
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_ui_events, e, &woken) != pdTRUE) {
        s_ui_events_dropped++;
        return false;
    }
    portYIELD_FROM_ISR(woken);
    return true;
}

/* ---- core 1 ------------------------------------------------------------ */

extern void gba_link_service(void);   /* one transfer, or time out */

static void IRAM_ATTR link_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "link task on core %d", xPortGetCoreID());

    for (;;) {
        gba_link_service();
        /* Yield exactly one tick. Long enough for the watchdog, short enough
         * that the next transfer is never missed. */
        vTaskDelay(LINK_IDLE_YIELD_TICKS);
    }
}

/* ---- core 0 ------------------------------------------------------------ */

extern void ui_service(const ui_event_t *e, bool have_event);

static void ui_task(void *arg) {
    (void)arg;
    for (;;) {
        ui_event_t e;
        bool got = xQueueReceive(s_ui_events, &e, pdMS_TO_TICKS(16)) == pdTRUE;
        ui_service(got ? &e : NULL, got);
    }
}

#ifndef GBAGATE_QEMU
static void transport_task(void *arg) {
    (void)arg;
    for (;;) {
        /* bridge_tick can emit a packet, so it belongs here and not on core 1:
         * core 1 must never touch a transport. */
        bridge_tick(&s_bridge, (uint32_t)esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

/* ---- setup -------------------------------------------------------------- */

void tasks_start(uint32_t features) {
    s_ui_events = xQueueCreate(16, sizeof(ui_event_t));
    configASSERT(s_ui_events);

#ifdef GBAGATE_QEMU
    /*
     * Bring the emulated link up before the tasks that use it: open a default
     * database so the gate has an identity to announce (MMBN6 US = "bn6g"),
     * start the UART bridge, and route inbound words into link_source. The
     * uart_rx task drives bridge_tick itself, so the transport_task below is
     * not needed here.
     */
    gate_open_database("bn6g");
    if (transport_uart_start(&s_bridge) == ESP_OK) {
        s_bridge.on_word = qemu_feed_word;
    } else {
        ESP_LOGE(TAG, "UART bridge failed to start; the gate will not link");
    }
#endif

    if (features & (FEAT_LINK_MASTER | FEAT_LINK_SLAVE)) {
        xTaskCreatePinnedToCore(link_task, "link", 4096, NULL,
                                PRIO_LINK, NULL, CORE_LINK);
    }

    if (features & FEAT_DISPLAY) {
        xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL,
                                PRIO_UI, NULL, CORE_APP);
    }

#ifndef GBAGATE_QEMU
    if (features & (FEAT_ESPNOW | FEAT_USB_CDC)) {
        xTaskCreatePinnedToCore(transport_task, "transport", 4096, NULL,
                                PRIO_TRANSPORT, NULL, CORE_APP);
    }
#endif

#if defined(GBAGATE_QEMU)
    /*
     * No encoder exists under emulation, so keyboard bytes from the viewer
     * stand in. They arrive as the same ui_event_t the encoder would produce,
     * so nothing above this line is emulation-aware.
     */
    if (features & FEAT_DISPLAY) {
        extern void input_qemu_start(void);
        input_qemu_start();
    }
#endif

    ESP_LOGI(TAG, "tasks started for the %s image", GBAGATE_VARIANT_NAME);
}

link_bridge_t *tasks_bridge(void) {
    return &s_bridge;
}

uint32_t tasks_ui_events_dropped(void) {
    return s_ui_events_dropped;
}
