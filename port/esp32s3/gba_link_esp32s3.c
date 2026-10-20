/*
 * gba_link_esp32s3.c -- GBA Multi-Play PHY for the ESP32-S3: one engine, two
 * clients.
 *
 *   gate    the Battle Chip Gate (and anything else gbagate answers for
 *           itself), through link_source's gba_link_poll/stage/last;
 *   gblink  GB-Link's link layer (port/gblink/link_layer_core.c), through
 *           the gbl_phy_driver_t installed by gba_link_install_gblink().
 *
 * Only one owns the port at a time. A GB-Link host that selects a link mode
 * takes it; leaving the mode hands it back.
 *
 * The wire, as GB-Link's RP2040 PIO programs drive it
 * ---------------------------------------------------
 * The previous version of this file clocked SC once per bit and sent data MSB
 * first. Neither is how Multi-Play works, and GB-Link's firmware -- proven
 * against real games -- shows what does:
 *
 *   - SC is not a bit clock. The parent pulls it low for the whole transfer
 *     (it is the "busy" signal) and lets it go afterwards.
 *   - SD carries asynchronous serial at 115200 baud, 8.68 us a bit: start bit
 *     low, 16 data bits LEAST significant first, stop bit high. The receiver
 *     times bits from the start edge and samples mid-bit, like a UART.
 *   - SD is driven push-pull while a side transmits, then released. An open
 *     drain with the GBA's pull-up rises far too slowly for 8.68 us bits on a
 *     cable.
 *   - The parent sends, drops SO to pass the token, waits up to ~170 us for
 *     the child's start bit, receives one word, then releases SC. A child that
 *     never starts reads as 0xFFFF.
 *   - As child, the adapter holds SO low (which is what tells the GBA it is
 *     the parent), times the parent's word from SC falling, waits ~17 us after
 *     it, and answers.
 *
 * The constants below are the PIO programs' delays converted at their
 * 542.5 ns per instruction, so each is traceable to pio_master_mode.pio /
 * pio_slave_mode.pio upstream.
 *
 * Roles on the ESP32-S3
 * ---------------------
 * Child: a GPIO interrupt on SC falling, on core 1, IRAM, does the whole
 * exchange (~330 us) -- the answer must already be staged, so the client is
 * asked for it right after the previous word completes (see link_phy.h for
 * why that ordering is equivalent to upstream's).
 *
 * Parent: a task on core 1 at top priority does each word with interrupts off
 * on that core, then spends the inter-word gap sleeping when it is long and
 * spinning when it is short.
 *
 * NOT YET RUN ON HARDWARE. Everything above the bit level is exercised by the
 * host simulator (test/gblink_sim); the bit level is traceable to upstream's
 * PIO but has not been on a scope.
 */
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

#include "board.h"
#include "link_phy.h"
#include "link_proto.h"

static const char *TAG = "mp_phy";

/* ---- timing, from the PIO programs (542.5 ns per instruction) ------------ */

#define NS_BIT            8680u   /* 16 instructions: 115200 baud            */
#define NS_SLAVE_SAMPLE0  13020u  /* "irq [23]": 24 instr after SC falls     */
#define NS_SLAVE_TURN     17360u  /* "nop [31]": 32 instr before answering   */
#define NS_MASTER_HOLD    16820u  /* "set pins [30]": SC low after the reply */
#define NS_CHILD_TIMEOUT  168000u /* 31 x 10-instr wait loop                 */
#define NS_START_TO_MID   12480u  /* "set x [22]": 23 instr, edge to mid-bit */

/* ---- pins ------------------------------------------------------------------ */

static int s_pin_sd = PIN_LINK_SD;

static inline uint32_t IRAM_ATTR in_word(void) { return GPIO.in; }

static inline bool IRAM_ATTR pin_get(int pin) { return (GPIO.in >> pin) & 1u; }

static inline void IRAM_ATTR pin_set(int pin, bool high) {
    if (high) GPIO.out_w1ts = 1u << pin;
    else      GPIO.out_w1tc = 1u << pin;
}

