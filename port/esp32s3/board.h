/*
 * board.h -- every pin, in one place, per board.
 *
 * These used to be #defines scattered across seven port files, and they had
 * drifted into six collisions: three buttons sat on the display's SPI bus, the
 * SD and display shared a chip select, and I2C SCL was the display's DC line.
 * Nothing caught it because no file could see another file's choices.
 *
 * One header, one map per board, and a host test (test/test_board.c) that
 * fails if any two functions claim the same pin. That test is the point of the
 * file -- a pin map is exactly the kind of thing that looks fine in review and
 * only fails once copper exists.
 *
 * Select with BOARD_DEVKIT (44 pins, switches direct) or BOARD_SUPERMINI
 * (14 pins, switches on an I2C expander). Defaults to the devkit.
 *
 * What is fitted, separately from what is wired
 * ---------------------------------------------
 * Each board also declares BOARD_HAS_* flags. They exist so a part can be left
 * off without editing anything: bringing a board up one subsystem at a time is
 * the normal way to do it, and "comment out the SD init" is how that usually
 * gets done and how it usually gets forgotten.
 *
 * Any of them can be overridden from the build:
 *
 *     pio run -e mini-bringup            display and link only
 *     idf.py -DBOARD_HAS_SD=0 build      the same, by hand
 *
 * The card and the display share SPI, so bringing the display up first with
 * the card absent is worth doing deliberately -- with both at once, a bus
 * problem and a driver problem look identical.
 */

#ifndef BOARD_H
#define BOARD_H

#if !defined(BOARD_DEVKIT) && !defined(BOARD_SUPERMINI) && \
    !defined(BOARD_CARRIER_TOUCH) && !defined(BOARD_CARRIER_KNOB)
#define BOARD_DEVKIT 1
#endif

/* ---- ESP32-S3-WROOM-1 devkit, 44 pins ----------------------------------- */
#if defined(BOARD_DEVKIT)

#define BOARD_NAME "devkit"

/*
 * The link port. Four pins, and the multiboot SPI master and the infrared
 * driver both reuse them -- they are different protocols on the same
 * connector, never active at once, so sharing is correct rather than a clash.
 */
#define PIN_LINK_SC   4
#define PIN_LINK_SD   5
#define PIN_LINK_SI   6
#define PIN_LINK_SO   7

/*
 * GB-Link's second SD path. Their adapter takes SD on one pin for a GBA cable
 * and on another for a GBC cable, which leaves pin 4 of the plug unconnected;
 * the firmware picks by sampling SI, by the persisted 0x4B selection, and by
 * its wrong-pin watchdog. A board that wires this pin to the connector the way
 * the GB-Link DIY board does (see doc/GBLINK.md) gets all of that; one that
 * does not reports a GBA cable whatever is plugged in, which is the truth.
 */
#define PIN_LINK_SD_ALT 1
#ifndef BOARD_HAS_SD_ALT
#define BOARD_HAS_SD_ALT 0
#endif

/*
 * GB-Link's bridge UART (2.2.7): the 'GB'-framed transport to a companion MCU,
 * GP8/GP9 at 921600 on the RP2040 board. With it, a GB-Link-Switch-LDN board
 * wired here drives the Wireless Adapter mode exactly as it drives an RP2040
 * GB-Link -- FireRed/LeafGreen against a Switch. Off unless built with
 * -DBOARD_HAS_BRIDGE=1, so an unwired board does not listen on a floating pin.
 * Cross the pair: our TX to the companion's RX.
 */
#define PIN_BRIDGE_TX 47
#define PIN_BRIDGE_RX 2
#ifndef BOARD_HAS_BRIDGE
#define BOARD_HAS_BRIDGE 0
#endif

/*
 * Status LED for the GB-Link personality (mode colours, the 0x42/0x46
 * commands). The DevKitC-1 v1.0 has its WS2812 on GPIO 48. v1.1 moved it to
 * 38, which this map gives to the Start button, so on a v1.1 board the LED
 * stays dark rather than fighting a switch.
 */
#define PIN_RGB_LED 48
#ifndef BOARD_HAS_RGB_LED
#define BOARD_HAS_RGB_LED 1
#endif

