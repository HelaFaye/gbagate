/*
 * net_esp32s3.h -- the board's WiFi: a network to join, a hotspot to set it
 * up from, the setup page, the /gblink WebSocket, and Celio's socket.
 */
#ifndef NET_ESP32S3_H
#define NET_ESP32S3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare/start the shared Wi-Fi driver. The default netifs and event loop
 * are process-lifetime objects; radio_stop releases the driver but retains
 * them so Online and ESP-NOW can take turns. Idempotent.
 */
esp_err_t radio_init(void);

/* Is the station associated (so the channel is the access point's, not one
 * ESP-NOW can choose)? */
bool radio_sta_connected(void);

/* Compatibility entry point for Online start. net_resume() is the reversible
 * start operation; net_suspend() quiesces the Celio/HTTP/reconnect clients
 * and stops the driver before another radio mode is allowed to start.
 */
esp_err_t net_start(bool hotspot);

/* Reversible Online ownership: stop/restore services without destroying the
 * process-global netif or event-loop objects. Both calls are idempotent. */
esp_err_t net_suspend(void);
esp_err_t net_resume(bool hotspot);

/* Stop/start the initialized Wi-Fi driver while retaining its IDF objects.
 * Call radio_stop only after the active protocol client has relinquished it. */
esp_err_t radio_stop(void);

/* At boot: a network is stored, so come up without being asked (and /gblink
 * is there for a PC on the same WiFi). */
void net_boot(void);

typedef enum {
    NET_OFF = 0,
    NET_SETUP,       /* nothing stored: hotspot only */
    NET_JOINING,
    NET_UP,
    NET_FAILED,      /* would not join; hotspot up   */
} net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    char ip[16];
    char hotspot[33];     /* the hotspot's name while it is up, else "" */
    bool gblink_client;   /* a host is on /gblink                       */
} net_view_t;

void net_view(net_view_t *out);

/* The session code to join, from the stored config ("" to create one). */
void net_session(char *out, size_t cap);
/* Forget it: a code is used once. */
void net_clear_session(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_ESP32S3_H */
