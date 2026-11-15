/*
 * peer_espnow_esp32s3.c -- the wireless link cable's radio: ESP-NOW, board to
 * board, no access point.
 *
 *   Finding the peer   broadcast until gbl_peer accepts a datagram (a HELLO
 *                      in our mode, or one of the session), then lock to that
 *                      sender's MAC (gblink_peer_radio_accepted, on the
 *                      cable's thread) and send to it unicast, which gets
 *                      ESP-NOW's link-layer ACK and retries on top of
 *                      gbl_peer's own. A board in another mode is never
 *                      locked to, so it cannot keep the cable from pairing.
 *   Letting go         gblink_peer_start() or GBL_PEER_TIMEOUT_MS of silence
 *                      unlocks, so a board that is switched for another finds
 *                      the new one.
 *   Strangers          only 'G' 'P' datagrams of the right size are passed
 *                      on; gbl_peer checks the rest (version, CRC, mode,
 *                      epoch). A third board in the same mode cannot take a
 *                      locked cable, but can win the race to lock a fresh one
 *                      -- the link screen shows the peer's MAC so that is
 *                      visible.
 *
 * The radio coordinator stops Online before this client starts. radio_init()
 * then brings the Wi-Fi driver up in station mode; in this exclusive mode the
 * peers use PEER_CHANNEL rather than sharing an active Online connection.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <string.h>

#include "esp_check.h"
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
#include "net_esp32s3.h"

static const char *TAG = "peer_now";

/* Both boards must agree. Channel 1 is in every regulatory domain. */
#define PEER_CHANNEL 1

static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t  s_peer[ESP_NOW_ETH_ALEN];
static volatile bool s_locked;   /* s_peer is written before this is set */
static volatile int64_t s_last_rx_us;
static bool     s_up;
static bool     s_peer_running;
static uint8_t  s_mode = 0x01;
static uint8_t  s_variant;

static esp_err_t add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.channel = 0;   /* whatever channel the radio is on: 1, or the network's */
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    return esp_now_add_peer(&p);
}

static void rx_cb(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
    if (len < 15 || len > (int)GBL_PEER_MAX_DATAGRAM || d[0] != 'G' || d[1] != 'P') return;
    int64_t now = esp_timer_get_time();
    if (s_locked && now - s_last_rx_us > (int64_t)GBL_PEER_TIMEOUT_MS * 1000) {
        s_locked = false;   /* the locked peer went quiet: open the cable again */
    }
    if (s_locked && memcmp(info->src_addr, s_peer, ESP_NOW_ETH_ALEN) != 0) return;
    if (s_locked) s_last_rx_us = now;
    gblink_peer_radio_rx_from(d, (size_t)len, info->src_addr);
}

/* On the cable's thread: gbl_peer took a datagram from src. */
void gblink_peer_radio_accepted(const uint8_t src[6]) {
    if (s_locked) {
        if (memcmp(src, s_peer, ESP_NOW_ETH_ALEN) == 0) s_last_rx_us = esp_timer_get_time();
        return;
    }
    memcpy(s_peer, src, ESP_NOW_ETH_ALEN);
    esp_err_t e = add_peer(s_peer);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "could not add locked peer: %s", esp_err_to_name(e));
        return;
    }
    s_last_rx_us = esp_timer_get_time();
    __atomic_store_n(&s_locked, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "cable locked to " MACSTR, MAC2STR(s_peer));
}

bool gblink_peer_radio_send(const uint8_t *d, size_t n) {
    if (!s_up) return false;
    return esp_now_send(s_locked ? s_peer : BCAST, d, n) == ESP_OK;
}

esp_err_t wireless_cable_stop_checked(void);

