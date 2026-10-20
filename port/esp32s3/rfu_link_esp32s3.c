/*
 * rfu_link_esp32s3.c -- the Wireless Adapter's link port on the ESP32-S3: the
 * gbl_rfu_phy_t under GB-Link's RFU mode (port/gblink/rfu_phy.h has the wire
 * contract; upstream does it with two RP2040 PIO programs, linkLayer_rfu.c).
 *
 * Pins, as upstream's: SC; the GBA's SO arrives on PIN_LINK_SI (our receive)
 * and our transmit goes out on PIN_LINK_SO to the GBA's SI; SD (both paths, if
 * fitted) carries the game's soft-reset pulse.
 *
 * Speed: dedicated GPIO
 * ---------------------
 * The GBA clocks at up to 2 MHz, 250 ns a half period. An ordinary GPIO
 * register access goes over the APB and costs a few hundred nanoseconds
 * (GB-Link-Switch-LDN measured ~403 ns on its ESP32s, and concluded an ESP32
 * cannot do this; its README says so). The S3 has dedicated GPIO: up to eight
 * pins per CPU bundled onto CPU registers, read and written by single
 * instructions. SC and the GBA's SO are an input bundle and our transmit an
 * output bundle, so the bit loop polls and drives in a few cycles. SC stays
 * off the output bundle: a dedicated output's enable comes from the
 * peripheral and is always on, which would fight the GBA's clock; the adapter
 * only clocks SC at ~154 kHz, where ordinary GPIO writes are quick enough.
 *
 * A bundle belongs to the CPU that creates it, so everything that touches
 * these pins runs on one task on core 1: the bundles are made when the mode is
 * entered and deleted when it is left (the Multi-Play engine uses the same
 * pins through ordinary GPIO the rest of the time), and the driver functions
 * other cores call post their pin work to that task through a small mailbox.
 *
 * GBA_MASTER
 * ----------
 * Each word is shifted edge by edge from a polling loop with interrupts off on
 * core 1. That cannot be the whole time -- the tick, the flash cache's IPI and
 * the watchdogs need that core -- and librfu makes it unnecessary: in the
 * command phase the GBA starts a word only after SI has followed its SO
 * (handshake_wait 1, then 0), so while SI is held low it waits. The loop holds
 * SI low ("not ready", what the PIO's `pull` stall shows the GBA upstream)
 * whenever it is not watching with interrupts off, and mirrors SI = NOT SO
 * only while it is:
 *
 *   - nothing staged: SI low; the task sleeps until stage() or the mailbox;
 *   - staged: interrupts off, mirror, wait for SC to fall, shift 32 bits MSB
 *     first, then SI low, interrupts on, gbl_rfu_word_done() in task context
 *     (it takes the shim lock and stages the next answer);
 *   - staged but the GBA quiet: the watch lets go every WATCH_SLICE_US, at
 *     once for the mailbox, and after IDLE_SLEEP_US the task sleeps a tick.
 *
 * The ID exchange (Sio32ID) has no handshake: the GBA clocks ~40 us after the
 * previous word whatever SI says, so a word that lands in a let-go moment is
 * lost; checkID retries, and the slices are short.
 *
 * ADAPTER_MASTER
 * --------------
 * exchange() clocks 32 bits at upstream's ~154 kHz with interrupts off
 * (~210 us); drive_si() sets SI between words. librfu's slave handshake is the
 * section's business, above.
 *
 * SD
 * --
 * An edge interrupt timestamps rising edges; a pulse counts when SD has stayed
 * high RFU_SD_HOLD_US, measured from the edges rather than caught at a poll
 * (the section polls about once a millisecond against a 1.1 ms pulse). The
 * GPIO ISR service lives on core 1 (gba_link_esp32s3.c), so an edge can be
 * seen late by up to a watch slice; a glitch that comes and goes inside one
 * never reads as a rise.
 *
 * NOT YET RUN ON HARDWARE. The protocol above it is tested against pret's
 * librfu in the host simulator (test/gblink_sim/test_rfu.py); the bit loop has
 * not been on a scope.
 */
#include <stdatomic.h>

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "soc/gpio_struct.h"

#include "board.h"
#include "rfu_phy.h"

static const char *TAG = "rfu_phy";