static inline void IRAM_ATTR pin_drive(int pin, bool on) {
    if (on) GPIO.enable_w1ts = 1u << pin;
    else    GPIO.enable_w1tc = 1u << pin;
}

/* Every link pin is a plain GPIO below 32 on both boards; the fast paths above
 * rely on that, so refuse to build otherwise. */
_Static_assert(PIN_LINK_SC < 32 && PIN_LINK_SD < 32 && PIN_LINK_SI < 32 &&
               PIN_LINK_SO < 32 && PIN_LINK_SD_ALT < 32,
               "link pins must be GPIO 0..31");

static uint32_t s_cyc_per_us;

static inline uint32_t IRAM_ATTR ns_to_cyc(uint32_t ns) {
    return (uint32_t)(((uint64_t)ns * s_cyc_per_us) / 1000u);
}

static inline void IRAM_ATTR wait_until(uint32_t t) {
    while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0) { }
}

/* ---- one word on the wire ---------------------------------------------------- */

/* Drive start, 16 bits LSB first, stop on the active SD pin, push-pull. */
static void IRAM_ATTR send_word(uint16_t w, uint32_t t) {
    const uint32_t bit = ns_to_cyc(NS_BIT);
    pin_set(s_pin_sd, false);
    pin_drive(s_pin_sd, true);
    t += bit;
    wait_until(t);
    for (int i = 0; i < 16; i++) {
        pin_set(s_pin_sd, (w >> i) & 1u);
        t += bit;
        wait_until(t);
    }
    pin_set(s_pin_sd, true);
    t += bit;
    wait_until(t);
}

/* Sample 16 bits LSB first, the first at t. */
static uint16_t IRAM_ATTR recv_bits(uint32_t t) {
    const uint32_t bit = ns_to_cyc(NS_BIT);
    uint16_t w = 0;
    for (int i = 0; i < 16; i++) {
        wait_until(t);
        w |= (uint16_t)(pin_get(s_pin_sd) ? 1u : 0u) << i;
        t += bit;
    }
    return w;
}

/* Parent: our word, then one child's. Interrupts must be off on this core. */
static uint16_t IRAM_ATTR master_word(uint16_t tx) {
    uint32_t t = esp_cpu_get_cycle_count();
    pin_set(PIN_LINK_SC, false);           /* busy                        */
    send_word(tx, t);
    pin_drive(s_pin_sd, false);            /* release SD for the child    */
    pin_set(PIN_LINK_SO, false);           /* pass the token              */

    uint16_t rx = 0xFFFF;
    uint32_t t0 = esp_cpu_get_cycle_count();
    const uint32_t limit = ns_to_cyc(NS_CHILD_TIMEOUT);
    while (esp_cpu_get_cycle_count() - t0 < limit) {
        if (!pin_get(s_pin_sd)) {
            rx = recv_bits(esp_cpu_get_cycle_count() + ns_to_cyc(NS_START_TO_MID));
            break;
        }
    }

    pin_set(PIN_LINK_SO, true);
    wait_until(esp_cpu_get_cycle_count() + ns_to_cyc(NS_MASTER_HOLD));
    pin_set(PIN_LINK_SC, true);            /* idle                        */
    return rx;
}

/* ---- engine state ------------------------------------------------------------- */

typedef enum { OWNER_NONE = 0, OWNER_GATE, OWNER_GBLINK } owner_t;

static volatile gbl_phy_mode_t s_mode = GBL_PHY_OFF;
static volatile owner_t        s_owner = OWNER_NONE;
static volatile uint16_t       s_staged = 0xFFFF;   /* child's next answer   */
static volatile uint16_t       s_last_tx = 0xFFFF;
static QueueHandle_t           s_words;             /* ISR -> task: rx words */
static TaskHandle_t            s_task;
static portMUX_TYPE            s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Counters for the diagnostics screen. */
static volatile uint32_t s_words_done, s_rx_dropped, s_late_stage;
static volatile bool     s_stage_fresh;

