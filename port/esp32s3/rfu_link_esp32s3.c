/*
 * rfu_link_esp32s3.c -- the Wireless Adapter's link port on the ESP32-S3: the
 * gbl_rfu_phy_t under GB-Link's RFU mode (port/gblink/rfu_phy.h has the wire
 * contract; upstream does it with two RP2040 PIO programs, linkLayer_rfu.c).
 *
 * Pins, as upstream's: SC; the GBA's SO arrives on PIN_LINK_SI (our receive)
 * and our transmit goes out on PIN_LINK_SO to the GBA's SI; SD (both paths, if
 * fitted) carries the game's soft-reset pulse.
 *
 * GBA_MASTER: a task on core 1
 * ----------------------------
 * The GBA clocks at up to 2 MHz, 250 ns a half period, so a word is shifted
 * edge by edge from a polling loop with interrupts off on core 1. That cannot
 * be the whole time -- the tick, the flash cache's IPI and the watchdogs need
 * that core now and then -- and librfu makes it unnecessary: in the command
 * phase the GBA starts a word only after SI has followed its SO (handshake_wait
 * 1 then 0), so while SI is held low it waits. The loop therefore holds SI low
 * ("not ready", what the PIO's `pull` stall does upstream) whenever it is not
 * watching with interrupts off, and mirrors SI = NOT SO only while it is:
 *
 *   - nothing staged: SI low, the task waits for stage() with interrupts on;
 *   - staged: interrupts off, mirror, wait for SC to fall, shift 32 bits,
 *     then SI low, interrupts on, gbl_rfu_word_done() in task context (it
 *     takes the shim lock and stages the next answer);
 *   - staged but the GBA quiet: the watch is let go every WATCH_SLICE_US for
 *     a moment, and after IDLE_SLEEP_US the task sleeps a tick, SI low.
 *
 * The ID exchange (Sio32ID) has no handshake: the GBA clocks ~40 us after the
 * previous word whatever SI says, so a word that falls in a let-go moment is
 * lost; checkID retries, and the slices are short.
 *
 * ADAPTER_MASTER: on the caller
 * -----------------------------
 * exchange() bit-bangs 32 bits at upstream's ~154 kHz with interrupts off on
 * the calling core (~210 us). The GBA-as-slave is edge-driven and indifferent
 * to the rate.
 *
 * SD: an edge interrupt
 * ---------------------
 * Rising edges are timestamped and a pulse counts when SD has stayed high
 * RFU_SD_HOLD_US, measured from the edges rather than caught at a poll (the
 * section polls about once a millisecond against a 1.1 ms pulse). The GPIO ISR
 * service lives on core 1 (gba_link_esp32s3.c), so an edge can be seen late by
 * up to a watch slice; a glitch that comes and goes inside one never reads as
 * a rise.
 *
 * NOT YET RUN ON HARDWARE. The protocol above it is tested against pret's
 * librfu in the host simulator (test/gblink_sim/test_rfu.py); the bit loop has
 * not been on a scope.
 */
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* ---- pins ----------------------------------------------------------------- */

static inline bool IRAM_ATTR pin_get(int pin) { return (GPIO.in >> pin) & 1u; }

static inline void IRAM_ATTR pin_set(int pin, bool high) {
    if (high) GPIO.out_w1ts = 1u << pin;
    else      GPIO.out_w1tc = 1u << pin;
}

static inline void IRAM_ATTR pin_out_enable(int pin, bool on) {
    if (on) GPIO.enable_w1ts = 1u << pin;
    else    GPIO.enable_w1tc = 1u << pin;
}

/* ---- state -------------------------------------------------------------- */

static TaskHandle_t       s_task;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t           s_cyc_per_us;

static volatile bool      s_enabled;
static volatile int       s_role = RFU_ROLE_GBA_MASTER;
static volatile bool      s_staged_valid;
static volatile uint32_t  s_staged;
static volatile bool      s_watching;     /* core 1 is in the bit loop    */
static volatile bool      s_park;         /* asked to leave it (set_role) */