#define PIN_SC PIN_LINK_SC
#define PIN_RX PIN_LINK_SI   /* the GBA's SO */
#define PIN_TX PIN_LINK_SO   /* to the GBA's SI */

#define WATCH_SLICE_US  500u   /* longest stretch with core 1's interrupts off */
#define IDLE_SLEEP_US   2000u  /* staged, GBA quiet this long: sleep a tick    */
#define BIT_TIMEOUT_US  1000u  /* SC stalled mid-word: abandon it              */
#define MASTER_HALF_NS  3250u  /* ~154 kHz, upstream's reverse clock           */

/* ---- dedicated GPIO -------------------------------------------------------- */

static dedic_gpio_bundle_handle_t s_in_bundle, s_out_bundle;
static uint32_t s_m_sc_in, s_m_rx;       /* masks in the CPU's input register  */
static uint32_t s_m_tx;                  /* and in its output register         */

static inline uint32_t IRAM_ATTR in_now(void) { return dedic_gpio_cpu_ll_read_in(); }
static inline void IRAM_ATTR tx_set(bool high) { dedic_gpio_cpu_ll_write_mask(s_m_tx, high ? s_m_tx : 0); }
static inline void IRAM_ATTR sc_set(bool high) {
    if (high) GPIO.out_w1ts = 1u << PIN_SC;
    else      GPIO.out_w1tc = 1u << PIN_SC;
}
static inline void sc_drive(bool on) {
    if (on) GPIO.enable_w1ts = 1u << PIN_SC;
    else    GPIO.enable_w1tc = 1u << PIN_SC;
}

/* Ordinary reads, for the other cores (slow, but only polled). */
static inline bool pin_get(int pin) { return (GPIO.in >> pin) & 1u; }

/* ---- state ------------------------------------------------------------------- */

static TaskHandle_t       s_task;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t           s_cyc_per_us;

static volatile bool      s_enabled;
static volatile int       s_role = RFU_ROLE_GBA_MASTER;
static volatile bool      s_staged_valid;
static volatile uint32_t  s_staged;

/* The mailbox: one request at a time, from whichever core. */
typedef enum { OP_NONE = 0, OP_ENABLE, OP_DISABLE, OP_ROLE, OP_EXCHANGE, OP_SI } op_t;
static portMUX_TYPE       s_box_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile op_t      s_op;
static volatile uint32_t  s_op_arg, s_op_result;
static volatile bool      s_op_done;

/* SD reset pulse */
static const int          k_sd_pins[2] = { PIN_LINK_SD, BOARD_HAS_SD_ALT ? PIN_LINK_SD_ALT : -1 };
static volatile int64_t   s_sd_rise_us[2];
static volatile bool      s_sd_high[2], s_sd_reported[2];
static atomic_int         s_sd_pending = -1;   /* index of a qualified pulse */
static volatile int       s_sd_last = -1;

/* counters for the diagnostics screen */
static volatile uint32_t  s_words, s_aborted;

extern void gba_link_yield(bool yield);

static inline uint32_t IRAM_ATTR cyc(void) { return esp_cpu_get_cycle_count(); }

/* ---- GBA_MASTER: the bit loop ---------------------------------------------------- */

/* Wait for SC to reach level, mirroring SI = NOT SO meanwhile if mirror.
 * False on timeout, or (mirroring) when the mailbox wants the task. */
static inline bool IRAM_ATTR wait_sc(bool level, uint32_t t0, uint32_t limit, bool mirror) {
    const uint32_t want = level ? s_m_sc_in : 0;
    for (;;) {
        const uint32_t in = in_now();
        if ((in & s_m_sc_in) == want) return true;
        if (mirror) {
            tx_set(!(in & s_m_rx));
            if (s_op != OP_NONE) return false;
        }
        if (cyc() - t0 > limit) return false;
    }
}

/* One watch: interrupts off, mirror until SC falls (or the slice ends), then
 * the word. 1 with *rx for a word, 0 for a quiet slice, -1 for a word the GBA
 * abandoned. */
