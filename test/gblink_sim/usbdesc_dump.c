/*
 * usbdesc_dump.c -- print the GB-Link USB descriptors the firmware serves, as
 * JSON, so host-side compatibility tests can build a WebUSB device out of the
 * real bytes rather than a hand-written copy of them.
 */
#include <stdio.h>

#include "gblink_usbdesc.h"

static void hex(const char *name, const uint8_t *p, size_t n, int last) {
    printf("  \"%s\": \"", name);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\"%s\n", last ? "" : ",");
}

int main(void) {
    uint8_t dev[GBLINK_DESC_DEVICE_LEN], cfg[GBLINK_DESC_CONFIG_LEN];
    uint8_t bos[GBLINK_DESC_BOS_LEN], url[GBLINK_DESC_URL_LEN];
    gblink_desc_device(dev);
    gblink_desc_config(cfg);
    gblink_desc_bos(bos, true);
    gblink_desc_url(url);
    printf("{\n");
    hex("device", dev, sizeof dev, 0);
    hex("config", cfg, sizeof cfg, 0);
    hex("bos", bos, sizeof bos, 0);
    hex("url", url, sizeof url, 1);
    printf("}\n");
    return 0;
}