/* ---- child: the SC-falling interrupt -------------------------------------------- */

static void IRAM_ATTR sc_isr(void *arg) {
    (void)arg;
    uint32_t t_isr = esp_cpu_get_cycle_count();
    GPIO.status_w1tc = 1u << PIN_LINK_SC;

    gbl_phy_mode_t mode = s_mode;
    if (mode != GBL_PHY_SLAVE && mode != GBL_PHY_SLAVE_EREADER) return;

    /* ISR entry costs ~1 us on this part; sample from the estimated edge. */
    uint32_t t_fall = t_isr - s_cyc_per_us;
    uint16_t rx = recv_bits(t_fall + ns_to_cyc(NS_SLAVE_SAMPLE0));

    uint16_t tx = s_staged;
    if (!s_stage_fresh) s_late_stage++;
    s_stage_fresh = false;

    uint32_t t = esp_cpu_get_cycle_count() + ns_to_cyc(NS_SLAVE_TURN);
    wait_until(t);
    send_word(tx, t);
    pin_drive(s_pin_sd, false);

    s_last_tx = tx;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_words, &rx, &woken) != pdTRUE) s_rx_dropped++;
    portYIELD_FROM_ISR(woken);
}

/* ---- pin states per mode ---------------------------------------------------------- */

static void cfg_input(int pin, bool pullup) {
    if (pin < 0) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}

static void cfg_output(int pin, bool level) {
    if (pin < 0) return;
    gpio_reset_pin(pin);
    gpio_set_level(pin, level);
    /* Input-output so pin_get() still reads it, and so pin_drive() can turn
     * the driver off without touching the matrix. */
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
}

