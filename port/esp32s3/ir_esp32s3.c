/*
 * ir_esp32s3.c -- the AGB-006 infrared adapter, in hardware.
 *
 * BRING-UP SCAFFOLDING. The protocol underneath (src/ir_cdz.c) is solved and
 * tested; the timing in this file is not. Nothing here has met a scope, and
 * the numbers most likely to be wrong are called out below. Treat it as a
 * starting point for a bench session, not as a finished driver.
 *
 * What this replaces
 * ------------------
 * The real adapter is a dumb transceiver on the end of the link cable. The
 * game drives it through RCNT in General Purpose mode:
 *
 *   SO high -> LED on, SO low -> LED off      (RCNT bit 3, values 0x80BE/0x80B6)
 *   SI      -> the adapter pulls it to raise the game's serial interrupt
 *
 * So we watch SO and mirror it onto a real LED, and when our receiver sees
 * light we pulse SI. The game detects the adapter's presence by sending a
 * pulse and expecting an interrupt back, which falls out of doing both.
 *
 * The receiver must not be a TSOP
 * -------------------------------
 * Every common IR receiver module demodulates a 36-40 kHz carrier and outputs
 * nothing without one. The GBA drives the LED as a bare DC pulse -- there is no
 * carrier anywhere in this protocol -- so a TSOP38238 and its relatives will
 * sit silent no matter how correct the rest of this is. Use a photodiode or
 * phototransistor into a comparator, or a photodiode straight into a GPIO with
 * a pull-up if the range is short.
 *
 * That is the single most likely reason for a first bring-up to see nothing,
 * and it looks exactly like a software fault.
 */

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "board.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ir_cdz.h"

static const char *TAG = "ir";

/*
 * Pins. SO and SI are the link connector's; the LED and detector are ours.
 *
 * Do not drive the LED straight off a GPIO: an IR emitter wants tens of
 * milliamps for any useful range and the pin will not give it. A small NPN
 * with a base resistor, LED to 3V3 through a current-limiting resistor.
 */
#define PIN_GBA_SO   PIN_LINK_SO    /* link pin 2, the game's output: LED command */
#define PIN_GBA_SI   PIN_LINK_SI    /* link pin 3, the game's input: our interrupt */
#define PIN_IR_TX    PIN_IR_TX_GPIO    /* transistor base */
#define PIN_IR_RX    PIN_IR_RX_GPIO    /* detector, active low */

/*
 * SCOPE: the interrupt pulse width. The adapter raises the game's serial
 * interrupt by moving SI; how long it must be held is not documented and
 * 20 us is a guess wide enough for the GBA to sample and short enough not to
 * overlap the next mini pulse at ~19 us. If presence detection fails, this is
 * the first number to change.
 */
#define SI_PULSE_US  20

/*
 * SCOPE: how long the LED stays on per edge. The article establishes that the
 * on time is brief and constant and that the *gaps* carry the data, but not
 * what the on time is. Long enough for a detector to register, short enough to
 * be finished before the shortest gap.
 */
#define TX_ON_US     8

static cdz_rx_t s_rx;
static volatile uint16_t s_last;
static volatile bool     s_ready;
static volatile uint32_t s_edges;

/*
 * Both handlers run from GPIO interrupts, so everything they touch is in IRAM:
 * a flash write elsewhere suspends the cache, and a handler that faults out to
 * flash mid-pulse loses the edge that was being timed. esp_timer_get_time() is
 * IRAM-resident in ESP-IDF and safe to call here.
 */

/* The detector saw light. Time the edge and hand it to the decoder. */
static void IRAM_ATTR ir_rx_isr(void *arg) {
    (void)arg;
    s_edges++;
    uint32_t now = (uint32_t)esp_timer_get_time();

    uint16_t cmd;
    if (cdz_rx_edge(&s_rx, now, &cmd)) {
        s_last = cmd;
    }

    /*
     * Tell the game something arrived. The adapter interrupts on no-light to
     * light only -- never on light going away -- so this fires once per pulse
     * and not twice.
     */
    gpio_set_level(PIN_GBA_SI, 0);
    esp_rom_delay_us(SI_PULSE_US);
    gpio_set_level(PIN_GBA_SI, 1);
}

