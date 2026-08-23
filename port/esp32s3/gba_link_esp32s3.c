/*
 * gba_link_esp32s3.c -- GBA Multi-Play PHY for ESP32-S3, both roles.
 *
 * Framing is now spec-derived rather than guessed. GBATEK, SIO Multi-Player
 * Mode, "Transfer Protocol":
 *
 *   - The master's SI pin is always LOW.
 *   - When all GBAs are in multiplayer mode (ready), SD is HIGH.
 *   - When the master starts a transfer it sets SC=LOW; slaves see the Busy bit.
 *   - Step A: the master outputs a Startbit (LOW), 16 data bits, and a Stopbit
 *     (HIGH) through SD. This lands in SIOMULTI0 of every GBA. The master then
 *     forwards LOW from its SO to the first child's SI.
 *   - Step B: the first child does the same into SIOMULTI1, then forwards LOW
 *     from its SO to the second child's SI. And so on.
 *   - The transfer ends if the next child does not output data within a
 *     certain time.
 *
 * And from SIO Normal Mode, which shares the bit clocking convention:
 * data is driven when the master takes SC LOW, and sampled when SC goes HIGH.
 *
 * So a word is 18 bits on the wire, not 16, and a missing player is not an
 * error -- the transfer just ends early and that slot reads 0xFFFF.
 *
 * Roles
 * -----
 * SLAVE  is what a Battle Chip Gate is. The local GBA sets the pace and we
 *        must have our word staged before it starts. Fine for a device that
 *        generates its own answers; useless for relaying a remote player.
 *
 * MASTER is the mode that makes networking work. We own SC, so we choose when
 *        each transfer begins, and GBATEK explicitly says the parent may need
 *        to insert delays between transfers. Remote latency turns into a pause
 *        rather than a missed slot. Wire the adapter into the parent connector
 *        position; the local GBA then comes up as a child on its own.
 */

#include <string.h>

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link_proto.h"
#include "link_session.h"

static const char *TAG = "gba_link";

/* ---- wiring ----------------------------------------------------------
 *
 * Link port, looking into the socket:
 *   1 VCC (3.3 V, milliamps only -- never run the radio from it)
 *   2 SO   -> next device's SI
 *   3 SI   <- previous device's SO
 *   4 SD   shared data bus, open drain, pull-up required
 *   5 SC   shared clock
 *   6 GND
 */
#define PIN_SC 4
#define PIN_SD 5
#define PIN_SI 6
#define PIN_SO 7

/* Multi-Play supports 9600 / 38400 / 57600 / 115200. As master we pick. */
#define LINK_BAUD 115200u

#define WORD_BITS  16
#define FRAME_BITS 18 /* start + 16 + stop */

/* A word is 18 * 8.68 us = 156 us; four of them plus turnaround is under 1 ms.
 * A game that transfers once per VBlank therefore leaves ~15.7 ms of slack,
 * which is the budget the network has to fit inside. */

/* If a child does not start driving within this, the transfer ends. */
#define CHILD_TIMEOUT_US 100

/* ---- dedicated GPIO --------------------------------------------------
 * Single-cycle GPIO that skips the GPIO matrix. This is what makes a spin
 * loop viable at 8.68 us/bit without PIO. */

enum { IN_SC = 0, IN_SD = 1, IN_SI = 2 };
enum { OUT_SC = 0, OUT_SD = 1, OUT_SO = 2 };

static dedic_gpio_bundle_handle_t s_in;
static dedic_gpio_bundle_handle_t s_out;
static uint32_t s_out_shadow;

static gba_role_t     s_role;
static link_session_t s_session;
static uint32_t       s_cyc_per_half_bit;

/* Supplied by the transport layer (USB CDC / ESP-NOW). */
extern void gba_link_report(const gba_transfer_t *xfer, uint16_t seq, bool underran);
extern void gba_link_pump(void); /* service the transport; must not block */

/* ---- primitives ------------------------------------------------------ */

static inline uint32_t IRAM_ATTR rd(void) {
    return dedic_gpio_bundle_read_in(s_in);
}

static inline void IRAM_ATTR wr(uint32_t mask, bool high) {
    s_out_shadow = high ? (s_out_shadow | mask) : (s_out_shadow & ~mask);
    dedic_gpio_bundle_write(s_out, mask, high ? mask : 0u);
}

static inline void IRAM_ATTR sc(bool high) { wr(1u << OUT_SC, high); }
static inline void IRAM_ATTR so(bool high) { wr(1u << OUT_SO, high); }

/* SD is shared by every device on the bus, so it is open drain: drive low, or
 * release and let the pull-up do the rest. Never drive it high. */
static inline void IRAM_ATTR sd(bool high) { wr(1u << OUT_SD, high); }

static inline bool IRAM_ATTR sd_in(void) { return (rd() >> IN_SD) & 1u; }
static inline bool IRAM_ATTR sc_in(void) { return (rd() >> IN_SC) & 1u; }
static inline bool IRAM_ATTR si_in(void) { return (rd() >> IN_SI) & 1u; }