static void apply_pins(gbl_phy_mode_t mode, bool sd_alt) {
    gpio_intr_disable(PIN_LINK_SC);
    int sd = (sd_alt && BOARD_HAS_SD_ALT) ? PIN_LINK_SD_ALT : PIN_LINK_SD;
    int other = (sd == PIN_LINK_SD) ? (BOARD_HAS_SD_ALT ? PIN_LINK_SD_ALT : -1) : PIN_LINK_SD;
    s_pin_sd = sd;

    switch (mode) {
    case GBL_PHY_MASTER:
        cfg_output(PIN_LINK_SC, true);
        cfg_output(PIN_LINK_SO, true);
        cfg_input(PIN_LINK_SI, true);
        cfg_output(sd, true);
        pin_drive(sd, false);          /* driven only while sending */
        gpio_set_pull_mode(sd, GPIO_PULLUP_ONLY);
        cfg_input(other, false);
        break;
    case GBL_PHY_SLAVE:
    case GBL_PHY_SLAVE_EREADER:
        cfg_input(PIN_LINK_SC, true);
        cfg_input(PIN_LINK_SI, true);
        cfg_output(PIN_LINK_SO, false); /* SO low: the GBA becomes parent */
        cfg_output(sd, true);
        pin_drive(sd, false);
        gpio_set_pull_mode(sd, GPIO_PULLUP_ONLY);
        cfg_input(other, false);
        gpio_set_intr_type(PIN_LINK_SC, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(PIN_LINK_SC);
        break;
    case GBL_PHY_PRESENCE:
        cfg_input(PIN_LINK_SC, true);
        cfg_input(PIN_LINK_SI, true);
        cfg_output(PIN_LINK_SO, false);
        cfg_output(PIN_LINK_SD, true);
        if (BOARD_HAS_SD_ALT) cfg_output(PIN_LINK_SD_ALT, true);
        break;
    case GBL_PHY_OFF:
    default:
        cfg_input(PIN_LINK_SC, true);
        cfg_input(PIN_LINK_SI, false);
        cfg_input(PIN_LINK_SO, false);
        cfg_input(PIN_LINK_SD, true);
        if (BOARD_HAS_SD_ALT) cfg_input(PIN_LINK_SD_ALT, false);
        break;
    }
}

/* ---- the task: parent transfers, and child bookkeeping -------------------------- */

static void deliver(uint16_t rx);

static void IRAM_ATTR phy_task(void *arg) {
    (void)arg;
    for (;;) {
        gbl_phy_mode_t mode = s_mode;

        if (mode == GBL_PHY_MASTER && s_owner == OWNER_GBLINK) {
            uint32_t gap = 0;
            uint16_t tx = gbl_phy_next_tx(&gap);
            portENTER_CRITICAL(&s_mux);
            uint16_t rx = master_word(tx);
            portEXIT_CRITICAL(&s_mux);
            s_last_tx = tx;
            s_words_done++;
            gbl_phy_word_done(rx);

            /* The gap after this word: sleep the whole ticks of it, spin the
             * rest. 0.75 ms between words spins; 7 and 16 ms mostly sleep. */
            uint64_t ns = gbl_phy_loops_to_ns(gap);
            uint32_t end = esp_cpu_get_cycle_count() + ns_to_cyc((uint32_t)(ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : ns));
            uint32_t ms = (uint32_t)(ns / 1000000u);
            if (ms >= 2) vTaskDelay(pdMS_TO_TICKS(ms - 1));
            wait_until(end);
            continue;
        }

        if (mode == GBL_PHY_SLAVE || mode == GBL_PHY_SLAVE_EREADER) {
            uint16_t rx;
            if (xQueueReceive(s_words, &rx, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_words_done++;
                deliver(rx);
            }
            continue;
        }

        /* Off or presence: nothing to clock. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

/* ---- gate client: link_source's hardware source ------------------------------------ */

static QueueHandle_t  s_gate_words;
static gba_transfer_t s_gate_last;

static void deliver(uint16_t rx) {
    if (s_owner == OWNER_GBLINK) {
        gbl_phy_word_done(rx);
        s_staged = gbl_phy_next_tx(NULL);
        s_stage_fresh = true;
        return;
    }
    if (s_owner == OWNER_GATE) {
        memset(&s_gate_last, 0, sizeof s_gate_last);
        s_gate_last.word[0] = rx;
        s_gate_last.word[1] = s_last_tx;
        s_gate_last.word[2] = GBA_WORD_EMPTY;
        s_gate_last.word[3] = GBA_WORD_EMPTY;
        s_gate_last.players = 2;
        s_gate_last.self_id = 1;
        xQueueOverwrite(s_gate_words, &rx);
    }
}

static void set_mode(owner_t owner, gbl_phy_mode_t mode, bool sd_alt) {
    s_owner = owner;
    s_mode = GBL_PHY_OFF;          /* the ISR ignores edges while we rewire */
    xQueueReset(s_words);
    apply_pins(mode, sd_alt);
    s_mode = mode;
    if (s_task) xTaskNotifyGive(s_task);
}

/* GB-Link's Wireless Adapter mode drives these pins in 32-bit Normal mode
 * through its own driver (rfu_link_esp32s3.c); while it does, neither client
 * here may take them. */
static volatile bool s_yielded;

void gba_link_yield(bool yield) {
    if (yield) set_mode(OWNER_NONE, GBL_PHY_OFF, false);
    s_yielded = yield;
}

/* Wait for the parent's next word. The gate answers it through stage(). */
bool gba_link_poll(uint16_t *from_game, uint32_t timeout_ms) {
    if (s_owner == OWNER_GBLINK || s_yielded) {  /* a GB-Link host has the port */
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    if (s_owner != OWNER_GATE) set_mode(OWNER_GATE, GBL_PHY_SLAVE, false);
    uint16_t w;
    if (xQueueReceive(s_gate_words, &w, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    *from_game = w;
    return true;
}

/* The answer for the parent's next word. */
void gba_link_stage(uint16_t word) {
    s_staged = word;
    s_stage_fresh = true;
}

void gba_link_last(gba_transfer_t *out) { *out = s_gate_last; }

/* ---- GB-Link client --------------------------------------------------------------------- */

static void gbl_configure(gbl_phy_mode_t mode, bool sd_alt) {
    /* Called with the shim lock held, from GB-Link's module thread. Taking
     * the port from the gate is deliberate: the host asked for this mode. */
    if (mode == GBL_PHY_OFF) {
        set_mode(OWNER_NONE, GBL_PHY_OFF, false);
        return;
    }
    set_mode(OWNER_GBLINK, mode, sd_alt);
    if (mode == GBL_PHY_SLAVE || mode == GBL_PHY_SLAVE_EREADER) {
        /* Stage the first answer before the GBA's first edge. */
        s_staged = gbl_phy_next_tx(NULL);
        s_stage_fresh = true;
    }
}

static bool gbl_si_seen_high(uint32_t window_us) {
    gpio_set_direction(PIN_LINK_SI, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_LINK_SI, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(10);                    /* settle, as upstream */
    int64_t end = esp_timer_get_time() + window_us;
    while (esp_timer_get_time() < end) {
        if (pin_get(PIN_LINK_SI)) return true;
    }
    return false;
}

static uint8_t gbl_read_pins(void) {
    uint32_t in = in_word();
    uint8_t m = 0;
    if (!((in >> PIN_LINK_SC) & 1u)) m |= 0x01;
    if ((in >> PIN_LINK_SI) & 1u)    m |= 0x02;
    if ((in >> PIN_LINK_SO) & 1u)    m |= 0x04;
    if ((in >> PIN_LINK_SD) & 1u)    m |= 0x08;
#if BOARD_HAS_SD_ALT
    if ((in >> PIN_LINK_SD_ALT) & 1u) m |= 0x10;
#endif
    return m;
}

static void gbl_release(void) {
    cfg_input(PIN_LINK_SO, false);
    cfg_input(PIN_LINK_SD, false);
    if (BOARD_HAS_SD_ALT) cfg_input(PIN_LINK_SD_ALT, false);
}

static const gbl_phy_driver_t k_gblink_driver = {
    .configure    = gbl_configure,
    .si_seen_high = gbl_si_seen_high,
    .read_pins    = gbl_read_pins,
    .release      = gbl_release,
    .has_sd_alt   = BOARD_HAS_SD_ALT,
};

/* ---- setup -------------------------------------------------------------------------------- */

static void install_isr_on_core1(void *arg) {
    /* An interrupt is serviced on the core that allocated it; the child
     * exchange must run where WiFi and USB do not. */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_LINK_SC, sc_isr, NULL));
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

void gba_link_start(void) {
    if (s_task) return;
    s_cyc_per_us = (uint32_t)(esp_clk_cpu_freq() / 1000000);
    s_words = xQueueCreate(8, sizeof(uint16_t));
    s_gate_words = xQueueCreate(1, sizeof(uint16_t));
    apply_pins(GBL_PHY_OFF, false);

    xTaskCreatePinnedToCore(install_isr_on_core1, "mp_isr", 3072,
                            xTaskGetCurrentTaskHandle(), configMAX_PRIORITIES - 1, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xTaskCreatePinnedToCore(phy_task, "mp_phy", 4096, NULL,
                            configMAX_PRIORITIES - 1, &s_task, 1);
    gbl_phy_install(&k_gblink_driver);
    ESP_LOGI(TAG, "Multi-Play PHY on core 1: SC=%d SD=%d SI=%d SO=%d SD_ALT=%d",
             PIN_LINK_SC, PIN_LINK_SD, PIN_LINK_SI, PIN_LINK_SO,
             BOARD_HAS_SD_ALT ? PIN_LINK_SD_ALT : -1);
}

void gba_link_counters(uint32_t *words, uint32_t *dropped, uint32_t *late) {
    if (words)   *words = s_words_done;
    if (dropped) *dropped = s_rx_dropped;
    if (late)    *late = s_late_stage;
}
