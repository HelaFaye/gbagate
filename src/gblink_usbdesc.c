/*
 * gblink_usbdesc.c -- see gblink_usbdesc.h.
 */
#include "gblink_usbdesc.h"

#include <string.h>

typedef struct {
    uint8_t *p;
    size_t   n;
} wr_t;

static void b(wr_t *w, uint8_t v)   { w->p[w->n++] = v; }
static void h(wr_t *w, uint16_t v)  { b(w, (uint8_t)v); b(w, (uint8_t)(v >> 8)); }
static void l(wr_t *w, uint32_t v)  { h(w, (uint16_t)v); h(w, (uint16_t)(v >> 16)); }
static void m(wr_t *w, const void *src, size_t len) { memcpy(w->p + w->n, src, len); w->n += len; }

static void ep(wr_t *w, uint8_t addr, uint8_t attr, uint16_t size, uint8_t interval) {
    b(w, 7); b(w, 0x05); b(w, addr); b(w, attr); h(w, size); b(w, interval);
}

void gblink_desc_device(uint8_t out[GBLINK_DESC_DEVICE_LEN]) {
    wr_t w = { out, 0 };
    b(&w, 18); b(&w, 0x01);
    h(&w, 0x0210);                   /* bcdUSB: 2.1, for the BOS */
    b(&w, 0xEF); b(&w, 0x02); b(&w, 0x01); /* Misc / IAD: composite with IAD */
    b(&w, 64);                       /* bMaxPacketSize0 */
    h(&w, GBLINK_USB_VID);
    h(&w, GBLINK_USB_PID);
    h(&w, GBLINK_USB_BCD_DEVICE);
    b(&w, GBLINK_STR_MANUFACTURER);
    b(&w, GBLINK_STR_PRODUCT);
    b(&w, GBLINK_STR_SERIAL);
    b(&w, 1);                        /* bNumConfigurations */
}

void gblink_desc_config(uint8_t out[GBLINK_DESC_CONFIG_LEN]) {
    wr_t w = { out, 0 };
    /* configuration */
    b(&w, 9); b(&w, 0x02); h(&w, GBLINK_DESC_CONFIG_LEN);
    b(&w, 3);                        /* bNumInterfaces */
    b(&w, 1);                        /* bConfigurationValue */
    b(&w, 0);                        /* iConfiguration */
    b(&w, 0x80);                     /* bus powered */
    b(&w, 50);                       /* 100 mA, Zephyr's default as upstream */

    /* interface 0: vendor, four bulk endpoints, in upstream's order */
    b(&w, 9); b(&w, 0x04); b(&w, GBLINK_IF_VENDOR); b(&w, 0); b(&w, 4);
    b(&w, 0xFF); b(&w, 0x00); b(&w, 0x00); b(&w, 0);
    ep(&w, 0x81, 0x02, 64, 0);
    ep(&w, 0x01, 0x02, 64, 0);
    ep(&w, 0x82, 0x02, 64, 0);
    ep(&w, 0x02, 0x02, 64, 0);

    /* IAD for the CDC function */
    b(&w, 8); b(&w, 0x0B); b(&w, GBLINK_IF_CDC_COMM); b(&w, 2);
    b(&w, 0x02); b(&w, 0x02); b(&w, 0x00); b(&w, 0);

    /* interface 1: CDC communication, ACM */
    b(&w, 9); b(&w, 0x04); b(&w, GBLINK_IF_CDC_COMM); b(&w, 0); b(&w, 1);
    b(&w, 0x02); b(&w, 0x02); b(&w, 0x00); b(&w, 0);
    b(&w, 5); b(&w, 0x24); b(&w, 0x00); h(&w, 0x0120);            /* header, CDC 1.2 */
    b(&w, 5); b(&w, 0x24); b(&w, 0x01); b(&w, 0x00); b(&w, GBLINK_IF_CDC_DATA); /* call mgmt */
    b(&w, 4); b(&w, 0x24); b(&w, 0x02); b(&w, 0x02);               /* ACM: line coding */
    b(&w, 5); b(&w, 0x24); b(&w, 0x06); b(&w, GBLINK_IF_CDC_COMM); b(&w, GBLINK_IF_CDC_DATA);
    ep(&w, GBLINK_EP_CDC_NOTIF, 0x03, 16, 10);

    /* interface 2: CDC data */
    b(&w, 9); b(&w, 0x04); b(&w, GBLINK_IF_CDC_DATA); b(&w, 0); b(&w, 2);
    b(&w, 0x0A); b(&w, 0x00); b(&w, 0x00); b(&w, 0);
    ep(&w, GBLINK_EP_CDC_OUT, 0x02, 64, 0);
    ep(&w, GBLINK_EP_CDC_IN, 0x02, 64, 0);
}

static const uint8_t UUID_WEBUSB[16] = {
    0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
    0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65 };
static const uint8_t UUID_MSOS2[16] = {
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F };