static int IRAM_ATTR watch(uint32_t tx, uint32_t *rx) {
    int result = 0;
    portENTER_CRITICAL(&s_mux);
    const uint32_t t0 = cyc();
    if (wait_sc(false, t0, WATCH_SLICE_US * s_cyc_per_us, true)) {
        uint32_t v = 0;
        result = 1;
        for (int bit = 31; bit >= 0; bit--) {
            /* SC is low: drive this bit, then sample on the rising edge. */
            tx_set((tx >> bit) & 1u);
            if (!wait_sc(true, cyc(), BIT_TIMEOUT_US * s_cyc_per_us, false)) { result = -1; break; }
            v = (v << 1) | ((in_now() & s_m_rx) ? 1u : 0u);
            if (bit && !wait_sc(false, cyc(), BIT_TIMEOUT_US * s_cyc_per_us, false)) { result = -1; break; }
        }
        /* Hold the last bit past the GBA's sample edge (upstream: two
         * 32-cycle nops), then "not ready" until the next is staged. */
        esp_rom_delay_us(1);
        *rx = v;
    }
    tx_set(false);
    portEXIT_CRITICAL(&s_mux);
    return result;
}

/* ---- ADAPTER_MASTER ----------------------------------------------------------------- */

static void IRAM_ATTR spin_ns(uint32_t ns) {
    const uint32_t t0 = cyc(), n = ns * s_cyc_per_us / 1000u;
    while (cyc() - t0 < n) { }
}

static uint32_t IRAM_ATTR master_exchange(uint32_t word) {
    uint32_t v = 0;
    portENTER_CRITICAL(&s_mux);
    for (int bit = 31; bit >= 0; bit--) {
        /* Falling edge with our bit; the GBA shifts its SO bit out on it. */
        sc_set(false);
        tx_set((word >> bit) & 1u);
        spin_ns(MASTER_HALF_NS / 2);
        v = (v << 1) | ((in_now() & s_m_rx) ? 1u : 0u);   /* mid-low, as upstream */
        spin_ns(MASTER_HALF_NS / 2);
        sc_set(true);                                      /* rising: it latches SI */
        spin_ns(MASTER_HALF_NS);
    }
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/* ---- pins in and out of the mode ---------------------------------------------------- */

static void sd_isr(void *arg);

static void sd_arm(bool on) {
    for (int i = 0; i < 2; i++) {
        const int pin = k_sd_pins[i];
        if (pin < 0) continue;
        if (on) {
            gpio_reset_pin(pin);
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);   /* as upstream */
            s_sd_high[i] = pin_get(pin);
            s_sd_reported[i] = s_sd_high[i];               /* not a pulse we saw start */
            gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
            gpio_isr_handler_add(pin, sd_isr, (void *)(intptr_t)i);
            gpio_intr_enable(pin);
        } else {
            gpio_intr_disable(pin);
            gpio_isr_handler_remove(pin);
        }
    }
    atomic_store(&s_sd_pending, -1);
}

/* On core 1 (the bundles bind to the CPU that makes them). */
static void pins_take(void) {
    gba_link_yield(true);
    gpio_intr_disable(PIN_SC);

    /* SC: an input with its pull-up until we clock, its output latch set high
     * first so that driving it later starts without a falling edge. */
    gpio_reset_pin(PIN_SC);
    gpio_set_pull_mode(PIN_SC, GPIO_PULLUP_ONLY);
    sc_set(true);
    sc_drive(false);
    gpio_reset_pin(PIN_RX);
    gpio_set_pull_mode(PIN_RX, GPIO_PULLUP_ONLY);

    const int in_pins[2] = { PIN_SC, PIN_RX };
    const int out_pins[1] = { PIN_TX };
    dedic_gpio_bundle_config_t in_cfg = { .gpio_array = in_pins, .array_size = 2, .flags = { .in_en = 1 } };
    dedic_gpio_bundle_config_t out_cfg = { .gpio_array = out_pins, .array_size = 1, .flags = { .out_en = 1 } };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&in_cfg, &s_in_bundle));
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&out_cfg, &s_out_bundle));
    uint32_t in_off, out_off;
    dedic_gpio_get_in_offset(s_in_bundle, &in_off);
    dedic_gpio_get_out_offset(s_out_bundle, &out_off);
    s_m_sc_in = 1u << in_off;          /* bundle bits follow gpio_array order */
    s_m_rx    = 1u << (in_off + 1);
    s_m_tx    = 1u << out_off;
    tx_set(false);
    sd_arm(true);
}