/* The game raised SO: emit one pulse. */
static void IRAM_ATTR ir_so_isr(void *arg) {
    (void)arg;
    gpio_set_level(PIN_IR_TX, 1);
    esp_rom_delay_us(TX_ON_US);
    gpio_set_level(PIN_IR_TX, 0);

    /* Sending also raises the game's interrupt, which is how presence
     * detection succeeds: the game pulses SO and expects an IRQ back. */
    gpio_set_level(PIN_GBA_SI, 0);
    esp_rom_delay_us(SI_PULSE_US);
    gpio_set_level(PIN_GBA_SI, 1);
}

/* ---- the interface gate_app.c expects ---------------------------------- */

bool ir_ready(void)        { return s_ready; }
uint32_t ir_commands(void) { return s_rx.commands; }
uint32_t ir_aborted(void)  { return s_rx.aborted; }
uint16_t ir_last_command(void) { return s_last; }

/*
 * Send a command ourselves, for driving a Zoid without a console attached.
 * Blocking, and deliberately not from an interrupt: a command is twelve gaps
 * plus a ~114 ms terminator, so it is the better part of a second.
 */
bool ir_send(uint16_t cmd) {
    if (!s_ready) {
        return false;
    }

    uint32_t gaps[CDZ_CMD_BITS + 1];
    size_t n = cdz_encode(cmd, gaps, sizeof gaps / sizeof gaps[0]);
    if (n == 0) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        gpio_set_level(PIN_IR_TX, 1);
        esp_rom_delay_us(TX_ON_US);
        gpio_set_level(PIN_IR_TX, 0);

        /*
         * The gap follows the pulse it belongs to. Long gaps are milliseconds,
         * so busy-waiting through the terminator would hold the CPU for a
         * tenth of a second -- delay in ticks past a threshold and spin only
         * where the resolution is needed.
         */
        uint32_t gap = gaps[i];
        if (gap > 2000u) {
            vTaskDelay(pdMS_TO_TICKS(gap / 1000u));
        } else {
            esp_rom_delay_us(gap);
        }
    }
    return true;
}

esp_err_t ir_init(void) {
    cdz_rx_init(&s_rx);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_IR_TX) | (1ULL << PIN_GBA_SI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out) != ESP_OK) {
        return ESP_FAIL;
    }
    gpio_set_level(PIN_IR_TX, 0);
    gpio_set_level(PIN_GBA_SI, 1);   /* idle high; we pull low to interrupt */

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_IR_RX) | (1ULL << PIN_GBA_SO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&in) != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * Detector active low, so light is a falling edge. SO is the game driving
     * high, so a rising edge. Getting either backwards produces a decoder that
     * times the gaps between the wrong things and aborts everything -- which
     * is what the aborted counter on the Infrared screen is for.
     */
    gpio_set_intr_type(PIN_IR_RX, GPIO_INTR_NEGEDGE);
    gpio_set_intr_type(PIN_GBA_SO, GPIO_INTR_POSEDGE);

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;   /* INVALID_STATE just means someone installed it first */
    }
    gpio_isr_handler_add(PIN_IR_RX, ir_rx_isr, NULL);
    gpio_isr_handler_add(PIN_GBA_SO, ir_so_isr, NULL);

    s_ready = true;
    ESP_LOGI(TAG, "infrared up: SO=%d SI=%d TX=%d RX=%d",
             PIN_GBA_SO, PIN_GBA_SI, PIN_IR_TX, PIN_IR_RX);
    ESP_LOGW(TAG, "timing unverified; see the SCOPE notes in this file");
    return ESP_OK;
}