esp_err_t peer_espnow_start(void) {
    if (s_up) {
        s_locked = false;
        return ESP_OK;
    }

    /* Online must have suspended before this client is entered. radio_init is
     * idempotent and starts the already-initialized Wi-Fi driver when needed. */
    esp_err_t e = radio_init();
    if (e != ESP_OK) return e;
    /* A connected station owns the channel; otherwise both peers use 1. */
    if (!radio_sta_connected()) {
        e = esp_wifi_set_channel(PEER_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (e != ESP_OK) return e;
    } else {
        ESP_LOGI(TAG, "on WiFi: the cable uses the network's channel");
    }

    e = esp_now_init();
    if (e != ESP_OK) return e;
    s_up = true;  /* from here on the stop callback can unwind partial setup */
    e = esp_now_register_recv_cb(rx_cb);
    if (e == ESP_OK) e = add_peer(BCAST);
    if (e != ESP_OK) {
        esp_err_t cleanup = esp_now_deinit();
        if (cleanup == ESP_OK) s_up = false;
        else ESP_LOGE(TAG, "partial ESP-NOW setup cleanup failed: %s",
                      esp_err_to_name(cleanup));
        return e;
    }
    s_locked = false;
    memset(s_peer, 0, sizeof s_peer);
    return ESP_OK;
}

/* Start the cable for a GB-Link mode: 0x01 Gen 3 relay, 0x04 Advance Wars. */
esp_err_t wireless_cable_start(uint8_t mode, uint8_t variant) {
    esp_err_t e = peer_espnow_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio failed: %s", esp_err_to_name(e));
        return e;
    }
    s_mode = mode;
    s_variant = variant;
    if (!gblink_peer_start_wait(mode, variant, esp_random() | 1u)) {
        ESP_LOGE(TAG, "peer worker did not acknowledge start");
        (void)wireless_cable_stop_checked();
        return ESP_ERR_TIMEOUT;
    }
    s_peer_running = true;
    return ESP_OK;
}

/* A failed stop must not permit a second radio owner. If deinit/driver stop
 * fails, try to restore the same Wireless client before reporting failure. */
esp_err_t wireless_cable_stop_checked(void) {
    const bool restore_peer = s_peer_running;
    if (!gblink_peer_stop_wait()) {
        ESP_LOGE(TAG, "peer worker did not acknowledge stop");
        return ESP_ERR_TIMEOUT;
    }
    s_peer_running = false;
    s_locked = false;
    memset(s_peer, 0, sizeof s_peer);
    s_last_rx_us = 0;

    if (s_up) {
        esp_err_t e = esp_now_deinit();
        if (e != ESP_OK) {
            /* A failed deinit leaves s_up true: do not register a duplicate
             * callback. The existing callback and broadcast peer remain live;
             * restart only the protocol worker before returning the failure. */
            if (!restore_peer) return e;  /* failed partial start: remain stopped */
            if (gblink_peer_start_wait(s_mode, s_variant, esp_random() | 1u)) {
                s_peer_running = true;
                return e;  /* original owner is running again */
            }
            ESP_LOGE(TAG, "ESP-NOW deinit failed (%s); peer worker restore failed",
                     esp_err_to_name(e));
            return e;
        }
        s_up = false;
    }

    esp_err_t e = radio_stop();
    if (e != ESP_OK) {
        esp_err_t restore = peer_espnow_start();
        if (restore == ESP_OK &&
            (!restore_peer || gblink_peer_start_wait(s_mode, s_variant, esp_random() | 1u))) {
            s_peer_running = restore_peer;
            return e;  /* restore the old owner; do not start another client */
        }
        ESP_LOGE(TAG, "Wi-Fi stop failed (%s); Wireless restore failed (%s)",
                 esp_err_to_name(e), esp_err_to_name(restore));
        return restore != ESP_OK ? restore : ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void wireless_cable_stop(void) {
    (void)wireless_cable_stop_checked();
}

bool wireless_cable_peer_mac(uint8_t mac[6]) {
    if (!s_locked) return false;
    memcpy(mac, s_peer, 6);
    return true;
}