/* GB-Link switches link VCC between 3.3 V and 5 V with two load switches;
 * no gbagate board has them (nor does the GB-Link DIY board, which uses a
 * slide switch). 0x40/0x41 are then accepted and do nothing. */
#ifndef BOARD_HAS_VCC_SWITCH
#define BOARD_HAS_VCC_SWITCH 0
#endif

/* Display and card share SPI2; only the chip selects differ. */
#define PIN_SPI_SCK   12
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13   /* the card reads; the panel is write-only */
#define PIN_LCD_CS    14
#define PIN_LCD_DC    21
#define PIN_SD_CS     10

/* I2C, for a chip reader if one is fitted. */
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9

/* Switches, direct to GPIO. Nothing here may touch 0, 3, 45 or 46 -- those
 * are strapping pins and a finger resting on one changes the boot mode. */
#define PIN_BTN_UP     15
#define PIN_BTN_DOWN   16
#define PIN_BTN_LEFT   17
#define PIN_BTN_RIGHT  18
#define PIN_BTN_A      35
#define PIN_BTN_B      36
#define PIN_BTN_SELECT 37
#define PIN_BTN_START  38
#define PIN_BTN_L      39
#define PIN_BTN_R      40

/* Infrared, if fitted. The emitter and detector are ours; the two link pins
 * the protocol uses are the ones above. */
#define PIN_IR_TX_GPIO 41
#define PIN_IR_RX_GPIO 42

#define BOARD_BUTTONS_ON_EXPANDER 0

/*
 * The rotary encoder, in the "encoder" input mode. It needs real GPIOs --
 * ~1 kHz sampling, which an I2C round trip per poll cannot give -- and none
 * were ever assigned, so the default input mode had no pins at all. It takes
 * the five-way's UP, DOWN and centre pins: the two inputs are alternatives
 * (Settings > input), and in encoder mode the switches are not read.
 */
#define BOARD_HAS_ENCODER 1
#define PIN_ENC_A   PIN_BTN_UP
#define PIN_ENC_B   PIN_BTN_DOWN
#define PIN_ENC_SW  PIN_BTN_A

/* Fitted by default on this board. */
#ifndef BOARD_HAS_DISPLAY
#define BOARD_HAS_DISPLAY 1
#endif
#ifndef BOARD_HAS_LINK
#define BOARD_HAS_LINK    1
#endif
#ifndef BOARD_HAS_SD
#define BOARD_HAS_SD      1
#endif
#ifndef BOARD_HAS_BUTTONS
#define BOARD_HAS_BUTTONS 1
#endif
#ifndef BOARD_HAS_IR
#define BOARD_HAS_IR      0   /* the emitter is not on the reference build */
#endif
#ifndef BOARD_HAS_READER
#define BOARD_HAS_READER  0   /* no chip reader on a mini board */
#endif

/* ---- Supermini, 14 usable pins ------------------------------------------ */
#elif defined(BOARD_SUPERMINI)

#define BOARD_NAME "supermini"

/*
 * Fourteen pins: 1..13 and 48. The link, display and card take twelve, which
 * leaves exactly two -- so the switches move to an I2C expander and the I2C
 * pins are the two that remain.
 *
 * There is no room for infrared or a chip reader here, and the variant does
 * not include them.
 */
#define PIN_LINK_SC   4
#define PIN_LINK_SD   5
#define PIN_LINK_SI   6
#define PIN_LINK_SO   7

#define PIN_SPI_SCK   12
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13
#define PIN_LCD_CS    10
#define PIN_LCD_DC    48
#define PIN_SD_CS     3

#define PIN_I2C_SDA   1
#define PIN_I2C_SCL   2

/* No pin for GB-Link's second SD path, and the Supermini's own WS2812 is on
 * GPIO 48, which is the display's DC line here. Neither can be fitted. The
 * LED's data input stays on 48 all the same, so it may light at random as
 * DC toggles; the carrier maps below use the inner rows of pins (14-18, 21,
 * 33-42) and leave 48 to the LED. */
