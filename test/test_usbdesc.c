/*
 * test_usbdesc.c -- the GB-Link USB identity, byte by byte.
 *
 * Walks every descriptor the way a host parser does (lengths chain, totals
 * agree) and checks each field a GB-Link host keys on. The MS OS 2.0 set is
 * also checked against its 170-byte size in upstream's packed struct, because
 * a wrong wTotalLength makes Windows ignore the whole set silently.
 */
#include <stdio.h>
#include <string.h>

#include "gblink_usbdesc.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-46s got %ld (0x%lx), want %ld (0x%lx)\n", what, got, got, want, want);
        g_fail++;
    }
}

static unsigned u16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long u32(const uint8_t *p) { return u16(p) | ((unsigned long)u16(p + 2) << 16); }

static void test_device(void) {
    printf("device\n");
    uint8_t d[GBLINK_DESC_DEVICE_LEN];
    gblink_desc_device(d);
    check("bLength", d[0], 18);
    check("bcdUSB 2.1 (WebUSB needs >= 2.10)", u16(d + 2), 0x0210);
    check("composite with IAD", d[4] == 0xEF && d[5] == 0x02 && d[6] == 0x01, 1);
    check("idVendor (netplay-bridge GBLINK_VID)", u16(d + 8), 0x2FE3);
    check("idProduct (netplay-bridge GBLINK_PID)", u16(d + 10), 0x000A);
    check("bcdDevice = BCD 2.2.3 (launcher reads it)", u16(d + 12), 0x0223);
    check("one configuration", d[17], 1);
}

static void test_config(void) {
    printf("configuration\n");
    uint8_t c[GBLINK_DESC_CONFIG_LEN];
    gblink_desc_config(c);
    check("wTotalLength", u16(c + 2), GBLINK_DESC_CONFIG_LEN);
    check("three interfaces", c[4], 3);

    size_t off = 0;
    int ifaces = 0, eps = 0, cur_if = -1, iads = 0;
    unsigned ep_addr[16];
    int ep_if[16];
    while (off < GBLINK_DESC_CONFIG_LEN) {
        uint8_t len = c[off], type = c[off + 1];
        if (len == 0) { check("zero-length descriptor", 1, 0); return; }
        if (type == 0x04) { ifaces++; cur_if = c[off + 2]; }
        if (type == 0x0B) iads++;
        if (type == 0x05 && eps < 16) { ep_addr[eps] = c[off + 2]; ep_if[eps] = cur_if; eps++; }
        if (type == 0x04 && c[off + 2] == 0) {
            check("interface 0 is vendor class", c[off + 5], 0xFF);
            check("interface 0 has 4 endpoints", c[off + 4], 4);
        }
        off += len;
    }
    check("descriptors chain to wTotalLength", (long)off, GBLINK_DESC_CONFIG_LEN);
    check("interface descriptors", ifaces, 3);
    check("one IAD", iads, 1);
    check("seven endpoints", eps, 7);
    /* adapter.ts: EP_CMD_OUT 0x01, EP_STATUS_IN 0x81, EP_DATA_OUT 0x02, EP_DATA_IN 0x82 */
    unsigned want[4] = { 0x81, 0x01, 0x82, 0x02 };
    for (int i = 0; i < 4; i++) {
        check("vendor endpoint address", ep_addr[i], want[i]);
        check("vendor endpoint on interface 0", ep_if[i], 0);
    }
    /* ESP32-S3's USB-OTG has four IN endpoints besides EP0: this uses all four. */
    int in = 0;
    for (int i = 0; i < eps; i++) in += (ep_addr[i] & 0x80) != 0;
    check("IN endpoints fit the ESP32-S3 (4 + EP0)", in, 4);
    /* launcher.js sorts endpoints by number and takes the first OUT as cmd,
     * the last OUT/IN as data, within the vendor interface only. */
    check("launcher: lowest vendor OUT is 1", ep_addr[1], 0x01);
    check("launcher: highest vendor IN is 2", ep_addr[2] & 0x0F, 2);
}

static void test_bos(void) {
    printf("BOS\n");
    uint8_t d[GBLINK_DESC_BOS_LEN];
    gblink_desc_bos(d, true);
    check("wTotalLength", u16(d + 2), GBLINK_DESC_BOS_LEN);
    check("three capabilities", d[4], 3);
    size_t off = 5;
    int caps = 0;
    while (off < GBLINK_DESC_BOS_LEN) { off += d[off]; caps++; }
    check("capabilities chain", (long)off, GBLINK_DESC_BOS_LEN);
    check("capability count", caps, 3);
    check("WebUSB vendor code", d[5 + 22], 0x01);
    check("WebUSB landing page index", d[5 + 23], 1);
    check("MS OS 2.0 set length", u16(d + 29 + 24), GBLINK_DESC_MSOS2_LEN);
    check("MS OS 2.0 vendor code", d[29 + 26], 0x02);
    gblink_desc_bos(d, false);
    check("landing page off (0x44 0)", d[5 + 23], 0);
}

static void test_msos2(void) {
    printf("MS OS 2.0\n");
    uint8_t d[GBLINK_DESC_MSOS2_LEN];
    gblink_desc_msos2(d);
    check("wTotalLength", u16(d + 8), 170);
    check("Windows 8.1", (long)u32(d + 4), 0x06030000L);
    check("function subset on interface 0", d[10 + 4], 0);
    check("subset length", u16(d + 10 + 6), 160);
    check("WINUSB", memcmp(d + 18 + 4, "WINUSB\0\0", 8), 0);
    const uint8_t *p = d + 38;
    check("property length", u16(p), 132);
    check("REG_MULTI_SZ", u16(p + 4), 7);
    check("name length", u16(p + 6), 42);
    check("name starts D", p[8], 'D');
    check("data length", u16(p + 50), 80);
    check("GUID opens with {", p[52], '{');
    check("GUID F of FA611CC3", p[54], 'F');
    check("double NUL terminator", u16(p + 52 + 76) == 0 && u16(p + 52 + 78) == 0, 1);
}

static void test_vendor_requests(void) {
    printf("vendor requests\n");
    const uint8_t *data;
    int n = gblink_desc_vendor_request(0x01, 0x0001, 0x0002, &data);
    check("GET_URL length", n, 21);
    check("URL descriptor type", data[1], 0x03);
    check("https scheme", data[2], 0x01);
    check("launcher.gblink.io", memcmp(data + 3, "launcher.gblink.io", 18), 0);
    check("GET_URL index 2 stalls", gblink_desc_vendor_request(0x01, 0x0002, 0x0002, &data), -1);
    check("allowed origins", gblink_desc_vendor_request(0x01, 0, 0x0001, &data), 13);
    check("MS OS 2.0 set", gblink_desc_vendor_request(0x02, 0, 0x0007, &data), 170);
    check("MS OS 1.0 compat ID", gblink_desc_vendor_request(0x03, 0, 0x0004, &data), 40);
    check("unknown request stalls", gblink_desc_vendor_request(0x09, 0, 0, &data), -1);
    check("MSFT100 vendor code", gblink_desc_msos1_string[16], 0x03);
}

int main(void) {
    test_device();
    test_config();
    test_bos();
    test_msos2();
    test_vendor_requests();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
