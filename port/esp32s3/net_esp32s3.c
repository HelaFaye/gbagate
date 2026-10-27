/*
 * net_esp32s3.c -- the board's WiFi.
 *
 *   joining        the network wifi_cfg_t names (src/wifi_cfg.h), from NVS.
 *                  Retried with a growing pause; after a few failures the
 *                  setup hotspot comes up beside it, so a wrong password can
 *                  be fixed from a phone.
 *   hotspot        "gbagate-XXXX" (the last two bytes of its MAC), open
 *                  unless a hotspot password is set, with the setup page at
 *                  http://192.168.4.1/.
 *   setup page     GET / and POST /save, built by wifi_setup_html().
 *   wifi.txt       read from the USB drive whenever the drive is the
 *                  board's (boot, or a PC ejecting it), imported if it
 *                  changed since the last import.
 *   /gblink        a WebSocket on port 80: GB-Link's CDC framing in binary
 *                  messages, for a host on the same network (PATCHES.md P15).
 *                  One host at a time; a new one replaces the old.
 *   Celio          the WebSocket to the Celio server for celio_glue.cpp,
 *                  over tcp_transport's WebSocket (ESP-IDF's own; the
 *                  esp_websocket_client component is a registry download the
 *                  CI image cannot fetch). TLS with the certificate bundle.
 *
 * NOT YET RUN ON HARDWARE. The protocol logic it carries -- the Celio client,
 * the config and the page -- is host-tested (test_celio, test_wificfg, and
 * test/gblink_compat/test_celio_device.mjs against the real server); this
 * file is the ESP-IDF calls around it.
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gblink_port.h"
#include "net_esp32s3.h"
#include "settings.h"
#include "wifi_cfg.h"

static const char *TAG = "net";

#define NVS_NS        "gbagate"
#define NVS_KEY_CFG   "wifi"
#define NVS_KEY_TXT   "wifitxt"   /* CRC of the last wifi.txt imported */
#define WS_RX_MAX     2048
#define JOIN_TRIES    4           /* then the hotspot comes up too     */

/* ---- state ----------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;       /* everything below                  */
static wifi_cfg_t  s_cfg;
static net_state_t s_state;
static char        s_ip[16];
static char        s_hotspot[33];
static bool        s_ap_on;
static bool        s_started;
static int         s_tries;
static volatile bool s_sta_connected;
static volatile bool s_import_wanted;
static TaskHandle_t s_task;

static httpd_handle_t s_httpd;
static volatile int   s_gblink_fd = -1;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ---- NVS ------------------------------------------------------------------- */

static void cfg_load(wifi_cfg_t *c) {
    uint8_t buf[WIFI_CFG_BLOB_MAX];
    size_t n = sizeof buf;
    nvs_handle_t h;
    wifi_cfg_defaults(c);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, NVS_KEY_CFG, buf, &n) == ESP_OK) {
        wifi_cfg_unpack(c, buf, n);   /* corrupt: defaults */
    }
    nvs_close(h);
}

static bool cfg_save(const wifi_cfg_t *c) {
    uint8_t buf[WIFI_CFG_BLOB_MAX];
    size_t n = wifi_cfg_pack(c, buf, sizeof buf);
    nvs_handle_t h;
    if (n == 0 || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, NVS_KEY_CFG, buf, n) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---- the radio --------------------------------------------------------------- */

esp_err_t radio_init(void) {
    static bool up;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    bool first = !up;
    up = true;
    taskEXIT_CRITICAL(&mux);
    if (!first) {
        return ESP_OK;
    }
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    return ESP_OK;
}

bool radio_sta_connected(void) {
    return s_sta_connected;
}

static void apply_sta(void) {
    wifi_config_t w = { 0 };
    strncpy((char *)w.sta.ssid, s_cfg.ssid, sizeof w.sta.ssid);
    strncpy((char *)w.sta.password, s_cfg.pass, sizeof w.sta.password);
    w.sta.threshold.authmode = s_cfg.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    w.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &w) != ESP_OK) {
        ESP_LOGE(TAG, "station config refused");
        return;
    }
    s_tries = 0;
    s_state = NET_JOINING;
    s_ip[0] = '\0';
    esp_wifi_connect();
}

