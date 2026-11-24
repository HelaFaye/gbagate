/*
 * main.c -- the RMT GBA link test: an ESP32-S3 SuperMini, two jumper wires,
 * and (for timing) a logic analyzer. No GBA. README.md says how to run it.
 *
 * RMT sends the GBA link's waveforms on two pins, jumpers bring them back
 * into two more, and RMT receives them; the firmware decodes what came back
 * and prints PASS or FAIL for each part over the USB port:
 *
 *   A  Multi-Player frames (SD: start, 16 bits, stop; 115200), RX idle
 *      threshold 200 us
 *   B  the same at 60 us, the guide's signal_range_max_ns: the words the
 *      host model predicts fail must fail (cut short, or read wrong), and
 *      the others pass
 *   C  normal mode, SC and SO sent together by a sync manager, 256 kHz and
 *      2 MHz: SO read at SC's rising edges
 *   D  channels made, used and deleted 200 times, with a receive left
 *      waiting at each delete (a mode switch's teardown): no error, no leak
 *
 * Then it repeats one burst a second of A's and C's waveforms, GPIO1 high
 * while it lasts, for the logic analyzer (tools/la_check.py checks the
 * capture against the nominal timing: the loopback cannot, sharing the
 * ESP32's own clock).
 *
 * Pins (SuperMini): GPIO4 SC out, GPIO5 SO/SD out, GPIO6 SO/SD in,
 * GPIO7 SC in, GPIO1 marker. Jumpers: GPIO5 to GPIO6, GPIO4 to GPIO7.
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "rl_codec.h"
#include "rl_patterns.h"

#define PIN_SC_OUT  4
#define PIN_SO_OUT  5
#define PIN_SO_IN   6
#define PIN_SC_IN   7
#define PIN_MARK    1

#define RX_SYMBOLS  48      /* one RMT memory block: an S3 RX channel's own */
#define IDLE_GOOD   200000  /* ns: past 0x0000's 147.6 us low run and 256 kHz SO's 125 us */
#define IDLE_GUIDE  60000   /* ns: the guide's signal_range_max_ns */

typedef struct { rmt_channel_handle_t ch; QueueHandle_t q; rmt_symbol_word_t buf[RX_SYMBOLS]; } rx_t;

static rmt_channel_handle_t s_sc, s_so;
static rmt_encoder_handle_t s_copy;
static rmt_sync_manager_handle_t s_sync;
static rx_t s_rx_so, s_rx_sc;

static bool on_rx(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *ctx) {
    (void)ch;
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)ctx, e, &woke);
    return woke == pdTRUE;
}

static esp_err_t tx_make(int pin, rmt_channel_handle_t *ch) {
    rmt_tx_channel_config_t c = {
        .gpio_num = pin, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RL_RES_HZ,
        .mem_block_symbols = 48, .trans_queue_depth = 4,
    };
    esp_err_t e = rmt_new_tx_channel(&c, ch);
    if (e == ESP_OK) e = rmt_enable(*ch);
    return e;
}