static void pins_give(void) {
    sd_arm(false);
    sc_drive(false);
    dedic_gpio_del_bundle(s_out_bundle);
    dedic_gpio_del_bundle(s_in_bundle);
    s_out_bundle = s_in_bundle = NULL;
    gpio_reset_pin(PIN_TX);
    gpio_reset_pin(PIN_RX);
    gpio_reset_pin(PIN_SC);
    gba_link_yield(false);
}

static void role_change(enum RfuRole role) {
    if (role == RFU_ROLE_ADAPTER_MASTER) {
        /* Latch high before the driver comes on: no falling edge for the
         * armed GBA to count. SI low, the inverted-ACK lead-in. */
        sc_set(true);
        sc_drive(true);
        tx_set(false);
    } else {
        /* Released at the same level: the latch is high from the last bit. */
        sc_set(true);
        sc_drive(false);
        tx_set(false);
    }
    s_role = role;
}

/* ---- the task ---------------------------------------------------------------------- */

static void serve_mailbox(void) {
    const op_t op = s_op;
    if (op == OP_NONE) return;
    switch (op) {
    case OP_ENABLE:   pins_take(); s_role = RFU_ROLE_GBA_MASTER; s_enabled = true; break;
    case OP_DISABLE:  s_enabled = false; pins_give(); break;
    case OP_ROLE:     role_change((enum RfuRole)s_op_arg); break;
    case OP_EXCHANGE: s_op_result = master_exchange(s_op_arg); break;
    case OP_SI:       tx_set(s_op_arg != 0); break;
    default: break;
    }
    s_op = OP_NONE;
    s_op_done = true;
}

static void rfu_task(void *arg) {
    (void)arg;
    uint32_t quiet_since = cyc();
    for (;;) {
        serve_mailbox();
        if (!s_enabled || s_role != RFU_ROLE_GBA_MASTER || !s_staged_valid) {
            if (s_enabled && s_role == RFU_ROLE_GBA_MASTER) tx_set(false);
            if (s_role == RFU_ROLE_ADAPTER_MASTER && s_enabled) {
                /* A delivery is under way: the section posts words and SI
                 * levels back to back, so keep answering at once. */
                taskYIELD();
            } else {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            }
            quiet_since = cyc();
            continue;
        }
        uint32_t rx;
        const int r = watch(s_staged, &rx);
        if (r == 1) {
            s_staged_valid = false;
            s_words++;
            gbl_rfu_word_done(rx);
            quiet_since = cyc();
        } else if (r < 0) {
            s_aborted++;
            quiet_since = cyc();
        } else if (s_op == OP_NONE && cyc() - quiet_since > IDLE_SLEEP_US * s_cyc_per_us) {
            vTaskDelay(1);                 /* SI is low: the GBA waits */
            quiet_since = cyc();
        }
    }
}

/* Post pin work to the core-1 task and wait for it. From that task itself
 * (the transfer callback) it runs in place. */
static uint32_t post(op_t op, uint32_t arg) {
    if (xTaskGetCurrentTaskHandle() == s_task) {
        s_op_arg = arg;
        s_op = op;
        serve_mailbox();
        return s_op_result;
    }
    portENTER_CRITICAL(&s_box_mux);
    while (s_op != OP_NONE) {          /* one at a time */
        portEXIT_CRITICAL(&s_box_mux);
        taskYIELD();
        portENTER_CRITICAL(&s_box_mux);
    }
    s_op_done = false;
    s_op_arg = arg;
    s_op = op;
    portEXIT_CRITICAL(&s_box_mux);
    xTaskNotifyGive(s_task);
    while (!s_op_done) {
        if (op == OP_ENABLE || op == OP_DISABLE) vTaskDelay(1);
    }
    return s_op_result;
}

/* ---- SD ------------------------------------------------------------------------- */