/* SD reset pulse */
static const int          k_sd_pins[2] = { PIN_LINK_SD, BOARD_HAS_SD_ALT ? PIN_LINK_SD_ALT : -1 };
static volatile int64_t   s_sd_rise_us[2];
static volatile bool      s_sd_high[2], s_sd_reported[2];
static atomic_int         s_sd_pending = -1;   /* index of a qualified pulse */
static volatile int       s_sd_last = -1;

/* counters for the diagnostics screen */
static volatile uint32_t  s_words, s_aborted;

extern void gba_link_yield(bool yield);

/* ---- GBA_MASTER: the bit loop -------------------------------------------------- */

static inline uint32_t IRAM_ATTR cyc(void) { return esp_cpu_get_cycle_count(); }

/* Wait for SC to reach level, mirroring SI = NOT SO meanwhile if mirror.
 * False on timeout (cycles from start). */
static inline bool IRAM_ATTR wait_sc(bool level, uint32_t t0, uint32_t limit, bool mirror) {
    for (;;) {
        const uint32_t in = GPIO.in;
        if (((in >> PIN_SC) & 1u) == level) return true;
        if (mirror) pin_set(PIN_TX, !((in >> PIN_RX) & 1u));
        if (cyc() - t0 > limit) return false;
    }
}

/* One watch: interrupts off, mirror until SC falls (or the slice ends), then
 * the word. Returns 1 with *rx for a word, 0 for a quiet slice, -1 for a word
 * the GBA abandoned. */
static int IRAM_ATTR watch(uint32_t tx, uint32_t *rx) {
    int result = 0;
    portENTER_CRITICAL(&s_mux);
    s_watching = true;
    const uint32_t t0 = cyc();
    if (!s_park && wait_sc(false, t0, WATCH_SLICE_US * s_cyc_per_us, true)) {
        uint32_t v = 0;
        result = 1;
        for (int bit = 31; bit >= 0; bit--) {
            /* SC is low: drive this bit, then sample on the rising edge. */
            pin_set(PIN_TX, (tx >> bit) & 1u);
            if (!wait_sc(true, cyc(), BIT_TIMEOUT_US * s_cyc_per_us, false)) { result = -1; break; }
            v = (v << 1) | (uint32_t)pin_get(PIN_RX);
            if (bit && !wait_sc(false, cyc(), BIT_TIMEOUT_US * s_cyc_per_us, false)) { result = -1; break; }
        }
        /* Hold the last bit past the GBA's sample edge (upstream: two
         * 32-cycle nops), then "not ready" until the next is staged. */
        esp_rom_delay_us(1);
        *rx = v;
    }
    pin_set(PIN_TX, false);
    s_watching = false;
    portEXIT_CRITICAL(&s_mux);
    return result;
}

static void rfu_task(void *arg) {
    (void)arg;
    uint32_t quiet_since = cyc();
    for (;;) {
        if (!s_enabled || s_role != RFU_ROLE_GBA_MASTER || !s_staged_valid || s_park) {
            pin_set(PIN_TX, false);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
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
        } else if (cyc() - quiet_since > IDLE_SLEEP_US * s_cyc_per_us) {
            vTaskDelay(1);                 /* SI is low: the GBA waits */
            quiet_since = cyc();
        }
    }
}

/* ---- SD ------------------------------------------------------------------------- */