static inline void IRAM_ATTR spin(uint32_t cycles) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (esp_cpu_get_cycle_count() - t0 < cycles) {
        /* nothing */
    }
}

static inline bool IRAM_ATTR wait_for(int bit, bool want, uint32_t cycles) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (((rd() >> bit) & 1u) != (uint32_t)want) {
        if (esp_cpu_get_cycle_count() - t0 > cycles) {
            return false;
        }
    }
    return true;
}

static uint32_t us_to_cyc(uint32_t us) {
    return (uint32_t)((uint64_t)us * (uint64_t)esp_clk_cpu_freq() / 1000000ULL);
}

/* ---- master role ----------------------------------------------------- */

/* Clock out one 18-bit frame: start bit, 16 data bits MSB first, stop bit.
 * Data changes while SC is low; everyone samples on the rising edge. */
static void IRAM_ATTR m_send_frame(uint16_t w) {
    for (int i = FRAME_BITS - 1; i >= 0; i--) {
        bool bit;
        if (i == FRAME_BITS - 1) {
            bit = false;               /* start */
        } else if (i == 0) {
            bit = true;                /* stop  */
        } else {
            bit = (w >> (i - 1)) & 1u; /* data, MSB first */
        }
        sc(false);
        sd(bit);
        spin(s_cyc_per_half_bit);
        sc(true);
        spin(s_cyc_per_half_bit);
    }
    sd(true); /* release the bus */
}

/*
 * Clock in one frame from whichever child currently holds the bus.
 * Returns false if the child never asserted a start bit, which per GBATEK
 * ends the transfer -- that is how the master learns the player count.
 */
static bool IRAM_ATTR m_recv_frame(uint16_t *out) {
    /* Look for the start bit. The child drives SD low when SC is low. */
    uint32_t deadline = us_to_cyc(CHILD_TIMEOUT_US);
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (;;) {
        sc(false);
        spin(s_cyc_per_half_bit);
        bool start_low = !sd_in();
        sc(true);
        spin(s_cyc_per_half_bit);
        if (start_low) {
            break;
        }
        if (esp_cpu_get_cycle_count() - t0 > deadline) {
            return false; /* nobody home; transfer ends here */
        }
    }

    uint16_t w = 0;
    for (int i = 0; i < WORD_BITS; i++) {
        sc(false);
        spin(s_cyc_per_half_bit);
        sc(true);
        w = (uint16_t)((w << 1) | (sd_in() ? 1u : 0u));
        spin(s_cyc_per_half_bit);
    }

    /* Stop bit. */
    sc(false);
    spin(s_cyc_per_half_bit);
    sc(true);
    spin(s_cyc_per_half_bit);

    *out = w;
    return true;
}

/*
 * Run one full transfer as parent. our_word goes in slot 0; children fill the
 * rest. Never called until the remote word is in hand (or the stall budget
 * expired), which is the entire point of being master.
 */
static bool IRAM_ATTR m_transfer(uint16_t our_word, gba_transfer_t *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++) {
        out->word[i] = GBA_WORD_EMPTY;
    }
    out->self_id = 0;

    /* SD high means everyone is in multiplayer mode and idle. */
    if (!sd_in()) {
        return false;
    }

    so(true); /* hold the token until our word is out */
    m_send_frame(our_word);
    out->word[0] = our_word;
    out->players = 1;

    so(false); /* hand the bus to child 1 */

    for (int p = 1; p < 4; p++) {
        uint16_t w;
        if (!m_recv_frame(&w)) {
            break;
        }
        out->word[p] = w;
        out->players = (uint8_t)(p + 1);
    }

    so(true);
    sc(true);
    return true;
}

/* ---- slave role ------------------------------------------------------
 * Kept because device mode (Battle Chip Gate, e-Reader) needs it. Here the
 * local GBA is parent and we have no say in the timing. */

static bool IRAM_ATTR s_transfer(uint16_t our_word, gba_transfer_t *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++) {
        out->word[i] = GBA_WORD_EMPTY;
    }

    uint32_t frame_cyc = us_to_cyc(400); /* generous per-edge timeout */

    /* The master pulling SC low is the busy signal that opens a transfer. */
    if (!wait_for(IN_SC, false, us_to_cyc(20000))) {
        return false;
    }

    /* Listen to the master's frame: start bit, 16 bits, stop bit. */
    uint16_t master = 0;
    for (int i = 0; i < FRAME_BITS; i++) {
        if (!wait_for(IN_SC, false, frame_cyc)) return false;
        if (!wait_for(IN_SC, true,  frame_cyc)) return false;
        if (i > 0 && i < FRAME_BITS - 1) {
            master = (uint16_t)((master << 1) | (sd_in() ? 1u : 0u));
        }
    }
    out->word[0] = master;
    out->players = 1;

    /* Our SI going low is the token arriving. */
    if (!wait_for(IN_SI, false, frame_cyc)) {
        return false;
    }

    /* Drive our frame. The master is still generating SC. */
    for (int i = FRAME_BITS - 1; i >= 0; i--) {
        bool bit = (i == FRAME_BITS - 1) ? false
                 : (i == 0)              ? true
                 : ((our_word >> (i - 1)) & 1u);
        if (!wait_for(IN_SC, false, frame_cyc)) { sd(true); so(true); return false; }
        sd(bit);
        if (!wait_for(IN_SC, true, frame_cyc))  { sd(true); so(true); return false; }
    }
    sd(true);
    so(false); /* pass the token downstream */

    /* self_id is the slot we drove. With one adapter on the bus that is 1. */
    out->self_id = 1;
    out->word[1] = our_word;
    out->players = 2;

    so(true);
    return true;
}

