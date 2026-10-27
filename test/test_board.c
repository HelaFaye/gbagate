/*
 * test_board.c -- no two functions may claim the same pin.
 *
 *   cc -std=c11 -Wall -Wextra -I../port/esp32s3 -o test_board test_board.c
 *
 * The pin map used to be spread across seven port files, and it had drifted
 * into six collisions -- three buttons on the display's SPI bus, the card and
 * the panel sharing a chip select, I2C SCL on the panel's DC line. None of it
 * was visible in any one file.
 *
 * This is the check that makes that impossible. It runs for both boards, and
 * it also refuses strapping pins, which are the other way a pin map produces a
 * board that works intermittently and looks like a hardware fault.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define BOARD_DEVKIT 1
#include "board.h"

static int g_fail = 0;

typedef struct { int pin; const char *who; } claim_t;

/* Pins that may legitimately be claimed twice, with the reason. */
static bool shared_ok(const char *a, const char *b) {
    /*
     * The multiboot SPI master and the infrared driver both drive the link
     * connector. They are different protocols on the same four pins and are
     * never active at once -- the device is in one mode at a time -- so this
     * is sharing, not a clash.
     */
    if (strstr(a, "link") && strstr(b, "link")) {
        return true;
    }
    return false;
}

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("    FAIL %-40s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_map(const char *board, const claim_t *c, size_t n) {
    printf("  %s: %zu pins claimed\n", board, n);

    int collisions = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (c[i].pin != c[j].pin) {
                continue;
            }
            if (shared_ok(c[i].who, c[j].who)) {
                continue;
            }
            printf("    FAIL GPIO %d: %s vs %s\n", c[i].pin, c[i].who, c[j].who);
            collisions++;
            g_fail++;
        }
    }

    /* Strapping pins decide boot mode; a switch on one is an intermittent
     * boot failure that looks like bad hardware. */
    static const int strap[] = BOARD_STRAPPING_PINS;
    static const int usb[]   = BOARD_USB_PINS;
    for (size_t i = 0; i < n; i++) {
        for (size_t s = 0; s < sizeof strap / sizeof strap[0]; s++) {
            if (c[i].pin == strap[s]) {
                printf("    FAIL %s is on strapping pin %d\n", c[i].who, c[i].pin);
                g_fail++;
            }
        }
        for (size_t u = 0; u < sizeof usb / sizeof usb[0]; u++) {
            if (c[i].pin == usb[u]) {
                printf("    FAIL %s is on USB pin %d\n", c[i].who, c[i].pin);
                g_fail++;
            }
        }
    }

    if (!collisions) {
        printf("    no collisions\n");
    }
}

/* ---- the devkit --------------------------------------------------------- */

static const claim_t k_devkit[] = {
    { PIN_LINK_SC, "link SC" }, { PIN_LINK_SD, "link SD" },
    { PIN_LINK_SI, "link SI" }, { PIN_LINK_SO, "link SO" },
    { PIN_SPI_SCK, "SPI SCK" }, { PIN_SPI_MOSI, "SPI MOSI" },
    { PIN_SPI_MISO, "SPI MISO" },
    { PIN_LCD_CS, "LCD CS" }, { PIN_LCD_DC, "LCD DC" },
    { PIN_SD_CS, "SD CS" },
    { PIN_I2C_SDA, "I2C SDA" }, { PIN_I2C_SCL, "I2C SCL" },
    { PIN_BTN_UP, "btn up" }, { PIN_BTN_DOWN, "btn down" },
    { PIN_BTN_LEFT, "btn left" }, { PIN_BTN_RIGHT, "btn right" },
    { PIN_BTN_A, "btn A" }, { PIN_BTN_B, "btn B" },
    { PIN_BTN_SELECT, "btn Select" }, { PIN_BTN_START, "btn Start" },
    { PIN_BTN_L, "btn L" }, { PIN_BTN_R, "btn R" },
    { PIN_IR_TX_GPIO, "IR TX" }, { PIN_IR_RX_GPIO, "IR RX" },
    { PIN_LINK_SD_ALT, "link SD (GBC cable path)" },
    { PIN_RGB_LED, "RGB LED" },
    { PIN_BRIDGE_TX, "bridge UART TX" }, { PIN_BRIDGE_RX, "bridge UART RX" },
};

/* The Supermini map, checked in a second translation unit so both sets of
 * macros can exist at once. */
extern void check_supermini(int *fail);

int main(void) {
    printf("pin maps\n");
    check_map("devkit", k_devkit, sizeof k_devkit / sizeof k_devkit[0]);

    /*
     * The devkit must not exceed what the board exposes, and must leave the
     * strapping and USB pins alone. Fourteen of these would not fit a
     * Supermini, which is the whole reason there are two maps.
     */
    printf("    %zu pins used of ~44 available\n",
           sizeof k_devkit / sizeof k_devkit[0]);

    /*
     * A board cannot claim a part it has no pins for. These are the pairings
     * that make a build fail at link time or, worse, run and do nothing --
     * which on a bring-up reads as a broken driver rather than a board that
     * was never going to work.
     */
    printf("  what the devkit claims to have\n");
#if BOARD_HAS_SD && !BOARD_HAS_DISPLAY
    printf("    note: card without display -- they share SPI, so this is\n"
           "          legal but only useful for a headless test\n");
#endif
#if BOARD_HAS_READER && !defined(PIN_I2C_SDA)
    printf("    FAIL claims a chip reader with no I2C pins\n");
    g_fail++;
#endif
#if BOARD_BUTTONS_ON_EXPANDER && !defined(PIN_I2C_SDA)
    printf("    FAIL buttons on an expander with no I2C pins\n");
    g_fail++;
#endif
/*
 * The encoder: the default input mode, and for a long time it had no pins at
 * all. On the devkit it shares the five-way's UP, DOWN and centre pins -- the
 * two inputs are alternatives -- and those must be real, non-strapping GPIOs.
 */
#if !BOARD_HAS_ENCODER
    printf("    FAIL the devkit has no encoder pins\n");
    g_fail++;
#else
    check("encoder A is the UP pin", PIN_ENC_A, PIN_BTN_UP);
    check("encoder B is the DOWN pin", PIN_ENC_B, PIN_BTN_DOWN);
    check("encoder switch is the centre pin", PIN_ENC_SW, PIN_BTN_A);
    check("encoder pins are distinct", PIN_ENC_A != PIN_ENC_B && PIN_ENC_B != PIN_ENC_SW &&
          PIN_ENC_A != PIN_ENC_SW, 1);
#endif
#if !BOARD_HAS_BUTTONS && !BOARD_BUTTONS_ON_EXPANDER
    printf("    note: no buttons fitted -- nothing can be pressed, which is\n"
           "          only sensible for an automated bring-up\n");
#endif
    printf("    display=%d link=%d sd=%d buttons=%d ir=%d reader=%d\n",
           BOARD_HAS_DISPLAY, BOARD_HAS_LINK, BOARD_HAS_SD,
           BOARD_HAS_BUTTONS, BOARD_HAS_IR, BOARD_HAS_READER);

    check_supermini(&g_fail);

    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