static void IRAM_ATTR sd_isr(void *arg) {
    const int i = (int)(intptr_t)arg;
    const bool high = pin_get(k_sd_pins[i]);
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

/* ---- the driver ------------------------------------------------------------------ */

static void drv_enable(void) {
    gba_link_yield(true);
    gpio_intr_disable(PIN_SC);
    gpio_reset_pin(PIN_SC);
    gpio_set_direction(PIN_SC, GPIO_MODE_INPUT_OUTPUT);   /* output stays disabled */
    pin_out_enable(PIN_SC, false);
    pin_set(PIN_SC, true);
    gpio_set_pull_mode(PIN_SC, GPIO_PULLUP_ONLY);
    gpio_reset_pin(PIN_RX);
    gpio_set_direction(PIN_RX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_RX, GPIO_PULLUP_ONLY);
    gpio_reset_pin(PIN_TX);
    gpio_set_direction(PIN_TX, GPIO_MODE_INPUT_OUTPUT);
    pin_set(PIN_TX, false);
    sd_arm(true);
    s_role = RFU_ROLE_GBA_MASTER;
    s_staged_valid = false;
    s_park = false;
    s_enabled = true;
    xTaskNotifyGive(s_task);
}

static void drv_disable(void) {
    s_enabled = false;
    s_park = true;
    while (s_watching) { }
    sd_arm(false);
    pin_out_enable(PIN_SC, false);
    gpio_reset_pin(PIN_TX);
    s_park = false;
    gba_link_yield(false);
}

static void drv_set_role(enum RfuRole role) {
    /* Get core 1 out of the bit loop first: it mirrors SI and watches SC. */
    s_park = true;
    s_staged_valid = false;
    while (s_watching) { }
    if (role == RFU_ROLE_ADAPTER_MASTER) {
        /* SC latch high before the driver comes on: no falling edge for the
         * armed GBA to count. SI low, the inverted-ACK lead-in. */
        pin_set(PIN_SC, true);
        pin_out_enable(PIN_SC, true);
        pin_set(PIN_TX, false);
    } else {
        /* Release at the same level: the latch is high from the last bit. */
        pin_set(PIN_SC, true);
        pin_out_enable(PIN_SC, false);
        pin_set(PIN_TX, false);
    }
    s_role = role;
    s_park = false;
    xTaskNotifyGive(s_task);
}

static void drv_stage(uint32_t word) {
    s_staged = word;
    s_staged_valid = true;
    /* From the core-1 task's own callback this is just a flag; from another
     * core, wake it. */
    if (xTaskGetCurrentTaskHandle() != s_task) xTaskNotifyGive(s_task);
}

static void IRAM_ATTR spin_ns(uint32_t ns) {
    const uint32_t t0 = cyc(), n = ns * s_cyc_per_us / 1000u;
    while (cyc() - t0 < n) { }
}

static bool drv_exchange(uint32_t word, uint32_t *rx, uint32_t timeout_us) {
    (void)timeout_us;   /* no pin waits: it cannot hang */
    if (s_role != RFU_ROLE_ADAPTER_MASTER) return false;
    uint32_t v = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    for (int bit = 31; bit >= 0; bit--) {
        /* Falling edge with our bit; the GBA shifts SO out on it. */
        pin_set(PIN_SC, false);
        pin_set(PIN_TX, (word >> bit) & 1u);
        spin_ns(MASTER_HALF_NS / 2);
        v = (v << 1) | (uint32_t)pin_get(PIN_RX);   /* mid-low, as upstream */
        spin_ns(MASTER_HALF_NS / 2);
        pin_set(PIN_SC, true);                      /* rising: it latches SI */
        spin_ns(MASTER_HALF_NS);
    }
    portEXIT_CRITICAL(&mux);
    *rx = v;
    return true;
}

static void drv_drive_si(bool high) { pin_set(PIN_TX, high); }

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
    ESP_LOGI(TAG, "Wireless Adapter PHY on core 1: SC=%d RX=%d TX=%d SD=%d/%d",
             PIN_SC, PIN_RX, PIN_TX, k_sd_pins[0], k_sd_pins[1]);
}

void rfu_link_counters(uint32_t *words, uint32_t *aborted) {
    if (words)   *words = s_words;
    if (aborted) *aborted = s_aborted;
}
