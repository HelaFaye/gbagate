/*
 * peer_espnow_esp32s3.c -- the wireless link cable's radio: ESP-NOW, board to
 * board, no access point.
 *
 *   Finding the peer   broadcast until a datagram that gbl_peer accepts
 *                      arrives, then lock to that sender's MAC and send to it
 *                      unicast, which gets ESP-NOW's link-layer ACK and
 *                      retries on top of gbl_peer's own.
 *   Letting go         gblink_peer_start() or GBL_PEER_TIMEOUT_MS of silence
 *                      unlocks, so a board that is switched for another finds
 *                      the new one.
 *   Strangers          only 'G' 'P' datagrams of the right size are passed
 *                      on; gbl_peer checks the rest (version, CRC, epoch).
 *                      A third board on the channel cannot take a locked
 *                      cable, but can win the race to lock a fresh one -- the
 *                      link screen shows the peer's MAC so that is visible.
 *
 * The previous ESP-NOW transport (transport_esp32s3.c) called esp_now_init()
 * without starting WiFi, which fails, and had no caller. This brings WiFi up
 * in station mode on a fixed channel, which is all ESP-NOW needs.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "gbl_peer.h"
#include "gblink_port.h"

static const char *TAG = "peer_now";

/* Both boards must agree. Channel 1 is in every regulatory domain. */
#define PEER_CHANNEL 1

static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t  s_peer[ESP_NOW_ETH_ALEN];
static volatile bool s_locked;
static volatile int64_t s_last_rx_us;
static bool     s_up;

static void add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.channel = PEER_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static void rx_cb(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
    if (len < 15 || len > (int)GBL_PEER_MAX_DATAGRAM || d[0] != 'G' || d[1] != 'P') return;
    int64_t now = esp_timer_get_time();
    if (s_locked && now - s_last_rx_us > (int64_t)GBL_PEER_TIMEOUT_MS * 1000) {
        s_locked = false;   /* the locked peer went quiet: open the cable again */
    }
    if (s_locked && memcmp(info->src_addr, s_peer, ESP_NOW_ETH_ALEN) != 0) return;
    if (!s_locked) {
        memcpy(s_peer, info->src_addr, ESP_NOW_ETH_ALEN);
        add_peer(s_peer);
        s_locked = true;
        ESP_LOGI(TAG, "cable locked to " MACSTR, MAC2STR(s_peer));
    }
    s_last_rx_us = now;
    gblink_peer_radio_rx(d, (size_t)len);
}

bool gblink_peer_radio_send(const uint8_t *d, size_t n) {
    if (!s_up) return false;
    return esp_now_send(s_locked ? s_peer : BCAST, d, n) == ESP_OK;
}

esp_err_t peer_espnow_start(void) {
    if (!s_up) {
        esp_err_t e = esp_netif_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_channel(PEER_CHANNEL, WIFI_SECOND_CHAN_NONE));
        ESP_ERROR_CHECK(esp_now_init());
        ESP_ERROR_CHECK(esp_now_register_recv_cb(rx_cb));
        add_peer(BCAST);
        s_up = true;
    }
    s_locked = false;
    return ESP_OK;
}

/* Start the cable for a GB-Link mode: 0x01 Gen 3 relay, 0x04 Advance Wars. */
esp_err_t wireless_cable_start(uint8_t mode, uint8_t variant) {
    esp_err_t e = peer_espnow_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio failed: %s", esp_err_to_name(e));
        return e;
    }
    gblink_peer_start(mode, variant, esp_random() | 1u);
    return ESP_OK;
}

void wireless_cable_stop(void) {
    gblink_peer_stop();
    s_locked = false;
}

bool wireless_cable_peer_mac(uint8_t mac[6]) {
    if (!s_locked) return false;
    memcpy(mac, s_peer, 6);
    return true;
}
