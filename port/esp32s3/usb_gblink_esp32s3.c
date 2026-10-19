/*
 * usb_gblink_esp32s3.c -- the GB-Link USB personality on the ESP32-S3's
 * USB-OTG port: a vendor interface with four bulk endpoints for WebUSB and the
 * netplay bridge, and CDC-ACM for WebSerial, behind GB-Link's VID/PID and
 * WebUSB / MS OS 2.0 descriptors (src/gblink_usbdesc.c).
 *
 * TinyUSB has a vendor class, but it pairs one IN with one OUT per interface;
 * GB-Link puts two pairs on interface 0. So interface 0 gets a small class
 * driver of its own (usbd_app_driver_get_cb), and the CDC interfaces use
 * esp_tinyusb's ACM as they would anywhere.
 *
 * Mutually exclusive with the USB drive (usb_msc_esp32s3.c): both own the one
 * OTG controller, and the ESP32-S3 has four IN endpoints besides EP0, which
 * GB-Link's interface plus CDC use up exactly. gbagate picks one at boot from
 * the "usb" setting; see pio/src/usb_personality.c.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "device/usbd_pvt.h"

#include "gblink_port.h"
#include "gblink_usbdesc.h"

static const char *TAG = "usb_gblink";

/* Defined in port/gblink/gblink_glue.cpp: persisted 0x44 setting. */
extern bool gblink_landing_page_enabled(void);

/* ---- descriptors ----------------------------------------------------------- */

static uint8_t s_dev[GBLINK_DESC_DEVICE_LEN];
static uint8_t s_cfg[GBLINK_DESC_CONFIG_LEN];
static uint8_t s_bos[GBLINK_DESC_BOS_LEN];
static char    s_serial[13];

static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 }, /* 0: English (US) */
    "gbagate",                    /* 1: manufacturer */
    "GBLink USB",                 /* 2: product -- what GB-Link reports */
    s_serial,                     /* 3: serial, from the MAC */
};

uint8_t const *tud_descriptor_bos_cb(void) { return s_bos; }

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        req->bmRequestType_bit.direction != TUSB_DIR_IN) {
        return false;
    }
    const uint8_t *data;
    int n = gblink_desc_vendor_request(req->bRequest, req->wValue, req->wIndex, &data);
    if (n < 0) return false;
    return tud_control_xfer(rhport, req, (void *)data, (uint16_t)n);
}

/* ---- interface 0 class driver ------------------------------------------------- */

typedef struct {
    uint8_t           addr;
    SemaphoreHandle_t done;     /* IN: previous transfer collected */
    StaticSemaphore_t done_buf;
    uint8_t           buf[GBLINK_EP_SIZE];
} ep_t;

static ep_t s_status_in = { .addr = GBLINK_EP_STATUS_IN };
static ep_t s_data_in   = { .addr = GBLINK_EP_DATA_IN };
static ep_t s_cmd_out   = { .addr = GBLINK_EP_CMD_OUT };
static ep_t s_data_out  = { .addr = GBLINK_EP_DATA_OUT };
static volatile bool s_configured;
static uint8_t s_rhport;

static void ep_sems_init(void) {
    s_status_in.done = xSemaphoreCreateBinaryStatic(&s_status_in.done_buf);
    s_data_in.done   = xSemaphoreCreateBinaryStatic(&s_data_in.done_buf);
    xSemaphoreGive(s_status_in.done);
    xSemaphoreGive(s_data_in.done);
}

static void arm_out(ep_t *ep) {
    if (usbd_edpt_claim(s_rhport, ep->addr)) {
        usbd_edpt_xfer(s_rhport, ep->addr, ep->buf, GBLINK_EP_SIZE, false);
    }
}

static void gbl_init(void) {}
static bool gbl_deinit(void) { return true; }

static void gbl_reset(uint8_t rhport) {
    (void)rhport;
    if (s_configured) {
        s_configured = false;
        gblink_usb_on_configured(false);
    }
    /* Anything waiting on an IN that will never complete must not wait out
     * its timeout on every send while unplugged. */
    xSemaphoreGive(s_status_in.done);
    xSemaphoreGive(s_data_in.done);
}

