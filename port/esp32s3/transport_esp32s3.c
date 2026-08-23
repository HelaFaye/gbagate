/*
 * transport_esp32s3.c -- USB CDC and ESP-NOW backends for link_bridge.
 *
 * Both reduce to one function: hand these bytes to the far end. The bridge
 * owns everything above that, so these files are thin on purpose.
 *
 * Not yet run on hardware.
 *
 * Core placement
 * --------------
 * Everything here runs on core 0, alongside WiFi, BLE, USB and the display.
 * Core 1 belongs to the link timing and must never block on a transport: a
 * radio that stalls for 3 ms is normal, and a link frame that stalls for 3 ms
 * is a dropped slot. The bridge is fed from here and read from there through
 * link_session's ring, which is the only thing crossing between them.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include "link_bridge.h"

static const char *TAG = "transport";

static link_bridge_t *s_bridge;

static uint32_t now_us(void) {
    return (uint32_t)esp_timer_get_time();
}

/* ---- USB CDC ----------------------------------------------------------- */

#define CDC_ITF TINYUSB_CDC_ACM_0

static bool cdc_tx(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    /*
     * queue then flush, rather than a blocking write: a host that has stopped
     * reading must not be able to stall this task. If the ring is full the
     * frame is dropped, which the bridge already treats as an underrun.
     */
    size_t queued = tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
    if (queued != len) {
        return false;
    }
    return tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(10)) == ESP_OK;
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t buf[64];
    size_t n = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof buf, &n) == ESP_OK && n > 0) {
        bridge_feed(s_bridge, buf, n, now_us());
        n = 0;
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event) {
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    /*
     * DTR dropping means the host closed the port. Announce again on the next
     * open rather than assuming the old session survived -- mGBA restarting is
     * exactly this case.
     */
    if (!dtr && s_bridge) {
        s_bridge->state = BRIDGE_DOWN;
        ESP_LOGI(TAG, "host closed the port");
    }
}

esp_err_t transport_cdc_start(link_bridge_t *bridge) {
    s_bridge = bridge;
    bridge_init(bridge, cdc_tx, NULL, /* stream = */ true, GBA_ROLE_MASTER);

    const tinyusb_config_cdcacm_t cfg = {
        .usb_dev                  = TINYUSB_USBDEV_0,
        .cdc_port                 = CDC_ITF,
        .callback_rx              = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&cfg), TAG, "cdc init");
    ESP_LOGI(TAG, "CDC transport up");
    return ESP_OK;
}

/* ---- ESP-NOW ----------------------------------------------------------- */

static uint8_t s_peer[ESP_NOW_ETH_ALEN];
static bool    s_peer_known;

static bool now_tx(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (!s_peer_known) {
        return false;
    }
    /*
     * esp_now_send is asynchronous and its payload limit is 250 bytes; every
     * packet this protocol produces is under 24, so a frame never splits.
     */
    return esp_now_send(s_peer, data, len) == ESP_OK;
}

static void now_rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0) {
        return;
    }
    /*
     * Learn the peer from the first valid packet. The bridge rejects malformed
     * and wrong-version packets, so a stray sender on the same channel cannot
     * take the slot -- but it can occupy it before the real peer speaks, which
     * is why re-pairing is a menu action rather than automatic.
     */
    if (!s_peer_known) {
        memcpy(s_peer, info->src_addr, ESP_NOW_ETH_ALEN);
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, s_peer, ESP_NOW_ETH_ALEN);
        peer.channel = 0;      /* follow the current WiFi channel */
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) == ESP_OK) {
            s_peer_known = true;
            ESP_LOGI(TAG, "peer %02X:%02X:%02X:%02X:%02X:%02X",
                     s_peer[0], s_peer[1], s_peer[2],
                     s_peer[3], s_peer[4], s_peer[5]);
        }
    }
    bridge_feed(s_bridge, data, (size_t)len, now_us());
}

esp_err_t transport_espnow_start(link_bridge_t *bridge, const uint8_t *peer_mac) {
    s_bridge = bridge;
    bridge_init(bridge, now_tx, NULL, /* stream = */ false, GBA_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(now_rx_cb), TAG, "recv cb");

    if (peer_mac) {
        memcpy(s_peer, peer_mac, ESP_NOW_ETH_ALEN);
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, s_peer, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.encrypt = false;
        ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add peer");
        s_peer_known = true;
    } else {
        /* Broadcast until someone answers, so two fresh boards find each
         * other without either being told the other's address. */
        static const uint8_t bcast[ESP_NOW_ETH_ALEN] =
            { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        memcpy(s_peer, bcast, ESP_NOW_ETH_ALEN);
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, s_peer, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        s_peer_known = true;
    }

    ESP_LOGI(TAG, "ESP-NOW transport up");
    return ESP_OK;
}

void transport_forget_peer(void) {
    if (s_peer_known) {
        esp_now_del_peer(s_peer);
        s_peer_known = false;
    }
}

/* ---- housekeeping ------------------------------------------------------ */

/*
 * Retries the announcement and notices a peer that went away. Called from the
 * UI loop on core 0, not from the link task: bridge_tick can emit a packet,
 * and core 1 must never touch a transport.
 */
void transport_tick(link_bridge_t *bridge) {
    bridge_tick(bridge, now_us());
}
