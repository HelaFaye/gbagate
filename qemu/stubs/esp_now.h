#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#define ESP_NOW_ETH_ALEN 6
#define ESP_NOW_MAX_DATA_LEN 250
typedef struct { uint8_t src_addr[6]; uint8_t des_addr[6]; } esp_now_recv_info_t;
typedef struct { uint8_t peer_addr[6]; uint8_t lmk[16]; uint8_t channel; int ifidx; bool encrypt; void *priv; } esp_now_peer_info_t;
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *info, const uint8_t *data, int len);
typedef void (*esp_now_send_cb_t)(const uint8_t *mac, int status);
static inline esp_err_t esp_now_init(void) { return ESP_OK; }
static inline esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { (void)cb; return ESP_OK; }
static inline esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) { (void)cb; return ESP_OK; }
static inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t *p) { (void)p; return ESP_OK; }
static inline esp_err_t esp_now_del_peer(const uint8_t *mac) { (void)mac; return ESP_OK; }
static inline esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *d, size_t n) { (void)mac;(void)d;(void)n; return ESP_OK; }
