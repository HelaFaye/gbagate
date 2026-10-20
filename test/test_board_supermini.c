/*
 * The Supermini pin map, in its own translation unit so both boards' macros
 * can be checked in one run.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define BOARD_SUPERMINI 1
#include "board.h"

void check_supermini(int *fail);

void check_supermini(int *fail) {
    struct { int pin; const char *who; } c[] = {
        { PIN_LINK_SC, "link SC" }, { PIN_LINK_SD, "link SD" },
        { PIN_LINK_SI, "link SI" }, { PIN_LINK_SO, "link SO" },
        { PIN_SPI_SCK, "SPI SCK" }, { PIN_SPI_MOSI, "SPI MOSI" },
        { PIN_SPI_MISO, "SPI MISO" },
        { PIN_LCD_CS, "LCD CS" }, { PIN_LCD_DC, "LCD DC" },
        { PIN_SD_CS, "SD CS" },
        { PIN_I2C_SDA, "I2C SDA" }, { PIN_I2C_SCL, "I2C SCL" },
    };
    size_t n = sizeof c / sizeof c[0];
    printf("  supermini: %zu pins claimed\n", n);

    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (c[i].pin == c[j].pin &&
                !(strstr(c[i].who, "link") && strstr(c[j].who, "link"))) {
                printf("    FAIL GPIO %d: %s vs %s\n",
                       c[i].pin, c[i].who, c[j].who);
                bad++; (*fail)++;
            }
        }
    }

    /*
     * Every pin must be one the board actually brings out: 1..13 and 48.
     * The devkit map uses 15..42, none of which exist here -- which is exactly
     * the mistake this catches when someone copies one map to the other board.
     */
    for (size_t i = 0; i < n; i++) {
        bool ok = (c[i].pin >= 1 && c[i].pin <= 13) || c[i].pin == 48;
        if (!ok) {
            printf("    FAIL %s on GPIO %d, which a Supermini does not expose\n",
                   c[i].who, c[i].pin);
            bad++; (*fail)++;
        }
    }

    /* Twelve of fourteen, leaving two -- which is why the switches are on an
     * expander rather than on GPIO. */
    if (n > 14) {
        printf("    FAIL %zu pins needed, 14 available\n", n);
        (*fail)++;
    }
    if (!bad) {
        printf("    no collisions, %zu of 14 pins used\n", n);
    }
    /*
     * A Supermini has no pins spare for infrared or a chip reader. Claiming
     * either would mean a pin map that cannot exist, so it is an error rather
     * than a preference.
     */
    if (BOARD_HAS_IR) {
        printf("    FAIL supermini has no pins for infrared\n");
        (*fail)++;
    }
    if (BOARD_HAS_READER) {
        printf("    FAIL supermini has no pins for a chip reader\n");
        (*fail)++;
    }
    if (!BOARD_BUTTONS_ON_EXPANDER) {
        printf("    FAIL supermini must put the switches on an expander\n");
        (*fail)++;
    }
}

/* The Supermini has no pin left for GB-Link's second SD path or an LED; the
 * map must say so rather than reuse one. */
#if BOARD_HAS_SD_ALT || BOARD_HAS_RGB_LED || PIN_LINK_SD_ALT >= 0 || PIN_RGB_LED >= 0
#error "supermini claims a GB-Link part it has no pin for"
#endif