static uint16_t gbl_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
        itf->bInterfaceNumber != GBLINK_IF_VENDOR) {
        return 0;
    }
    uint16_t len = sizeof(tusb_desc_interface_t);
    uint8_t const *p = tu_desc_next(itf);
    for (int i = 0; i < itf->bNumEndpoints; i++) {
        while (len < max_len && tu_desc_type(p) != TUSB_DESC_ENDPOINT) {
            len += tu_desc_len(p);
            p = tu_desc_next(p);
        }
        if (len >= max_len) return 0;
        if (!usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p)) return 0;
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    s_rhport = rhport;
    arm_out(&s_cmd_out);
    arm_out(&s_data_out);
    s_configured = true;
    gblink_usb_on_configured(true);
    return len;
}

/* WebUSB GET_URL with an interface recipient (the launcher's probe for older
 * TinyUSB firmware) arrives here instead of tud_vendor_control_xfer_cb. */
static bool gbl_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    return tud_vendor_control_xfer_cb(rhport, stage, req);
}

static bool gbl_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n) {
    (void)rhport;
    if (ep_addr == GBLINK_EP_CMD_OUT || ep_addr == GBLINK_EP_DATA_OUT) {
        ep_t *ep = ep_addr == GBLINK_EP_CMD_OUT ? &s_cmd_out : &s_data_out;
        if (result == XFER_RESULT_SUCCESS && n > 0) {
            gblink_usb_on_receive(ep_addr, ep->buf, n);
        }
        arm_out(ep);
        return true;
    }
    if (ep_addr == GBLINK_EP_STATUS_IN) { xSemaphoreGive(s_status_in.done); return true; }
    if (ep_addr == GBLINK_EP_DATA_IN)   { xSemaphoreGive(s_data_in.done);   return true; }
    return false;
}

static const usbd_class_driver_t s_driver = {
    .name            = "GBLINK",
    .init            = gbl_init,
    .deinit          = gbl_deinit,
    .reset           = gbl_reset,
    .open            = gbl_open,
    .control_xfer_cb = gbl_control,
    .xfer_cb         = gbl_xfer,
    .sof             = NULL,
};

static bool s_installed;

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count) {
    *count = s_installed ? 1 : 0;
    return s_installed ? &s_driver : NULL;
}

bool gblink_usb_write(uint8_t ep_in, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!s_configured || len > GBLINK_EP_SIZE) return false;
    ep_t *ep = ep_in == GBLINK_EP_STATUS_IN ? &s_status_in
             : ep_in == GBLINK_EP_DATA_IN   ? &s_data_in : NULL;
    if (!ep) return false;
    /* As upstream: wait for the previous transfer, but not forever. */
    xSemaphoreTake(ep->done, pdMS_TO_TICKS(timeout_ms));
    if (!s_configured) return false;
    memcpy(ep->buf, data, len);
    if (!usbd_edpt_claim(s_rhport, ep->addr)) {
        xSemaphoreGive(ep->done);
        return false;
    }
    if (!usbd_edpt_xfer(s_rhport, ep->addr, ep->buf, (uint16_t)len, false)) {
        usbd_edpt_release(s_rhport, ep->addr);
        xSemaphoreGive(ep->done);
        return false;
    }
    return true;
}

/* ---- CDC --------------------------------------------------------------------------- */

static void cdc_rx(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t n = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof buf, &n) == ESP_OK && n > 0) {
        gblink_cdc_on_receive(buf, n);
    }
}

bool gblink_cdc_write(const uint8_t *data, size_t len) {
    if (!tud_cdc_n_connected(0)) return false;   /* port closed: drop the frame */
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len) != len) return false;
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return true;
}

/* ---- bring-up ------------------------------------------------------------------------ */

esp_err_t usb_gblink_start(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    gblink_desc_device(s_dev);
    gblink_desc_config(s_cfg);
    gblink_desc_bos(s_bos, gblink_landing_page_enabled());
    ep_sems_init();
    s_installed = true;

    const tinyusb_config_t cfg = {
        .device_descriptor = (const tusb_desc_device_t *)s_dev,
        .string_descriptor = s_strings,
        .string_descriptor_count = sizeof s_strings / sizeof s_strings[0],
        .external_phy = false,
        .configuration_descriptor = s_cfg,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "tinyusb install");

    const tinyusb_config_cdcacm_t acm = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx,
    };
    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&acm), TAG, "cdc init");
    ESP_LOGI(TAG, "GB-Link USB: %04X:%04X, serial %s", GBLINK_USB_VID, GBLINK_USB_PID, s_serial);
    return ESP_OK;
}
