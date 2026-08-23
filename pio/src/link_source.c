/*
 * link_source.c -- see link_source.h
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "link_bridge.h"
#include "link_source.h"

static const char *TAG = "linksrc";

/* ---- bridge-backed source (QEMU, and any host-driven test) -------------- */

/*
 * One transfer in flight at a time, which matches the hardware: the parent
 * clocks, everyone answers, done. A queue of depth one makes a late host
 * visible as a timeout rather than as a growing backlog that hides the
 * problem.
 */
static QueueHandle_t s_from_host;
static uint16_t      s_staged = 0xFFFF;
static gba_transfer_t s_last;
static uint32_t      s_seq;

extern link_bridge_t *tasks_bridge(void);

/* Called by the bridge when the host reports what the game sent. */
void link_source_feed(uint16_t seq, uint16_t game_word) {
    s_seq = seq;
    if (s_from_host) {
        /* Overwrite rather than block: the newest word is the only useful
         * one, and blocking here would stall the transport task. */
        xQueueOverwrite(s_from_host, &game_word);
    }
}

static bool bridge_poll(uint16_t *from_game, uint32_t timeout_ms) {
    if (!s_from_host) {
        return false;
    }
    uint16_t w;
    if (xQueueReceive(s_from_host, &w, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    *from_game = w;

    memset(&s_last, 0, sizeof s_last);
    s_last.word[0] = w;             /* parent: the game            */
    s_last.word[1] = 0xFFFF;        /* filled in by bridge_stage() */
    s_last.word[2] = 0xFFFF;
    s_last.word[3] = 0xFFFF;
    s_last.players = 2;
    s_last.self_id = 1;             /* the gate is player 2        */
    return true;
}

static void bridge_stage(uint16_t word) {
    s_staged = word;
    s_last.word[1] = word;
    /* Report immediately: the host is waiting for this before it can run the
     * game's next frame. */
    link_bridge_t *b = tasks_bridge();
    if (b) {
        bridge_report(b, &s_last, (uint16_t)s_seq, false);
    }
}

static void bridge_last(gba_transfer_t *out) {
    *out = s_last;
}

static const link_source_t k_bridge = {
    .name = "bridge",
    .poll = bridge_poll,
    .stage = bridge_stage,
    .last_transfer = bridge_last,
};

const link_source_t *link_source_bridge(void) {
    if (!s_from_host) {
        s_from_host = xQueueCreate(1, sizeof(uint16_t));
    }
    return &k_bridge;
}

/* ---- hardware ----------------------------------------------------------- */

#if !defined(GBAGATE_QEMU)

extern bool gba_link_poll(uint16_t *from_game, uint32_t timeout_ms);
extern void gba_link_stage(uint16_t word);
extern void gba_link_last(gba_transfer_t *out);

static const link_source_t k_hardware = {
    .name = "phy",
    .poll = gba_link_poll,
    .stage = gba_link_stage,
    .last_transfer = gba_link_last,
};

const link_source_t *link_source_hardware(void) { return &k_hardware; }

#else

const link_source_t *link_source_hardware(void) { return NULL; }

#endif

/* ---- selection ---------------------------------------------------------- */

const link_source_t *link_source_active(void) {
#if defined(GBAGATE_QEMU)
    /*
     * QEMU has no pins. Using the PHY here would busy-wait on a clock edge
     * that never comes, and the failure would look like a hang rather than a
     * missing peripheral.
     */
    static bool announced;
    if (!announced) {
        ESP_LOGI(TAG, "no PHY under emulation; taking transfers from the bridge");
        announced = true;
    }
    return link_source_bridge();
#else
    return link_source_hardware();
#endif
}
