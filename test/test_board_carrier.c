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
        { PIN_LCD_RST, "LCD reset" }, { PIN_LCD_BL, "LCD backlight" },
        { PIN_SD_CS, "SD CS" },
        { PIN_I2C_SDA, "I2C SDA" }, { PIN_I2C_SCL, "I2C SCL" },
        { PIN_EXP_INT, "expander INT" },
        { PIN_BRIDGE_TX, "UART0 TX" }, { PIN_BRIDGE_RX, "UART0 RX" },
#if defined(BOARD_CARRIER_TOUCH)
        { PIN_TOUCH_CS, "touch CS" }, { PIN_TOUCH_IRQ, "touch IRQ" },
#else
        { PIN_ENC_A, "encoder A" }, { PIN_ENC_B, "encoder B" },
        { PIN_ENC_SW, "encoder push" },
        { PIN_BTN_A, "key A" }, { PIN_BTN_B, "key B" },
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
        /* The Supermini's pins: the edge rows (1-13, 43, 44), the inner
         * row (14-18, 21, 33-42, 45-48), and the LED it wires to 48. */
        const int p = c[i].pin;
        const bool outer = (p >= 1 && p <= 13) || p == 43 || p == 44;
        const bool inner = (p >= 14 && p <= 18) || p == 21 || (p >= 33 && p <= 42) || (p >= 45 && p <= 47);
        const bool led = p == 48 && strstr(c[i].who, "LED");
        if (!outer && !inner && !led) {
            printf("    FAIL %s on GPIO %d, which a Supermini does not bring out\n", c[i].who, p);
            bad++;
        }
        for (size_t s = 0; s < sizeof strap / sizeof strap[0]; s++) {
            if (c[i].pin == strap[s]) {
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
    }
    /* UART0 stays UART0: the ROM prints its banner on 43 at every reset,
     * and anything else there would see it. */
    if (PIN_BRIDGE_TX != 43 || PIN_BRIDGE_RX != 44) {
        printf("    FAIL 43 and 44 are UART0's\n");
        bad++;
    }
    if (BOARD_LINK_PORTS != 3) { printf("    FAIL three link ports\n"); bad++; }
    if (!BOARD_HAS_READER) { printf("    FAIL the chip reader is on I2C here\n"); bad++; }
#if defined(BOARD_CARRIER_TOUCH)
    if (!BOARD_HAS_TOUCH || BOARD_HAS_ENCODER) { printf("    FAIL touch board: touch, no encoder\n"); bad++; }
#else
    if (BOARD_HAS_TOUCH || !BOARD_HAS_ENCODER || BOARD_BUTTONS_ON_EXPANDER) {
        printf("    FAIL knob board: the encoder and keys, direct\n");
        bad++;
    }
#endif
    if (!bad) {
        printf("    no collisions, %zu pins used\n", n);
    }
    *fail += bad;
}