static esp_err_t rx_make(int pin, rx_t *rx) {
    rmt_rx_channel_config_t c = {
        .gpio_num = pin, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RL_RES_HZ,
        .mem_block_symbols = RX_SYMBOLS,
    };
    esp_err_t e = rmt_new_rx_channel(&c, &rx->ch);
    if (e != ESP_OK) return e;
    if (!rx->q) rx->q = xQueueCreate(2, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_event_callbacks_t cb = { .on_recv_done = on_rx };
    e = rmt_rx_register_event_callbacks(rx->ch, &cb, rx->q);
    if (e == ESP_OK) e = rmt_enable(rx->ch);
    return e;
}

static esp_err_t rx_start(rx_t *rx, uint32_t idle_ns) {
    xQueueReset(rx->q);
    rmt_receive_config_t c = { .signal_range_min_ns = 100, .signal_range_max_ns = idle_ns };
    return rmt_receive(rx->ch, rx->buf, sizeof rx->buf, &c);
}

/* The runs received, or -1 if nothing came within ms. */
static int rx_wait(rx_t *rx, rl_run_t *runs, int max, int ms) {
    rmt_rx_done_event_data_t e;
    if (xQueueReceive(rx->q, &e, pdMS_TO_TICKS(ms)) != pdTRUE) {
        /* nothing came: the receive is still pending, and the channel takes
         * no new one until it is stopped */
        rmt_disable(rx->ch);
        rmt_enable(rx->ch);
        return -1;
    }
    return rl_sym_to_runs((const uint32_t *)e.received_symbols, (int)e.num_symbols, runs, max);
}

static esp_err_t send(rmt_channel_handle_t ch, const uint32_t *sym, int n) {
    rmt_transmit_config_t c = { .loop_count = 0, .flags.eot_level = 1 };   /* idle high after */
    return rmt_transmit(ch, s_copy, sym, (size_t)n * sizeof(uint32_t), &c);
}

/* Both lines to their idle level (high): a TX pin is low until its first send. */
static void idle_high(void) {
    const uint32_t hi = rl_sym(1, 40, 1, 0);
    send(s_sc, &hi, 1);
    send(s_so, &hi, 1);
    rmt_tx_wait_all_done(s_sc, 100);
    rmt_tx_wait_all_done(s_so, 100);
}

/* ---- A and B: Multi-Player frames ------------------------------------------ */

/* One word out on SO, back on SO's input. Returns RL_OK with *got, or an error. */
static int mp_once(uint16_t word, uint32_t idle_ns, uint16_t *got) {
    uint32_t sym[32];
    const int n = rl_mp_encode(word, RL_MP_NS, sym, 32);
    rl_run_t runs[2 * RX_SYMBOLS];
    if (rx_start(&s_rx_so, idle_ns) != ESP_OK) return -10;
    vTaskDelay(1);
    if (send(s_so, sym, n) != ESP_OK) return -11;
    const int m = rx_wait(&s_rx_so, runs, 2 * RX_SYMBOLS, 50);
    rmt_tx_wait_all_done(s_so, 50);
    if (m < 0) return -12;                       /* nothing received */
    return rl_mp_decode(runs, m, RL_MP_NS, got);
}

static int test_mp(uint32_t idle_ns, int expect_guide) {
    int bad = 0;
    for (unsigned k = 0; k < RL_COUNT(RL_MP_WORDS); k++) {
        const uint16_t w = RL_MP_WORDS[k];
        uint16_t got = 0;
        const int r = mp_once(w, idle_ns, &got);
        const int ok = r == RL_OK && got == w;
        const int predicted_bad = expect_guide && rl_mp_longest_run(w) * RL_MP_NS > idle_ns;
        /* a predicted failure counts only as the threshold makes it: cut short,
         * or read as another word -- not a channel or wiring error (<= -10) */
        const int threshold_fail = !ok && r > -10;
        const int as_expected = predicted_bad ? threshold_fail : ok;
        bad += !as_expected;
        printf("  %s %04X -> %s%04X (result %d, longest run %d bits)%s\n", as_expected ? "ok  " : "FAIL", w,
               r == RL_OK ? "" : "error ", got, r, rl_mp_longest_run(w),
               predicted_bad ? (ok ? "  expected to fail, but passed" : threshold_fail ? "  fails, as predicted" : "  a channel error, not the threshold") : "");
    }
    return bad;
}

/* ---- C: normal mode, SC and SO in sync ------------------------------------- */

static int normal_once(uint32_t word, uint32_t bit_ns, uint32_t *got) {
    uint32_t sc[48], so[48];
    int nsc, nso;
    if (rl_normal_encode(word, bit_ns, sc, &nsc, so, &nso, 48)) return -20;
    if (rx_start(&s_rx_sc, IDLE_GOOD) != ESP_OK || rx_start(&s_rx_so, IDLE_GOOD) != ESP_OK) return -21;
    vTaskDelay(1);
    rmt_sync_reset(s_sync);
    if (send(s_sc, sc, nsc) != ESP_OK || send(s_so, so, nso) != ESP_OK) return -22;
    rl_run_t rsc[2 * RX_SYMBOLS], rso[2 * RX_SYMBOLS];
    const int a = rx_wait(&s_rx_sc, rsc, 2 * RX_SYMBOLS, 50), b = rx_wait(&s_rx_so, rso, 2 * RX_SYMBOLS, 50);
    rmt_tx_wait_all_done(s_sc, 50);
    rmt_tx_wait_all_done(s_so, 50);
    if (a < 0) return -23;
    if (b < 0) return -24;
    return rl_normal_decode(rsc, a, rso, b, got);
}

static int test_normal(void) {
    int bad = 0;
    for (unsigned q = 0; q < RL_COUNT(RL_NORMAL_BIT_NS); q++)
        for (unsigned k = 0; k < RL_COUNT(RL_NORMAL_WORDS); k++) {
            uint32_t got = 0;
            const int r = normal_once(RL_NORMAL_WORDS[k], RL_NORMAL_BIT_NS[q], &got);
            const int ok = r == RL_OK && got == RL_NORMAL_WORDS[k];
            bad += !ok;
            printf("  %s %s %08lX -> %08lX (result %d)\n", ok ? "ok  " : "FAIL",
                   q ? "2 MHz  " : "256 kHz", (unsigned long)RL_NORMAL_WORDS[k], (unsigned long)got, r);
        }
    return bad;
}

/* ---- D: make, use and delete, 200 times ----------------------------------- */

static int test_lifecycle(void) {
    /* the test's own channels go first: this makes and deletes its own */
    rmt_del_sync_manager(s_sync);
    rmt_disable(s_rx_so.ch); rmt_del_channel(s_rx_so.ch);
    rmt_disable(s_rx_sc.ch); rmt_del_channel(s_rx_sc.ch);
    rmt_disable(s_so); rmt_del_channel(s_so);
    rmt_disable(s_sc); rmt_del_channel(s_sc);
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int errors = 0, frames = 0;
    for (int i = 0; i < 200; i++) {
        esp_err_t e = tx_make(PIN_SO_OUT, &s_so);
        if (e == ESP_OK) e = rx_make(PIN_SO_IN, &s_rx_so);
        if (e != ESP_OK) { errors++; printf("  FAIL make, round %d: %s\n", i, esp_err_to_name(e)); break; }
        const uint32_t hi = rl_sym(1, 40, 1, 0);
        send(s_so, &hi, 1);
        rmt_tx_wait_all_done(s_so, 50);
        uint16_t got;
        if (i % 2 == 0) frames += mp_once(RL_MP_WORDS[i % RL_COUNT(RL_MP_WORDS)], IDLE_GOOD, &got) == RL_OK;
        else frames++;
        rx_start(&s_rx_so, IDLE_GOOD);           /* left waiting: torn down below, mid-receive */
        errors += rmt_disable(s_rx_so.ch) != ESP_OK;
        errors += rmt_del_channel(s_rx_so.ch) != ESP_OK;
        errors += rmt_disable(s_so) != ESP_OK;
        errors += rmt_del_channel(s_so) != ESP_OK;
    }
    const size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const long leak = (long)before - (long)after;
    printf("  %s 200 rounds: %d errors, %d of 200 frames, heap %ld bytes lower\n",
           !errors && frames == 200 && leak < 512 ? "ok  " : "FAIL", errors, frames, leak);
    return errors + (frames != 200) + (leak >= 512);
}

/* ---- the logic analyzer's burst -------------------------------------------- */

static void burst(void) {
    gpio_set_level(PIN_MARK, 1);
    for (unsigned k = 0; k < RL_COUNT(RL_MP_WORDS); k++) {
        uint32_t sym[32];
        const int n = rl_mp_encode(RL_MP_WORDS[k], RL_MP_NS, sym, 32);
        send(s_so, sym, n);
        rmt_tx_wait_all_done(s_so, 50);
        esp_rom_delay_us(300);                   /* idle between frames */
    }
    esp_rom_delay_us(1000);
    for (unsigned q = 0; q < RL_COUNT(RL_NORMAL_BIT_NS); q++) {
        for (unsigned k = 0; k < RL_COUNT(RL_NORMAL_WORDS); k++) {
            uint32_t sc[48], so[48];
            int nsc, nso;
            rl_normal_encode(RL_NORMAL_WORDS[k], RL_NORMAL_BIT_NS[q], sc, &nsc, so, &nso, 48);
            rmt_sync_reset(s_sync);
            send(s_sc, sc, nsc);
            send(s_so, so, nso);
            rmt_tx_wait_all_done(s_sc, 50);
            rmt_tx_wait_all_done(s_so, 50);
            esp_rom_delay_us(300);
        }
        esp_rom_delay_us(1000);
    }
    gpio_set_level(PIN_MARK, 0);
}

static esp_err_t setup_all(void) {
    esp_err_t e = tx_make(PIN_SC_OUT, &s_sc);
    if (e == ESP_OK) e = tx_make(PIN_SO_OUT, &s_so);
    if (e == ESP_OK) e = rx_make(PIN_SO_IN, &s_rx_so);
    if (e == ESP_OK) e = rx_make(PIN_SC_IN, &s_rx_sc);
    if (e != ESP_OK) return e;
    rmt_channel_handle_t both[] = { s_sc, s_so };
    rmt_sync_manager_config_t sc = { .tx_channel_array = both, .array_size = 2 };
    e = rmt_new_sync_manager(&sc, &s_sync);
    if (e == ESP_OK) idle_high();
    return e;
}

void app_main(void) {
    gpio_reset_pin(PIN_MARK);
    gpio_set_direction(PIN_MARK, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_MARK, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));             /* time to open the serial monitor */
    printf("\nRMTLINK the RMT GBA link test: jumpers GPIO5-GPIO6 and GPIO4-GPIO7\n");
    rmt_copy_encoder_config_t cc = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&cc, &s_copy));
    esp_err_t e = setup_all();
    if (e != ESP_OK) { printf("RMTLINK FAIL setting up the channels: %s\n", esp_err_to_name(e)); return; }

    /* the jumpers, first: with one missing, nothing comes back at all */
    uint16_t got;
    if (mp_once(0x5555, IDLE_GOOD, &got) == -12) {
        printf("RMTLINK FAIL nothing came back on GPIO6: is the jumper GPIO5-GPIO6 on?\n");
    }

    printf("RMTLINK A: Multi-Player frames, RX idle threshold 200 us\n");
    const int a = test_mp(IDLE_GOOD, 0);
    printf("RMTLINK B: the same at 60 us (the guide's signal_range_max_ns)\n");
    const int b = test_mp(IDLE_GUIDE, 1);
    printf("RMTLINK C: normal mode, SC and SO together\n");
    const int c = test_normal();
    printf("RMTLINK D: channels made, used and deleted 200 times\n");
    const int d = test_lifecycle();
    printf("RMTLINK RESULT A %s, B %s, C %s, D %s\n", a ? "FAIL" : "PASS", b ? "FAIL" : "PASS",
           c ? "FAIL" : "PASS", d ? "FAIL" : "PASS");

    /* the bursts, for the logic analyzer */
    if (setup_all() != ESP_OK) { printf("RMTLINK FAIL setting up again\n"); return; }
    printf("RMTLINK bursts: one a second, GPIO1 high during each (tools/la_check.py)\n");
    for (;;) {
        burst();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