#define PIN_LINK_SD_ALT (-1)
#define BOARD_HAS_SD_ALT 0
#define PIN_RGB_LED (-1)
#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_VCC_SWITCH 0
#define PIN_BRIDGE_TX (-1)       /* no pins left: see below */
#define PIN_BRIDGE_RX (-1)
#define BOARD_HAS_BRIDGE 0

#define BOARD_BUTTONS_ON_EXPANDER 1

/* No encoder: it needs three real GPIOs and this board has two spare
 * (doc/BOARD-MINI.md). A saved "encoder" setting falls back to the
 * switches on the expander. */
#define BOARD_HAS_ENCODER 0

#ifndef BOARD_HAS_DISPLAY
#define BOARD_HAS_DISPLAY 1
#endif
#ifndef BOARD_HAS_LINK
#define BOARD_HAS_LINK    1
#endif
#ifndef BOARD_HAS_SD
#define BOARD_HAS_SD      1
#endif
#ifndef BOARD_HAS_BUTTONS
#define BOARD_HAS_BUTTONS 1
#endif
/*
 * No pins left for either. Twelve of fourteen go to the link, display and
 * card, and the last two are the expander's I2C -- so these are not a choice
 * on this board, and the pin-map test enforces that.
 */
#ifndef BOARD_HAS_IR
#define BOARD_HAS_IR      0
#endif
#ifndef BOARD_HAS_READER
#define BOARD_HAS_READER  0
#endif

/*
 * Switch positions on the expander, not on the chip. GPA0..GPA7 then GPB0,
 * GPB1 -- the same order as the button bits, so no translation table exists to
 * fall out of step with the board.
 */
#define EXP_BTN_UP     0
#define EXP_BTN_DOWN   1
#define EXP_BTN_LEFT   2
#define EXP_BTN_RIGHT  3
#define EXP_BTN_A      4
#define EXP_BTN_B      5
#define EXP_BTN_SELECT 6
#define EXP_BTN_L      7
#define EXP_BTN_R      8
#define EXP_BTN_START  9

#elif defined(BOARD_CARRIER_TOUCH) || defined(BOARD_CARRIER_KNOB)

/*
 * The DIY carrier boards (doc/CARRIER.md): a Supermini, up to three GBA
 * link ports on one Multi-Play chain, the Battle Chip board on I2C, and
 * either a 320x240 touch panel (ILI9341 or ST7789 with an XPT2046) or a
 * 2" ST7789V with a rotary encoder and two keys. The touch carrier's panel
 * and touch drivers are written (lcd_ili9341_esp32s3.c, touch_xpt2046_
 * esp32s3.c) and run on qemu_esp32; the knob carrier's are not, and it is
 * not built yet.
 *
 * The Supermini has two sets of pins. The edge rows (the pin headers)
 * bring out GPIO 1-13, 43 (TX) and 44 (RX); the inner rows, through-holes
 * on the underside, bring out 14-18, 21, 33-42 and 45-48. 45 and 46 are
 * strapping pins and 48 is the board's own RGB LED (its pad is also at the
 * top edge), so neither is used. 33-37 are free because the Supermini's
 * S3FH4R2 has quad PSRAM; an octal-PSRAM module would take them.
 *
 * The core signals keep the edge rows, as before; the inner rows take what
 * the edge rows could not hold, so 43 and 44 stay UART0 (the console, or the
 * bridge UART) and GPIO 3, a strapping pin, is left free.
 *
 * The link ports: port 1 is wired as the single port always was (SI from
 * its GBA's SO, SO to its SI), so every one-cable mode runs on it unchanged.
 * Ports 2 and 3 share SC and SD and continue the chain -- port 1's SO is
 * port 2's SI, port 2's SO port 3's -- and the board reads each GBA's SO
 * (SI1..SI3), so it can take any slot after the last local GBA, or slot 0
 * before them. Fill the ports in order.
 */
#if defined(BOARD_CARRIER_TOUCH)
#define BOARD_NAME "carrier-touch"
#else
#define BOARD_NAME "carrier-knob"
#endif

