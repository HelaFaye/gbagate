/*
 * cg_phy_esp32s3.c -- GBA multiplayer-slave PHY for ESP32-S3, ESP-IDF v5.x.
 *
 * Runs the bit-level half of the link on core 1 so that WiFi, BLE, USB and the
 * ST7735 can have core 0 to themselves. Hands completed 16-bit frames to the
 * portable responder in chipgate.c.
 *
 * READ THIS BEFORE TRUSTING IT
 * ----------------------------
 * The protocol layer (chipgate.c) is verified against two independent
 * implementations. This file is not. The GBA multiplayer bus is a shared
 * open-drain data line with a token passed along the SI/SO daisy chain, and
 * the exact edge on which each device takes the bus is the one thing worth
 * confirming with a logic analyser before you cut a PCB. Every place that
 * matters is marked SCOPE.
 *
 * Link port pinout, looking into the socket on the GBA:
 *
 *     1 VCC (3.3 V, a few mA -- do NOT try to run WiFi off it)
 *     2 SO   out from this device -> next device's SI
 *     3 SI   in  from previous device's SO
 *     4 SD   shared data bus, open drain, needs a pull-up
 *     5 SC   shared clock, driven by the master (player 1)
 *     6 GND
 *
 * The gate must be player 2 (the grey wide connector) for single player, and
 * players 2 and 3 for a multiplayer session.
 *
 * All logic is 3.3 V, so the S3 connects directly. Power the board separately
 * and tie grounds together.
 */

#include <string.h>

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "chipgate.h"

static const char *TAG = "cg_phy";

/* ---- wiring ---------------------------------------------------------- */

#define CG_PIN_SC 4 /* clock in  */
#define CG_PIN_SD 5 /* data, bidirectional */
#define CG_PIN_SI 6 /* turn-token in  */
#define CG_PIN_SO 7 /* turn-token out */

/* Multiplayer mode at 115200 bps: 8.68 us per bit. At 240 MHz that is roughly
 * 2080 CPU cycles, which is an eternity for a spin loop -- the reason this
 * works without PIO. */
#define CG_BIT_US        8.68f
#define CG_FRAME_BITS    16

/* If the clock stops for this long mid-frame, the master went away. */
#define CG_TIMEOUT_US    2000

/* ---- dedicated GPIO bundles ------------------------------------------ */
/*
 * The dedicated GPIO peripheral gives single-cycle reads and writes that skip
 * the GPIO matrix, which is what makes a spin loop viable here. Bundle order
 * defines the bit positions.
 */

enum { CG_IN_SC = 0, CG_IN_SD = 1, CG_IN_SI = 2 };
enum { CG_OUT_SD = 0, CG_OUT_SO = 1 };

static dedic_gpio_bundle_handle_t s_in;
static dedic_gpio_bundle_handle_t s_out;

static chipgate_t s_gate;
static volatile uint16_t s_staged = CG_WORD_CONTINUE; /* next word out */
static volatile uint16_t s_last_rx;
static volatile bool     s_link_up;

/* Set by the UI task, latched by the link task at a frame boundary. */
static volatile uint16_t s_pending_chip;
static volatile bool     s_pending_valid;

/* ---- helpers --------------------------------------------------------- */

static inline uint32_t IRAM_ATTR cg_in(void) {
    return dedic_gpio_bundle_read_in(s_in);
}

static inline void IRAM_ATTR cg_sd(bool high) {
    /* SD is open drain across the bus: drive low, or release and let the
     * pull-up bring it high. Never drive it high actively. */
    dedic_gpio_bundle_write(s_out, 1u << CG_OUT_SD, high ? (1u << CG_OUT_SD) : 0u);
}

static inline void IRAM_ATTR cg_so(bool high) {
    dedic_gpio_bundle_write(s_out, 1u << CG_OUT_SO, high ? (1u << CG_OUT_SO) : 0u);
}

static inline bool IRAM_ATTR cg_wait_level(int bit, bool want, uint32_t timeout_cyc) {
    uint32_t start = esp_cpu_get_cycle_count();
    while (((cg_in() >> bit) & 1u) != (uint32_t)want) {
        if (esp_cpu_get_cycle_count() - start > timeout_cyc) {
            return false;
        }
    }
    return true;
}

/* ---- one multiplayer frame ------------------------------------------- */
/*
 * A transfer is four 16-bit words clocked back to back: the master's, then
 * each slave's in daisy-chain order. We shift in every bit that goes past so
 * we can read the master's word, and we drive SD only during our own slot.
 *
 * SCOPE: which SC edge the bus samples on, and whether our slot begins on the
 * falling edge of SI or one clock later. Capture a real gate if you have one;
 * otherwise capture a second GBA acting as player 2.
 */