static void hotspot(bool on) {
    if (on == s_ap_on) {
        return;
    }
    if (on) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_hotspot, sizeof s_hotspot, "gbagate-%02X%02X", mac[4], mac[5]);
        wifi_config_t w = { 0 };
        strcpy((char *)w.ap.ssid, s_hotspot);
        w.ap.ssid_len = (uint8_t)strlen(s_hotspot);
        strncpy((char *)w.ap.password, s_cfg.ap_pass, sizeof w.ap.password);
        w.ap.authmode = s_cfg.ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        w.ap.max_connection = 2;
        w.ap.channel = 1;   /* followed by the station's once it joins */
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &w);
        ESP_LOGI(TAG, "setup hotspot %s up", s_hotspot);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_hotspot[0] = '\0';
    }
    s_ap_on = on;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        lock();
        if (s_cfg.ssid[0]) {
            s_tries++;
            s_ip[0] = '\0';
            if (s_tries >= JOIN_TRIES) {
                s_state = NET_FAILED;
                hotspot(true);
            } else {
                s_state = NET_JOINING;
            }
        }
        unlock();
        if (s_task) xTaskNotifyGive(s_task);   /* retry, after a pause, there */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        lock();
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_UP;
        s_tries = 0;
        unlock();
        ESP_LOGI(TAG, "on %s at %s", s_cfg.ssid, s_ip);
    }
}

/* ---- wifi.txt ------------------------------------------------------------------ */

/* From usb_msc_esp32s3.c; an image without the drive reads nothing. */
__attribute__((weak)) bool msc_read_text(const char *name, char *buf, size_t cap, size_t *len) {
    (void)name; (void)buf; (void)cap; (void)len;
    return false;
}

/* From the drive driver, in whatever task handed the volume back. */
void msc_on_app_mounted(void) {
    s_import_wanted = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void import_wifi_txt(void) {
    static char text[1024];
    size_t n;
    if (!msc_read_text("wifi.txt", text, sizeof text, &n)) {
        return;
    }
    uint32_t crc = settings_crc32((const uint8_t *)text, n), last = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_get_u32(h, NVS_KEY_TXT, &last);
    if (crc == last) {
        nvs_close(h);
        return;   /* already taken: the stored config may have moved on since */
    }
    lock();
    wifi_cfg_t c = s_cfg;
    unlock();
    wifi_parse_t r;
    wifi_cfg_parse_text(&c, text, n, &r);
    ESP_LOGI(TAG, "wifi.txt: %u taken, %u unknown, %u refused (first at line %u)",
             r.applied, r.unknown, r.rejected, r.first_bad);
    nvs_set_u32(h, NVS_KEY_TXT, crc);
    nvs_commit(h);
    nvs_close(h);
    lock();
    bool net_changed = strcmp(c.ssid, s_cfg.ssid) || strcmp(c.pass, s_cfg.pass);
    s_cfg = c;
    cfg_save(&s_cfg);
    if (s_started && net_changed && s_cfg.ssid[0]) {
        apply_sta();
    }
    unlock();
}

/* ---- the setup page --------------------------------------------------------------- */

static void page_status(char *out, size_t cap) {
    switch (s_state) {
    case NET_UP:      snprintf(out, cap, "On %s at %s.", s_cfg.ssid, s_ip); break;
    case NET_JOINING: snprintf(out, cap, "Joining %s...", s_cfg.ssid); break;
    case NET_FAILED:  snprintf(out, cap, "Could not join %s. Check the name and password.", s_cfg.ssid); break;
    default:          snprintf(out, cap, "No WiFi network set yet."); break;
    }
}

static esp_err_t send_page(httpd_req_t *req, const char *notice) {
    static char html[WIFI_PAGE_MAX];   /* httpd runs one handler at a time */
    char status[96];
    gblink_celio_view_t cv;
    gblink_celio_view(&cv);
    lock();
    page_status(status, sizeof status);
    wifi_page_t pg = { status, notice, cv.state == 3 /* waiting */ ? cv.session : NULL };
    size_t n = wifi_setup_html(html, sizeof html, &s_cfg, &pg);
    unlock();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, (ssize_t)n);
}

static esp_err_t get_root(httpd_req_t *req) {
    return send_page(req, NULL);
}

static esp_err_t post_save(httpd_req_t *req) {
    static char body[1024];
    if (req->content_len >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "too long");
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    lock();
    wifi_cfg_t c = s_cfg;
    wifi_parse_t r;
    wifi_cfg_parse_form(&c, body, got, &r);
    bool net_changed = strcmp(c.ssid, s_cfg.ssid) || strcmp(c.pass, s_cfg.pass);
    s_cfg = c;
    bool saved = cfg_save(&s_cfg);
    if (net_changed && s_cfg.ssid[0]) {
        apply_sta();
    }
    unlock();
    char notice[96];
    if (!saved) {
        snprintf(notice, sizeof notice, "Could not save.");
    } else if (r.rejected) {
        snprintf(notice, sizeof notice, "Saved, but %u field(s) were not valid and were kept as they were.",
                 r.rejected);
    } else {
        snprintf(notice, sizeof notice, net_changed ? "Saved. Joining the network now." : "Saved.");
    }
    return send_page(req, notice);
}