/* edge rows */
#define PIN_I2C_SDA   1           /* the Battle Chip board                */
#define PIN_I2C_SCL   2
#define PIN_LINK_SC   4
#define PIN_LINK_SD   5
#define PIN_LINK_SI   6           /* port 1's GBA's SO                    */
#define PIN_LINK_SO   7           /* to port 1's GBA's SI                 */
#define PIN_LINK_SI2  8           /* port 2's GBA's SO                    */
#define PIN_LCD_DC    9
#define PIN_LCD_CS    10
#define PIN_SPI_MOSI  11
#define PIN_SPI_SCK   12
#define PIN_SPI_MISO  13          /* card and touch; the panel's SDO is left off */
#define PIN_BRIDGE_TX 43          /* UART0: the console unless the bridge is fitted */
#define PIN_BRIDGE_RX 44
#ifndef BOARD_HAS_BRIDGE
#define BOARD_HAS_BRIDGE 0
#endif

/* inner rows */
#define PIN_LINK_SI3  14          /* port 3's GBA's SO                    */
#define PIN_SD_CS     15
#define PIN_LCD_RST   18
#define PIN_LCD_BL    21          /* backlight, PWM                       */
#define PIN_EXP_INT   36          /* the Battle Chip board's expanders' INT (shared) */
#define BOARD_LINK_PORTS 3

#if defined(BOARD_CARRIER_TOUCH)
#define PIN_TOUCH_CS  16          /* XPT2046                              */
#define PIN_TOUCH_IRQ 17          /* low while touched                    */
#define BOARD_HAS_TOUCH 1
#define BOARD_HAS_ENCODER 0
#define BOARD_BUTTONS_ON_EXPANDER 0
#ifndef BOARD_HAS_BUTTONS
#define BOARD_HAS_BUTTONS 0       /* touch is the input                   */
#endif
#else
/* The panel module's encoder and two keys, direct: the encoder on the
 * pulse counter, as on the devkit. */
#define PIN_ENC_A     16
#define PIN_ENC_B     17
#define PIN_ENC_SW    33
#define PIN_BTN_A     34          /* the module's confirm key             */
#define PIN_BTN_B     35          /* its back key                         */
#define BOARD_HAS_TOUCH 0
#define BOARD_HAS_ENCODER 1
#define BOARD_BUTTONS_ON_EXPANDER 0
#ifndef BOARD_HAS_BUTTONS
#define BOARD_HAS_BUTTONS 1
#endif
#endif

#define PIN_LINK_SD_ALT (-1)
#define BOARD_HAS_SD_ALT 0
#define PIN_RGB_LED 48            /* the Supermini's own WS2812           */
#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_VCC_SWITCH 0

#ifndef BOARD_HAS_DISPLAY
#define BOARD_HAS_DISPLAY 1
#endif
#ifndef BOARD_HAS_LINK
#define BOARD_HAS_LINK    1
#endif
#ifndef BOARD_HAS_SD
#define BOARD_HAS_SD      1
#endif
#ifndef BOARD_HAS_IR
#define BOARD_HAS_IR      0
#endif
#ifndef BOARD_HAS_READER
#define BOARD_HAS_READER  1       /* the Battle Chip board, on I2C        */
#endif

#endif

/*
 * A 5 V level shifter on the link port (a bidirectional one, as the GB-Link
 * DIY board's SparkFun BOB-12009). Game Boy and Game Boy Color links are 5 V
 * and the S3's pins are not 5 V tolerant, so the modes that only ever talk to
 * a Game Boy -- GB Printer here, GB-Link's printer mode 0x03 -- are offered
 * only with one. None of the boards above has one: build with
 * -DBOARD_HAS_GB_5V=1 for a board that does. GBA links are 3.3 V and need none.
 */
#ifndef BOARD_HAS_GB_5V
#define BOARD_HAS_GB_5V 0
#endif

/*
 * Strapping pins. A switch or a bus line on one of these gives a board that
 * sometimes boots into the ROM bootloader instead of the firmware, depending
 * on what a finger was resting on -- which reads as an intermittent hardware
 * fault rather than a pin choice.
 */
#define BOARD_STRAPPING_PINS { 0, 3, 45, 46 }

/* USB D-/D+. Leave them alone or the USB-C port stops flashing and logging. */
#define BOARD_USB_PINS { 19, 20 }

#endif /* BOARD_H */