/* ---- the link task --------------------------------------------------- */

static void IRAM_ATTR gba_link_task(void *arg) {
    (void)arg;
    const uint32_t stall_cyc = us_to_cyc(s_session.stall_budget_us);

    for (;;) {
        gba_transfer_t xfer;
        bool underran = false;
        uint16_t seq = s_session.seq;

        if (s_role == GBA_ROLE_MASTER) {
            /*
             * The stall that makes this whole thing work. GBATEK sanctions it:
             * the parent may insert delays between transfers. We spend the
             * peer's latency here, where it is harmless, instead of missing a
             * slot, where it is fatal.
             */
            uint32_t t0 = esp_cpu_get_cycle_count();
            uint32_t waited_us = 0;
            while (!ls_ready(&s_session)) {
                gba_link_pump();
                if (esp_cpu_get_cycle_count() - t0 > stall_cyc) {
                    break;
                }
            }
            if (esp_cpu_get_cycle_count() - t0 > us_to_cyc(1)) {
                waited_us = (uint32_t)((esp_cpu_get_cycle_count() - t0) /
                                       (uint32_t)(esp_clk_cpu_freq() / 1000000));
            }

            uint16_t w = ls_take_word(&s_session, waited_us, &underran);
            if (m_transfer(w, &xfer)) {
                ls_complete(&s_session);
                gba_link_report(&xfer, seq, underran);
            } else {
                vTaskDelay(1); /* bus not ready; back off so the WDT is fed */
            }
        } else {
            uint16_t w = ls_ready(&s_session)
                             ? ls_take_word(&s_session, 0, &underran)
                             : GBA_WORD_EMPTY;
            if (s_transfer(w, &xfer)) {
                ls_complete(&s_session);
                gba_link_report(&xfer, seq, underran);
            } else {
                vTaskDelay(1);
            }
        }
    }
}

/* ---- setup ----------------------------------------------------------- */

void gba_link_start(gba_role_t role, uint32_t stall_budget_us) {
    s_role = role;
    ls_init(&s_session, stall_budget_us);

    /* Half a bit period, in CPU cycles. */
    s_cyc_per_half_bit =
        (uint32_t)((uint64_t)esp_clk_cpu_freq() / (uint64_t)LINK_BAUD / 2ULL);

    gpio_config_t shared = {
        .pin_bit_mask = (1ULL << PIN_SD),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&shared));

    /* SC is an output when we are parent, an input when we are child. It is
     * open drain either way so that plugging into the wrong connector cannot
     * put two drivers in contention. */
    gpio_config_t clk = {
        .pin_bit_mask = (1ULL << PIN_SC),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&clk));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_SI),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    int in_pins[]  = { PIN_SC, PIN_SD, PIN_SI };
    int out_pins[] = { PIN_SC, PIN_SD, PIN_SO };

    dedic_gpio_bundle_config_t in_cfg = {
        .gpio_array = in_pins,
        .array_size = 3,
        .flags = { .in_en = 1 },
    };
    dedic_gpio_bundle_config_t out_cfg = {
        .gpio_array = out_pins,
        .array_size = 3,
        .flags = { .out_en = 1 },
    };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&in_cfg, &s_in));
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&out_cfg, &s_out));

    sc(true);
    sd(true);
    so(true);

    /*
     * Core 1, top priority, everything IRAM_ATTR. WiFi, BLE, USB and the
     * display stay on core 0. The IRAM part is not optional: any flash write
     * anywhere in the system suspends the instruction cache, and a stall in
     * the middle of clocking 18 bits corrupts the frame.
     */
    xTaskCreatePinnedToCore(gba_link_task, "gba_link", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);

    ESP_LOGI(TAG, "link up as %s, %u baud, stall budget %u us",
             role == GBA_ROLE_MASTER ? "parent" : "child",
             (unsigned)LINK_BAUD, (unsigned)s_session.stall_budget_us);
}

/* Called by the transport when a peer word arrives. */
bool gba_link_feed(uint16_t seq, uint16_t word) {
    return ls_push_word(&s_session, seq, word);
}

const link_session_t *gba_link_stats(void) {
    return &s_session;
}