/* ---- /gblink ------------------------------------------------------------------------ */

static esp_err_t ws_gblink(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        s_gblink_fd = httpd_req_to_sockfd(req);   /* the newest host wins */
        ESP_LOGI(TAG, "/gblink host connected");
        return ESP_OK;
    }
    static uint8_t buf[WS_RX_MAX];
    httpd_ws_frame_t f = { 0 };
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &f, 0), TAG, "ws length");
    if (f.len > sizeof buf) {
        return ESP_FAIL;   /* drops the connection */
    }
    f.payload = buf;
    if (f.len) {
        ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &f, f.len), TAG, "ws payload");
    }
    if (f.type == HTTPD_WS_TYPE_BINARY && httpd_req_to_sockfd(req) == s_gblink_fd) {
        gblink_net_on_receive(buf, f.len);
    } else if (f.type == HTTPD_WS_TYPE_CLOSE) {
        s_gblink_fd = -1;
    }
    return ESP_OK;
}

bool gblink_net_write(const uint8_t *data, size_t len) {
    int fd = s_gblink_fd;
    if (fd < 0 || !s_httpd || httpd_ws_get_fd_info(s_httpd, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return false;
    }
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    if (httpd_ws_send_frame_async(s_httpd, fd, &f) != ESP_OK) {
        s_gblink_fd = -1;
        return false;
    }
    return true;
}

static void start_httpd(void) {
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.lru_purge_enable = true;
    c.core_id = 0;   /* core 1 is the link's */
    if (httpd_start(&s_httpd, &c) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        return;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    const httpd_uri_t gbl  = { .uri = "/gblink", .method = HTTP_GET, .handler = ws_gblink,
                               .is_websocket = true };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &gbl);
}

/* ---- the Celio socket ------------------------------------------------------------------ */

static SemaphoreHandle_t s_ws_lock;      /* the transport, between send and read */
static esp_transport_handle_t s_ws;      /* NULL while not connected             */
static volatile bool s_celio_wanted;
static TaskHandle_t s_ws_task;

void gblink_celio_ws_connect(bool on) {
    s_celio_wanted = on;
    if (on && s_ws_task) xTaskNotifyGive(s_ws_task);
}

bool gblink_celio_ws_send(const char *text, size_t len) {
    bool ok = false;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    if (s_ws) {
        ok = esp_transport_ws_send_raw(s_ws,
                 (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN),
                 text, (int)len, 2000) == (int)len;
    }
    xSemaphoreGive(s_ws_lock);
    return ok;
}

uint32_t gblink_celio_random(void) {
    return esp_random();
}

static void ws_session(const char *url) {
    bool tls;
    char host[96], path[160];
    uint16_t port;
    if (!wifi_url_split(url, &tls, host, sizeof host, &port, path, sizeof path)) {
        ESP_LOGE(TAG, "Celio URL %s is not one a socket can open", url);
        return;
    }
    esp_transport_list_handle_t list = esp_transport_list_init();
    esp_transport_handle_t base = tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (tls) {
        esp_transport_ssl_crt_bundle_attach(base, esp_crt_bundle_attach);
    }
    esp_transport_list_add(list, base, tls ? "ssl" : "tcp");
    esp_transport_handle_t ws = esp_transport_ws_init(base);
    esp_transport_list_add(list, ws, tls ? "wss" : "ws");
    esp_transport_ws_set_path(ws, path);

    if (esp_transport_connect(ws, host, port, 8000) < 0) {
        ESP_LOGW(TAG, "Celio %s:%u did not answer", host, port);
        esp_transport_list_destroy(list);
        return;
    }
    ESP_LOGI(TAG, "Celio socket open to %s", host);
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_ws_lock);
    gblink_celio_ws_open();

    static char frame[WS_RX_MAX];
    size_t have = 0, want = 0;
    while (s_celio_wanted) {
        /* Poll unlocked, read locked: TLS cannot read and write at once,
         * and a send must not wait out a read's timeout. */
        int p = esp_transport_poll_read(ws, 50);
        if (p < 0) break;
        if (p == 0) continue;
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        /* Data is waiting, so this returns at once -- unless a frame's
         * header is split across segments; a zero timeout could then leave
         * half a header read and the stream out of step, so give it a
         * second. A send waits that long only in that case. */
        int r = esp_transport_read(ws, frame + have, (int)(sizeof frame - 1 - have), 1000);
        int op = esp_transport_ws_get_read_opcode(ws);
        int total = esp_transport_ws_get_read_payload_len(ws);
        xSemaphoreGive(s_ws_lock);
        if (r < 0) break;
        if (op == WS_TRANSPORT_OPCODES_CLOSE) break;
        if (r == 0) continue;   /* a control frame the transport answered */
        if (have == 0) want = total > 0 ? (size_t)total : 0;
        have += (size_t)r;
        if (have >= want || have >= sizeof frame - 1) {
            if (want <= sizeof frame - 1 && (op == WS_TRANSPORT_OPCODES_TEXT || op == WS_TRANSPORT_OPCODES_CONT)) {
                gblink_celio_ws_text(frame, have);
            }   /* else too long for any frame Celio sends: dropped */
            have = want = 0;
        }
    }
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    s_ws = NULL;
    xSemaphoreGive(s_ws_lock);
    esp_transport_close(ws);
    esp_transport_list_destroy(list);
    gblink_celio_ws_closed();
    ESP_LOGI(TAG, "Celio socket closed");
}

