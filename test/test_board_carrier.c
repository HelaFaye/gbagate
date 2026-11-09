/*
 * The DIY carrier boards' pin maps (doc/CARRIER.md), each in its own
 * translation unit as the Supermini's is, so every map's macros can be
 * checked in one run. CARRIER_BOARD is BOARD_CARRIER_TOUCH or _KNOB.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"

#if defined(BOARD_CARRIER_TOUCH)
#define CHECK_FN check_carrier_touch
#else
#define CHECK_FN check_carrier_knob
#endif

void CHECK_FN(int *fail);

void CHECK_FN(int *fail) {
    struct { int pin; const char *who; } c[] = {
        { PIN_LINK_SC, "link SC" }, { PIN_LINK_SD, "link SD" },
        { PIN_LINK_SI, "link SI (port 1)" }, { PIN_LINK_SO, "link SO" },
        { PIN_LINK_SI2, "link SI (port 2)" }, { PIN_LINK_SI3, "link SI (port 3)" },
        { PIN_SPI_SCK, "SPI SCK" }, { PIN_SPI_MOSI, "SPI MOSI" },
        { PIN_SPI_MISO, "SPI MISO" },
        { PIN_LCD_CS, "LCD CS" }, { PIN_LCD_DC, "LCD DC" },
        { PIN_SD_CS, "SD CS" },
        { PIN_I2C_SDA, "I2C SDA" }, { PIN_I2C_SCL, "I2C SCL" },
#if defined(BOARD_CARRIER_TOUCH)
        { PIN_TOUCH_CS, "touch CS" },
#else
        { PIN_EXP_INT, "expander INT" },
#endif
        { PIN_RGB_LED, "RGB LED" },
    };
    const size_t n = sizeof c / sizeof c[0];
    printf("  %s: %zu pins claimed\n", BOARD_NAME, n);

    int bad = 0;
    static const int strap[] = BOARD_STRAPPING_PINS;
    static const int usb[] = BOARD_USB_PINS;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (c[i].pin == c[j].pin) {
                printf("    FAIL GPIO %d: %s vs %s\n", c[i].pin, c[i].who, c[j].who);
                bad++;
            }
        }
        /* The header's pins, and the LED the board itself wires to 48. */
        const bool header = (c[i].pin >= 1 && c[i].pin <= 13) || c[i].pin == 43 || c[i].pin == 44;
        const bool led = c[i].pin == 48 && strstr(c[i].who, "LED");
        if (!header && !led) {
            printf("    FAIL %s on GPIO %d, which a Supermini's header does not have\n", c[i].who, c[i].pin);
            bad++;
        }
        for (size_t s = 0; s < sizeof strap / sizeof strap[0]; s++) {
            if (c[i].pin == strap[s] && !(c[i].pin == BOARD_STRAP_OK_CS && strstr(c[i].who, "CS"))) {
                printf("    FAIL %s is on strapping pin %d\n", c[i].who, c[i].pin);
                bad++;
            }
        }
        for (size_t u = 0; u < sizeof usb / sizeof usb[0]; u++) {
            if (c[i].pin == usb[u]) {
                printf("    FAIL %s is on USB pin %d\n", c[i].who, c[i].pin);
                bad++;
            }
        }
        /* The ROM prints its banner on U0TXD (43) at every reset: only a
         * chip select, which nothing is clocked under then and nothing else
         * drives, may sit there. */
        if (c[i].pin == 43 && !strstr(c[i].who, "CS")) {
            printf("    FAIL %s on GPIO 43, which the ROM drives at reset\n", c[i].who);
            bad++;
        }
    }
    /* 44 must be an input: the ROM listens on it, never drives it. 43 an
     * input or a CS, never a link line. */
    if (PIN_LINK_SO == 43 || PIN_LINK_SC == 43 || PIN_LINK_SD == 43) {
        printf("    FAIL a driven link line on GPIO 43\n");
        bad++;
    }
    if (BOARD_LINK_PORTS != 3) { printf("    FAIL three link ports\n"); bad++; }
    if (!BOARD_HAS_READER) { printf("    FAIL the chip reader is on I2C here\n"); bad++; }
#if defined(BOARD_CARRIER_TOUCH)
    if (!BOARD_HAS_TOUCH || BOARD_HAS_ENCODER) { printf("    FAIL touch board: touch, no encoder\n"); bad++; }
#else
    if (BOARD_HAS_TOUCH || !BOARD_HAS_ENCODER || !BOARD_ENCODER_ON_EXPANDER) {
        printf("    FAIL knob board: the encoder, on the expander\n");
        bad++;
    }
    {
        const int e[] = { EXP_ENC_A, EXP_ENC_B, EXP_ENC_SW, EXP_BTN_A, EXP_BTN_B };
        for (size_t i = 0; i < 5; i++)
            for (size_t j = i + 1; j < 5; j++)
                if (e[i] == e[j]) { printf("    FAIL expander bit %d twice\n", e[i]); bad++; }
    }
#endif
    if (!bad) {
        printf("    no collisions, %zu of 15 header pins used\n", n - 1);
    }
    *fail += bad;
}