static bool IRAM_ATTR cg_run_frame(uint32_t timeout_cyc) {
    uint16_t master = 0;
    uint16_t mine   = s_staged;

    /* Our turn begins when the upstream device drops SI. */
    if (!cg_wait_level(CG_IN_SI, false, timeout_cyc)) {
        return false;
    }

    /* --- word 0: the master's, we only listen --- */
    for (int i = 0; i < CG_FRAME_BITS; i++) {
        if (!cg_wait_level(CG_IN_SC, false, timeout_cyc)) return false;
        if (!cg_wait_level(CG_IN_SC, true,  timeout_cyc)) return false; /* SCOPE */
        master = (uint16_t)((master << 1) | ((cg_in() >> CG_IN_SD) & 1u));
    }

    /* --- our word: drive SD, and hold SO low to keep the token --- */
    cg_so(false);
    for (int i = CG_FRAME_BITS - 1; i >= 0; i--) {
        if (!cg_wait_level(CG_IN_SC, false, timeout_cyc)) { cg_sd(true); cg_so(true); return false; }
        cg_sd((mine >> i) & 1u);
        if (!cg_wait_level(CG_IN_SC, true, timeout_cyc))  { cg_sd(true); cg_so(true); return false; }
    }
    cg_sd(true);  /* release the bus */
    cg_so(true);  /* pass the token downstream */

    s_last_rx = master;

    /* Compute the next response now: we have a whole frame of slack before
     * the master clocks us again, but zero slack once it starts. */
    if (s_pending_valid) {
        if (chipgate_load(&s_gate, s_pending_chip)) {
            s_pending_valid = false;
        }
    }
    s_staged  = chipgate_step(&s_gate, master);
    s_link_up = chipgate_connected(&s_gate);
    return true;
}

/* ---- link task ------------------------------------------------------- */

static void IRAM_ATTR cg_link_task(void *arg) {
    (void)arg;
    const uint32_t timeout_cyc =
        (uint32_t)((uint64_t)CG_TIMEOUT_US * esp_clk_cpu_freq() / 1000000ULL);

    for (;;) {
        if (!cg_run_frame(timeout_cyc)) {
            /* Master idle or cable unplugged. Drop the session so the next
             * title screen probe sees a clean gate, then yield briefly so the
             * watchdog and lower-priority work on this core still run. */
            chipgate_reset_link(&s_gate);
            s_staged  = s_gate.gate_id;
            s_link_up = false;
            vTaskDelay(1);
        }
    }
}

/* ---- public surface -------------------------------------------------- */

void cg_phy_start(cg_game_t game) {
    chipgate_init(&s_gate, game);
    s_staged = s_gate.gate_id;

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << CG_PIN_SC) | (1ULL << CG_PIN_SI),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* SD is read and written, and must be open drain because every device on
     * the bus shares it. SO is a normal push-pull output. */
    gpio_config_t sd_cfg = {
        .pin_bit_mask = (1ULL << CG_PIN_SD),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_cfg));

    gpio_config_t so_cfg = {
        .pin_bit_mask = (1ULL << CG_PIN_SO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&so_cfg));
    gpio_set_level(CG_PIN_SO, 1);

    int in_pins[]  = { CG_PIN_SC, CG_PIN_SD, CG_PIN_SI };
    int out_pins[] = { CG_PIN_SD, CG_PIN_SO };

    dedic_gpio_bundle_config_t in_bundle = {
        .gpio_array = in_pins,
        .array_size = sizeof in_pins / sizeof in_pins[0],
        .flags = { .in_en = 1 },
    };
    dedic_gpio_bundle_config_t out_bundle = {
        .gpio_array = out_pins,
        .array_size = sizeof out_pins / sizeof out_pins[0],
        .flags = { .out_en = 1 },
    };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&in_bundle, &s_in));
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&out_bundle, &s_out));

    /*
     * Pin to core 1 and give it the top priority there. Core 0 keeps WiFi,
     * BLE, USB and the display.
     *
     * The task and everything it calls are IRAM_ATTR because a flash write
     * anywhere in the system -- an NVS commit when the user saves a folder, an
     * OTA chunk -- suspends the instruction cache. Code running from flash
     * would stall for milliseconds and the game would see a dead gate. Build
     * chipgate.c into IRAM too, or mark chipgate_step with IRAM_ATTR.
     */
    xTaskCreatePinnedToCore(cg_link_task, "cg_link", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);

    ESP_LOGI(TAG, "gate up: %s, id %04X",
             chipgate_game_name(game), s_gate.gate_id);
}

/* Called from the UI / reader / network tasks on core 0. */
bool cg_phy_slot_chip(uint16_t chip_id) {
    if (s_pending_valid || chipgate_busy(&s_gate)) {
        return false;
    }
    s_pending_chip  = chip_id;
    s_pending_valid = true;
    return true;
}

bool cg_phy_linked(void)    { return s_link_up; }
uint16_t cg_phy_last_rx(void) { return s_last_rx; }