void gblink_desc_bos(uint8_t out[GBLINK_DESC_BOS_LEN], bool landing) {
    wr_t w = { out, 0 };
    b(&w, 5); b(&w, 0x0F); h(&w, GBLINK_DESC_BOS_LEN); b(&w, 3);

    /* WebUSB platform capability */
    b(&w, 24); b(&w, 0x10); b(&w, 0x05); b(&w, 0);
    m(&w, UUID_WEBUSB, 16);
    h(&w, 0x0100);
    b(&w, GBLINK_VENDOR_CODE_WEBUSB);
    b(&w, landing ? 1 : 0);          /* iLandingPage */

    /* Microsoft OS 2.0 platform capability */
    b(&w, 28); b(&w, 0x10); b(&w, 0x05); b(&w, 0);
    m(&w, UUID_MSOS2, 16);
    l(&w, 0x06030000);               /* Windows 8.1 */
    h(&w, GBLINK_DESC_MSOS2_LEN);
    b(&w, GBLINK_VENDOR_CODE_MSOS2);
    b(&w, 0);                        /* bAltEnumCode */

    /* USB 2.0 extension: no LPM, as upstream says, because neither stack has it */
    b(&w, 7); b(&w, 0x10); b(&w, 0x02); l(&w, 0);
}

/* "{FA611CC3-7057-42EE-9D82-4919639562B3}" -- upstream's random GUID. Kept,
 * so a Windows host that bound WinUSB to a GB-Link binds this the same way. */
static const char GUID[] = "{FA611CC3-7057-42EE-9D82-4919639562B3}";
static const char PROP[] = "DeviceInterfaceGUIDs";

static void utf16(wr_t *w, const char *s, size_t chars_incl_nul) {
    for (size_t i = 0; i < chars_incl_nul; i++) h(w, (uint8_t)s[i]);
}

void gblink_desc_msos2(uint8_t out[GBLINK_DESC_MSOS2_LEN]) {
    wr_t w = { out, 0 };
    /* set header */
    h(&w, 10); h(&w, 0x0000); l(&w, 0x06030000); h(&w, GBLINK_DESC_MSOS2_LEN);
    /* function subset: interface 0 only */
    h(&w, 8); h(&w, 0x0002); b(&w, GBLINK_IF_VENDOR); b(&w, 0); h(&w, 8 + 20 + 132);
    /* compatible ID */
    h(&w, 20); h(&w, 0x0003);
    m(&w, "WINUSB\0\0", 8);
    m(&w, "\0\0\0\0\0\0\0\0", 8);
    /* registry property: DeviceInterfaceGUIDs, REG_MULTI_SZ */
    h(&w, 132); h(&w, 0x0004); h(&w, 7);
    h(&w, 42); utf16(&w, PROP, sizeof PROP);           /* 21 chars incl. NUL */
    h(&w, 80); utf16(&w, GUID, sizeof GUID); h(&w, 0); /* 39 chars + extra NUL */
}

void gblink_desc_url(uint8_t out[GBLINK_DESC_URL_LEN]) {
    static const char host[] = "launcher.gblink.io";
    out[0] = GBLINK_DESC_URL_LEN;
    out[1] = 0x03;                   /* WEBUSB_URL */
    out[2] = 0x01;                   /* https:// -- Chrome shows only https */
    memcpy(&out[3], host, sizeof host - 1);
}

/* Upstream's legacy GET_ALLOWED_ORIGINS answer, verbatim. */
static const uint8_t ALLOWED_ORIGINS[13] = {
    0x05, 0x00, 0x0D, 0x00, 0x01,
    0x04, 0x01, 0x01, 0x01,
    0x04, 0x02, 0x02, 0x01 };

static const uint8_t MSOS1_COMPAT[40] = {
    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

const uint8_t gblink_desc_msos1_string[18] = {
    18, 0x03, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    GBLINK_VENDOR_CODE_MSOS1, 0 };

int gblink_desc_vendor_request(uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                               const uint8_t **data) {
    static uint8_t url[GBLINK_DESC_URL_LEN];
    static uint8_t msos2[GBLINK_DESC_MSOS2_LEN];

    if (bRequest == GBLINK_VENDOR_CODE_WEBUSB && wIndex == 0x0001) {
        *data = ALLOWED_ORIGINS;
        return (int)sizeof ALLOWED_ORIGINS;
    }
    if (bRequest == GBLINK_VENDOR_CODE_WEBUSB && wIndex == 0x0002) {
        if ((wValue & 0xFF) != 1) return -1; /* only URL index 1 exists */
        gblink_desc_url(url);
        *data = url;
        return (int)sizeof url;
    }
    if (bRequest == GBLINK_VENDOR_CODE_MSOS2 && wIndex == 0x0007) {
        gblink_desc_msos2(msos2);
        *data = msos2;
        return (int)sizeof msos2;
    }
    if (bRequest == GBLINK_VENDOR_CODE_MSOS1 && wIndex == 0x0004) {
        *data = MSOS1_COMPAT;
        return (int)sizeof MSOS1_COMPAT;
    }
    return -1;
}
