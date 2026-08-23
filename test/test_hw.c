/*
 * test_hw.c -- probe and menu-gating tests.
 *
 *   cc -std=c11 -Wall -Wextra -o test_hw test_hw.c nrf24_probe.c hw_caps.c
 *
 * Simulates four buses: a genuine nRF24L01+, a clone that drops RF_DR_LOW, an
 * unpopulated footprint, and a bus stuck high. The last two are the cases that
 * matter -- a probe that says "present" on an empty board is worse than no
 * probe at all, because the menu entry appears and then the mode hangs.
 */

#include <stdio.h>
#include <string.h>

#include "hw_caps.h"
#include "nrf24_probe.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-40s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got ? got : "(null)", want) != 0) {
        printf("  FAIL %-40s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

/* ---- simulated radios ------------------------------------------------- */

typedef struct {
    uint8_t reg[32];
    uint8_t tx_addr[5];
    bool    honours_dr_low; /* false models a clone */
} fake_nrf_t;

static void fake_init(fake_nrf_t *f, bool plus) {
    memset(f, 0, sizeof *f);
    f->reg[NRF24_REG_CONFIG]   = 0x08;
    f->reg[NRF24_REG_SETUP_AW] = 0x03;
    f->reg[NRF24_REG_RF_CH]    = 0x02;
    f->reg[NRF24_REG_RF_SETUP] = plus ? 0x0E : 0x0F;
    f->reg[NRF24_REG_STATUS]   = 0x0E;
    memset(f->tx_addr, 0xE7, sizeof f->tx_addr);
    f->honours_dr_low = plus;
}

static void fake_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    fake_nrf_t *f = (fake_nrf_t *)ctx;
    uint8_t cmd = tx[0];
    memset(rx, 0, len);
    rx[0] = f->reg[NRF24_REG_STATUS]; /* STATUS always shifts out first */

    if (cmd == NRF24_CMD_NOP) {
        return;
    }

    uint8_t reg = cmd & NRF24_REG_MASK;
    bool write = (cmd & 0xE0) == NRF24_CMD_W_REGISTER;

    if (reg == NRF24_REG_TX_ADDR) {
        for (size_t i = 1; i < len && i <= 5; i++) {
            if (write) {
                f->tx_addr[i - 1] = tx[i];
            } else {
                rx[i] = f->tx_addr[i - 1];
            }
        }
        return;
    }

    if (write) {
        uint8_t v = tx[1];
        if (reg == NRF24_REG_RF_SETUP && !f->honours_dr_low) {
            v &= (uint8_t)~NRF24_RF_DR_LOW; /* clone silently drops it */
        }
        f->reg[reg] = v;
    } else if (len > 1) {
        rx[1] = f->reg[reg];
    }
}

/* Unpopulated footprint: MISO pulled low. */
static void empty_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx; (void)tx;
    memset(rx, 0x00, len);
}

/* Bus stuck high: no device, pull-up on MISO. */
static void stuck_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx; (void)tx;
    memset(rx, 0xFF, len);
}

/* Noise that happens to pass the cheap checks but cannot echo TX_ADDR. */
static void noisy_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    memset(rx, 0, len);
    rx[0] = 0x0E;
    if (len > 1) {
        rx[1] = (tx[0] & NRF24_REG_MASK) == NRF24_REG_SETUP_AW ? 0x03 : 0x5A;
    }
}

/* ---- probe tests ------------------------------------------------------ */

static void test_probe_genuine(void) {
    printf("probe: genuine nRF24L01+\n");
    fake_nrf_t f;
    fake_init(&f, true);
    nrf24_probe_t r;
    nrf24_probe(fake_xfer, &f, &r);

    check("present", r.present, 1);
    check("plus", r.plus, 1);
    check("no failure reason", r.reason == NULL, 1);
    check("TX_ADDR restored",
          memcmp(f.tx_addr, "\xE7\xE7\xE7\xE7\xE7", 5) == 0, 1);
    check("RF_SETUP restored", f.reg[NRF24_REG_RF_SETUP], 0x0E);
}

static void test_probe_clone(void) {
    printf("probe: clone that drops RF_DR_LOW\n");
    fake_nrf_t f;
    fake_init(&f, false);
    nrf24_probe_t r;
    nrf24_probe(fake_xfer, &f, &r);

    check("present", r.present, 1);
    check("not flagged as plus", r.plus, 0);
    check("TX_ADDR restored",
          memcmp(f.tx_addr, "\xE7\xE7\xE7\xE7\xE7", 5) == 0, 1);
}

static void test_probe_absent(void) {
    printf("probe: nothing fitted\n");
    nrf24_probe_t r;

    nrf24_probe(empty_xfer, NULL, &r);
    check("empty bus: absent", r.present, 0);
    check_str("empty bus: reason", r.reason, "bus floating (STATUS 0x00/0xFF)");

    nrf24_probe(stuck_xfer, NULL, &r);
    check("stuck-high bus: absent", r.present, 0);
    check_str("stuck bus: reason", r.reason, "bus floating (STATUS 0x00/0xFF)");

    nrf24_probe(noisy_xfer, NULL, &r);
    check("noise: absent", r.present, 0);
    check_str("noise: reason", r.reason, "TX_ADDR did not read back");

    nrf24_probe(NULL, NULL, &r);
    check("no transfer fn: absent", r.present, 0);
}

/* ---- menu gating ------------------------------------------------------ */