static void IRAM_ATTR sd_isr(void *arg) {
    const int i = (int)(intptr_t)arg;
    const bool high = (GPIO.in >> k_sd_pins[i]) & 1u;
    const int64_t now = esp_timer_get_time();
    if (high && !s_sd_high[i]) {
        s_sd_rise_us[i] = now;
        s_sd_reported[i] = false;
    } else if (!high && s_sd_high[i] && !s_sd_reported[i] &&
               now - s_sd_rise_us[i] >= RFU_SD_HOLD_US) {
        atomic_store(&s_sd_pending, i);
        s_sd_reported[i] = true;
    }
    s_sd_high[i] = high;
}

/* ---- the driver ------------------------------------------------------------------ */

static void drv_enable(void) {
    s_staged_valid = false;
    post(OP_ENABLE, 0);
}

static void drv_disable(void) { post(OP_DISABLE, 0); }

static void drv_set_role(enum RfuRole role) {
    s_staged_valid = false;
    post(OP_ROLE, (uint32_t)role);
}

static void drv_stage(uint32_t word) {
    s_staged = word;
    s_staged_valid = true;
    /* From the core-1 task's own callback this is just the flag; from
     * another core, wake it. */
    if (xTaskGetCurrentTaskHandle() != s_task) xTaskNotifyGive(s_task);
}

static bool drv_exchange(uint32_t word, uint32_t *rx, uint32_t timeout_us) {
    (void)timeout_us;   /* no pin waits: it cannot hang */
    if (s_role != RFU_ROLE_ADAPTER_MASTER) return false;
    *rx = post(OP_EXCHANGE, word);
    return true;
}

static void drv_drive_si(bool high) { post(OP_SI, high ? 1u : 0u); }

static bool drv_so_high(void) { return pin_get(PIN_RX); }

static void drv_realign(void) {
    /* No state machine to restart: the bit loop starts each word afresh, so
     * only the staged answer goes. */
    s_staged_valid = false;
}

static bool drv_sd_reset_seen(uint8_t *pin) {
    int i = atomic_exchange(&s_sd_pending, -1);
    if (i < 0) {
        /* Still high and already long enough. */
        for (int j = 0; j < 2 && i < 0; j++) {
            if (k_sd_pins[j] < 0 || !s_sd_high[j] || s_sd_reported[j]) continue;
            if (esp_timer_get_time() - s_sd_rise_us[j] >= RFU_SD_HOLD_US) {
                s_sd_reported[j] = true;
                i = j;
            }
        }
    }
    if (i < 0) return false;
    s_sd_last = i;
    *pin = (uint8_t)k_sd_pins[i];
    return true;
}

static void drv_wait_reset_end(void) {
    if (s_sd_last < 0) return;
    const int64_t start = esp_timer_get_time();
    while (pin_get(k_sd_pins[s_sd_last]) && esp_timer_get_time() - start < RFU_SD_PULSE_MAX_US) { }
    esp_rom_delay_us(RFU_SD_SETTLE_US);
}

static uint8_t drv_lines(void) {
    return (uint8_t)((pin_get(PIN_RX) ? 1 : 0) | (pin_get(PIN_TX) ? 2 : 0) | (pin_get(PIN_SC) ? 4 : 0));
}

static const gbl_rfu_phy_t k_driver = {
    .enable = drv_enable,
    .disable = drv_disable,
    .set_role = drv_set_role,
    .stage = drv_stage,
    .exchange = drv_exchange,
    .drive_si = drv_drive_si,
    .so_high = drv_so_high,
    .realign = drv_realign,
    .sd_reset_seen = drv_sd_reset_seen,
    .wait_reset_end = drv_wait_reset_end,
    .lines = drv_lines,
};

void rfu_link_start(void) {
    if (s_task) return;
    s_cyc_per_us = (uint32_t)(esp_clk_cpu_freq() / 1000000);
    xTaskCreatePinnedToCore(rfu_task, "rfu_phy", 4096, NULL, configMAX_PRIORITIES - 1, &s_task, 1);
    gbl_rfu_install(&k_driver);
    ESP_LOGI(TAG, "Wireless Adapter PHY on core 1 (dedicated GPIO): SC=%d RX=%d TX=%d SD=%d/%d",
             PIN_SC, PIN_RX, PIN_TX, k_sd_pins[0], k_sd_pins[1]);
}

void rfu_link_counters(uint32_t *words, uint32_t *aborted) {
    if (words)   *words = s_words;
    if (aborted) *aborted = s_aborted;
}