static void ws_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!s_celio_wanted || s_state != NET_UP) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        char url[WIFI_URL_MAX + 1];
        lock();
        strcpy(url, s_cfg.celio);
        unlock();
        ws_session(url);
        vTaskDelay(pdMS_TO_TICKS(500));   /* socket.io's reconnection delay */
    }
}

/* ---- the network's own task ----------------------------------------------------------- */

static void net_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_import_wanted) {
            s_import_wanted = false;
            import_wifi_txt();
        }
        lock();
        bool retry = s_started && s_cfg.ssid[0] && !s_sta_connected && s_state != NET_UP;
        int tries = s_tries;
        unlock();
        if (retry) {
            /* 1, 2, 4, 8 s, then every 30: a network that comes back is found */
            vTaskDelay(pdMS_TO_TICKS(tries < JOIN_TRIES ? 1000u << tries : 30000u));
            if (!s_sta_connected) esp_wifi_connect();
        }
    }
}

static void ensure_tasks(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_ws_lock = xSemaphoreCreateMutex();
        cfg_load(&s_cfg);
    }
    if (!s_task) {
        xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 3, &s_task, 0);
    }
}

esp_err_t net_start(bool want_hotspot) {
    ensure_tasks();
    lock();
    if (!s_started) {
        esp_err_t e = radio_init();
        if (e != ESP_OK) {
            unlock();
            return e;
        }
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);
        start_httpd();
        xTaskCreatePinnedToCore(ws_task, "celio_ws", 8192, NULL, 3, &s_ws_task, 0);
        s_started = true;
        if (s_cfg.ssid[0]) {
            apply_sta();
        } else {
            s_state = NET_SETUP;
        }
    }
    if (want_hotspot || !s_cfg.ssid[0] || s_state == NET_FAILED) {
        hotspot(true);
    }
    unlock();
    return ESP_OK;
}

void net_boot(void) {
    ensure_tasks();
    s_import_wanted = true;          /* the drive may have mounted before us */
    xTaskNotifyGive(s_task);
    lock();
    bool have = s_cfg.ssid[0] != '\0';
    unlock();
    if (have) {
        net_start(false);
    }
}

void net_view(net_view_t *out) {
    memset(out, 0, sizeof *out);
    if (!s_lock) {
        return;
    }
    lock();
    out->state = s_started ? s_state : NET_OFF;
    snprintf(out->ssid, sizeof out->ssid, "%s", s_cfg.ssid);
    snprintf(out->ip, sizeof out->ip, "%s", s_ip);
    snprintf(out->hotspot, sizeof out->hotspot, "%s", s_ap_on ? s_hotspot : "");
    unlock();
    out->gblink_client = s_gblink_fd >= 0;
}

void net_session(char *out, size_t cap) {
    if (!s_lock) {
        if (cap) out[0] = '\0';
        return;
    }
    lock();
    snprintf(out, cap, "%s", s_cfg.session);
    unlock();
}

void net_clear_session(void) {
    if (!s_lock) return;
    lock();
    if (s_cfg.session[0]) {
        s_cfg.session[0] = '\0';
        cfg_save(&s_cfg);
    }
    unlock();
}