enum { MENU_CHIPS, MENU_FOLDER, MENU_LINK_USB, MENU_LINK_ESPNOW, MENU_RFU, MENU_READER };

static const menu_entry_t k_menu[] = {
    { MENU_CHIPS,       "Chip Library",   HW_CAP_DISPLAY, NULL },
    { MENU_FOLDER,      "Edit Folder",    HW_CAP_DISPLAY, NULL },
    { MENU_LINK_USB,    "Link: USB",      HW_CAP_DISPLAY | HW_CAP_USB,
      "plug in USB" },
    { MENU_LINK_ESPNOW, "Link: Wireless", HW_CAP_DISPLAY | HW_CAP_WIFI,
      "WiFi radio unavailable" },
    { MENU_RFU,         "Link: RFU",      HW_CAP_DISPLAY | HW_CAP_NRF24,
      "fit the nRF24 module" },
    { MENU_READER,      "Read Chip",      HW_CAP_DISPLAY | HW_CAP_CHIP_READER,
      "reader board not detected" },
};
#define MENU_N (sizeof k_menu / sizeof k_menu[0])

static void test_menu_gating(void) {
    printf("menu gating\n");

    const menu_entry_t *vis[MENU_N];

    /* Bare board: display and WiFi only. The RFU entry must not exist. */
    uint32_t caps = HW_CAP_DISPLAY | HW_CAP_WIFI | HW_CAP_BLE;
    size_t n = hw_menu_filter(k_menu, MENU_N, caps, vis, MENU_N);
    check("bare board entries", (long)n, 3);
    for (size_t i = 0; i < n; i++) {
        check("no RFU entry leaked", vis[i]->id != MENU_RFU, 1);
    }

    /* Radio fitted: the entry appears, in its declared position. USB is still
     * absent, so the RFU entry lands at index 3, not 4 -- gating shifts
     * positions, which is exactly why callers must use the filtered array
     * rather than indexing the master table. */
    caps |= HW_CAP_NRF24 | HW_CAP_NRF24_PLUS;
    n = hw_menu_filter(k_menu, MENU_N, caps, vis, MENU_N);
    check("with radio", (long)n, 4);
    check("RFU is last visible", vis[3]->id, MENU_RFU);
    check_str("RFU label", vis[3]->label, "Link: RFU");

    /* Everything present. */
    caps |= HW_CAP_USB | HW_CAP_CHIP_READER;
    n = hw_menu_filter(k_menu, MENU_N, caps, vis, MENU_N);
    check("full board", (long)n, (long)MENU_N);

    /* Order is preserved, not compacted arbitrarily. */
    check("first is chips", vis[0]->id, MENU_CHIPS);
    check("last is reader", vis[MENU_N - 1]->id, MENU_READER);
}

static void test_menu_diagnostics(void) {
    printf("diagnostics view\n");

    uint32_t caps = HW_CAP_DISPLAY | HW_CAP_WIFI;
    const menu_entry_t *hidden[MENU_N];
    size_t n = hw_menu_hidden(k_menu, MENU_N, caps, hidden, MENU_N);
    check("three hidden", (long)n, 3);

    uint32_t missing = hw_menu_missing(&k_menu[MENU_RFU], caps);
    check("RFU needs the radio", missing, HW_CAP_NRF24);

    char buf[64];
    hw_caps_describe(missing, buf, sizeof buf);
    check_str("described", buf, "nRF24 radio");

    hw_caps_describe(HW_CAP_NRF24 | HW_CAP_CHIP_READER, buf, sizeof buf);
    check_str("multi described", buf, "chip reader, nRF24 radio");

    /* Truncation must not overrun or emit a partial name. */
    char tiny[8];
    size_t used = hw_caps_describe(HW_CAP_NRF24 | HW_CAP_CHIP_READER, tiny, sizeof tiny);
    check("truncated safely", used < sizeof tiny, 1);
    check("still terminated", tiny[used] == '\0', 1);
}

static void trace(void) {
    printf("\ntrace: boot on a board with no radio fitted\n");

    nrf24_probe_t r;
    nrf24_probe(empty_xfer, NULL, &r);

    uint32_t caps = HW_CAP_DISPLAY | HW_CAP_WIFI | HW_CAP_BLE | HW_CAP_USB;
    if (r.present) {
        caps |= HW_CAP_NRF24;
        if (r.plus) {
            caps |= HW_CAP_NRF24_PLUS;
        }
    }
    printf("  nRF24 probe: %s (%s)\n",
           r.present ? "found" : "absent", r.reason ? r.reason : "ok");

    const menu_entry_t *vis[MENU_N];
    size_t n = hw_menu_filter(k_menu, MENU_N, caps, vis, MENU_N);
    printf("  menu:\n");
    for (size_t i = 0; i < n; i++) {
        printf("    %s\n", vis[i]->label);
    }

    const menu_entry_t *hid[MENU_N];
    size_t h = hw_menu_hidden(k_menu, MENU_N, caps, hid, MENU_N);
    printf("  hidden (diagnostics only):\n");
    for (size_t i = 0; i < h; i++) {
        char buf[64];
        hw_caps_describe(hw_menu_missing(hid[i], caps), buf, sizeof buf);
        printf("    %-16s needs %-16s (%s)\n",
               hid[i]->label, buf, hid[i]->absent_hint);
    }
}

int main(void) {
    test_probe_genuine();
    test_probe_clone();
    test_probe_absent();
    test_menu_gating();
    test_menu_diagnostics();
    trace();

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
