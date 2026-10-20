/*
 * sim_gba.c -- the far end of the link cable, for the host simulator.
 *
 * Three things a real adapter is plugged into, modelled just far enough that
 * the vendored firmware cannot tell:
 *
 *   - a GBA on the Multi-Play bus, as child (adapter is MASTER) or as parent
 *     (adapter is SLAVE), running the Gen 3 link handshake and then frames of
 *     crc + 8 command words, exactly the shape packetLayer.cpp expects;
 *   - a Game Boy on the 8-bit SPI bus (gb_link_transfer), answering from a
 *     script the test supplies;
 *   - a Game Boy driving the printer protocol over the raw pins gb.cpp
 *     bit-bangs, by toggling SC and reading SO like a real DMG.
 *
 * Every link word is reported to the test as SIM_EV_WORD, so assertions can
 * be made about the wire rather than about the firmware's internals.
 *
 * Control records ('G', see sim_port.c), first byte is the op:
 *   0x01 model        set the bus model (SIM_GBA_*)
 *   0x02 w0..w7       queue one 8-word command frame for the GBA to send
 *   0x03 n            parent model: send the connect word (0x8FFF) after n
 *                     handshake words have come back as 0xB9A0 (0 = never)
 *   0x04 bytes...     queue Game Boy SPI reply bytes
 *   0x05 bytes...     printer: clock these bytes out as the Game Boy would
 *   0x06 0|1          GBA cable (SI grounded) or GBC cable (SI floats high);
 *                     also puts the GBA's SD on the matching path
 *   0x07 0|1          SD on the alternate path (wrong-pin scenarios)
 *   0x20-0x3F         the wireless adapter's game (rfu_game.cpp)
 */
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>

#include "layers/gbLinkLayer.h"
#include "link_phy.h"
#include "sim.h"
#include "sim_rfu.h"
#include "zshim.h"

enum {
    SIM_GBA_NONE   = 0,  /* nothing plugged in                          */
    SIM_GBA_CHILD  = 1,  /* a Gen 3 game, waiting as child               */
    SIM_GBA_PARENT = 2,  /* a Gen 3 game, driving the bus as parent      */
    SIM_GBA_ECHO   = 3,  /* answers every word with the word it was sent */
    SIM_GBA_MMBN   = 4   /* a Battle Network game polling a chip gate:
                            parent; pings 0x8FFF until a gate answers, echoes
                            the gate's ID to open the session, then clocks */
};

static uint16_t s_mmbn_gate;          /* gate ID the game has seen, 0 = none */
static int      s_mmbn_phase;         /* 0 ping, 1 ack, 2 session            */

#define HS_SLAVE   0xB9A0u
#define HS_MASTER  0x8FFFu

/* Upstream pacing, so a parent-model GBA paces like the real thing does. */
#define GAP_HANDSHAKE 30097u
#define GAP_WORD      1378u
#define GAP_FRAME     12953u

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static atomic_int s_phy_mode = GBL_PHY_OFF;
static atomic_int s_sd_alt;
static atomic_int s_model = SIM_GBA_NONE;
static atomic_int s_gbc_cable = 1;      /* the firmware's fresh default is GBC */
static atomic_int s_wire_on_alt = 1;    /* which SD path the GBA is really on  */

/* The GBA's own view of the Gen 3 link. */
static bool     s_connected;
static int      s_word_index;           /* 0 = crc, 1..8 = commands */
static uint16_t s_crc_acc;
static uint16_t s_last_crc;
static uint16_t s_frame_out[8];
static uint16_t s_queue[64][8];
static int      s_q_head, s_q_count;
static int      s_hs_seen;
static int      s_connect_after = 3;
static bool     s_close_out, s_close_in; /* READY_CLOSE_LINK seen each way */

/* GB SPI replies. */
static uint8_t s_gb_reply[4096];
static size_t  s_gb_head, s_gb_count;

static void sleep_ns(uint64_t ns) {
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
}

/* ---- the GBA's Gen 3 frame engine (shared by both models) ---------------- */

static void reset_link(void) {
    s_connected = false;
    s_word_index = 0;
    s_crc_acc = 0;
    s_last_crc = HS_SLAVE; /* packetLayer's first crc is the handshake */
    s_hs_seen = 0;
    s_close_out = s_close_in = false;
}

static void load_frame(void) {
    if (s_q_count > 0) {
        memcpy(s_frame_out, s_queue[s_q_head], sizeof s_frame_out);
        s_q_head = (s_q_head + 1) % 64;
        s_q_count--;
    } else {
        /* An idle game in a link room sends "no key held". */
        memset(s_frame_out, 0, sizeof s_frame_out);
        s_frame_out[0] = 0xCAFE;
        s_frame_out[1] = 0x11;
    }
}

/* The word the GBA drives this transfer, given the one it will receive.
 * A frame is crc then 8 commands; the crc is the sum of all 16 words of the
 * previous frame, both directions (packetLayer.hpp: m_crc). */
static uint16_t gba_word_out(void) {
    if (!s_connected) return HS_SLAVE;
    if (s_word_index == 0) return s_last_crc;
    return s_frame_out[s_word_index - 1];
}

static void gba_word_in(uint16_t out, uint16_t in) {
    if (!s_connected) {
        if (in == HS_MASTER || out == HS_MASTER) {
            s_connected = true;
            s_word_index = 0;
            s_crc_acc = 0;
            load_frame();
        } else if (in == HS_SLAVE) {
            s_hs_seen++;
        }
        return;
    }
    if (s_word_index == 0) {
        s_crc_acc = 0;
    } else {
        s_crc_acc = (uint16_t)(s_crc_acc + out + in);
        if (s_word_index == 1) {
            if (out == 0x5FFF) s_close_out = true;
            if (in == 0x5FFF)  s_close_in = true;
        }
    }
    if (++s_word_index == 9) {
        s_word_index = 0;
        s_last_crc = s_crc_acc;
        /* Like the game: once both sides have said READY_CLOSE_LINK the link
         * is torn down and the next session starts from the handshake. */
        if (s_close_out && s_close_in) {
            reset_link();
            return;
        }
        load_frame();
    }
}

/* ---- PHY driver -------------------------------------------------------------- */

static bool wire_ok(void) {
    /* A word only gets through if the firmware listens on the SD path the GBA
     * is actually wired to. */
    return atomic_load(&s_sd_alt) == atomic_load(&s_wire_on_alt);
}

static void drv_configure(gbl_phy_mode_t mode, bool sd_alt) {
    atomic_store(&s_sd_alt, sd_alt);
    atomic_store(&s_phy_mode, mode);
    uint8_t ev[2] = { (uint8_t)mode, sd_alt };
    sim_event(SIM_EV_PHY_MODE, ev, 2);
}

static bool drv_si_seen_high(uint32_t window_us) {
    (void)window_us;
    return atomic_load(&s_gbc_cable) != 0;
}

static uint8_t drv_read_pins(void) {
    int mode = atomic_load(&s_phy_mode);
    int model = atomic_load(&s_model);
    /* A parent GBA toggles SC; report SC low half the time. */
    uint8_t m = 0;
    if (model == SIM_GBA_PARENT && mode != GBL_PHY_OFF) {
        m |= (zshim_now_us() / 1000) & 1 ? 0x01 : 0;
    }
    m |= 0x02 | 0x08 | 0x10;
    return m;
}

static void drv_release(void) {}

static const gbl_phy_driver_t s_drv = {
    .configure    = drv_configure,
    .si_seen_high = drv_si_seen_high,
    .read_pins    = drv_read_pins,
    .release      = drv_release,
    .has_sd_alt   = true,
};

static atomic_uint s_words_done;
uint32_t sim_gba_words(void) { return atomic_load(&s_words_done); }

static void report_word(uint16_t tx, uint16_t rx) {
    atomic_fetch_add(&s_words_done, 1);
    uint8_t ev[4] = { (uint8_t)tx, (uint8_t)(tx >> 8), (uint8_t)rx, (uint8_t)(rx >> 8) };
    sim_event(SIM_EV_WORD, ev, 4);
}

static void *bus_thread(void *arg) {
    (void)arg;
    uint32_t parent_gap = GAP_HANDSHAKE;
    for (;;) {
        int mode  = atomic_load(&s_phy_mode);
        int model = atomic_load(&s_model);

        if (mode == GBL_PHY_MASTER) {
            uint32_t gap = 0;
            uint16_t tx = gbl_phy_next_tx(&gap);
            uint16_t rx = 0xFFFF; /* nobody answered: SD idles high */
            pthread_mutex_lock(&s_mx);
            if (wire_ok() && model == SIM_GBA_CHILD) {
                rx = gba_word_out();
                gba_word_in(rx, tx);
            } else if (wire_ok() && model == SIM_GBA_ECHO) {
                rx = tx;
            }
            pthread_mutex_unlock(&s_mx);
            report_word(tx, rx);
            gbl_phy_word_done(rx);
            sleep_ns(gbl_phy_loops_to_ns(gap ? gap : 1000));
            continue;
        }

        if ((mode == GBL_PHY_SLAVE || mode == GBL_PHY_SLAVE_EREADER) &&
            model == SIM_GBA_MMBN) {
            uint16_t gw = s_mmbn_phase == 0 ? 0x8FFF
                        : s_mmbn_phase == 1 ? s_mmbn_gate : 0x0000;
            uint16_t tx = gbl_phy_next_tx(NULL);
            report_word(tx, gw);
            gbl_phy_word_done(gw);
            if (s_mmbn_phase == 0 && (tx & 0xFF00) == 0xFF00 && tx != 0xFFFF) {
                s_mmbn_gate = tx;            /* a gate announced itself */
                s_mmbn_phase = 1;
            } else if (s_mmbn_phase == 1) {
                s_mmbn_phase = 2;
            }
            sleep_ns(2000000);               /* faster than a frame, for tests */
            continue;
        }

        if ((mode == GBL_PHY_SLAVE || mode == GBL_PHY_SLAVE_EREADER) &&
            model == SIM_GBA_PARENT) {
            pthread_mutex_lock(&s_mx);
            uint16_t gw = gba_word_out();
            if (!s_connected && s_connect_after > 0 && s_hs_seen >= s_connect_after) {
                gw = HS_MASTER;
            }
            pthread_mutex_unlock(&s_mx);

            uint16_t tx = gbl_phy_next_tx(NULL);
            if (!wire_ok()) {
                /* The firmware is listening on the wrong SD pin: SC toggles,
                 * no word ever lands. */
                sleep_ns(gbl_phy_loops_to_ns(parent_gap));
                continue;
            }
            report_word(tx, gw);
            gbl_phy_word_done(gw);

            pthread_mutex_lock(&s_mx);
            gba_word_in(gw, tx);
            bool conn = s_connected;
            int idx = s_word_index;
            pthread_mutex_unlock(&s_mx);
            parent_gap = !conn ? GAP_HANDSHAKE : (idx == 0 ? GAP_FRAME : GAP_WORD);
            sleep_ns(gbl_phy_loops_to_ns(parent_gap));
            continue;
        }

        sleep_ns(1000000);
    }
    return NULL;
}

/* ---- GB SPI (gbLinkLayer.h) ----------------------------------------------------- */

void gb_link_init(void)   { sim_event(SIM_EV_PHY_MODE, (uint8_t[]){ 0x10, 0 }, 2); }
void gb_link_deinit(void) { sim_event(SIM_EV_PHY_MODE, (uint8_t[]){ 0x11, 0 }, 2); }

void gb_link_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t us_between) {
    for (uint32_t i = 0; i < len; i++) {
        pthread_mutex_lock(&s_mx);
        uint8_t r = 0x00;
        if (s_gb_count > 0) {
            r = s_gb_reply[s_gb_head];
            s_gb_head = (s_gb_head + 1) % sizeof s_gb_reply;
            s_gb_count--;
        }
        pthread_mutex_unlock(&s_mx);
        rx[i] = r;
        /* With when it went, so a test can measure the gaps. */
        const uint32_t t = (uint32_t)zshim_now_us();
        uint8_t ev[6] = { tx[i], r, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
        sim_event(SIM_EV_GB_BYTE, ev, sizeof ev);
        if (us_between) k_busy_wait(us_between);
    }
}

/* ---- raw pins, for the printer -----------------------------------------------------
 *
 * The Game Boy is the SPI master in printer mode: it drives SC and SI (our
 * input) and samples our SO on each rising edge. A thread plays that part at
 * a slower clock than the DMG's 8 kHz, MSB first: a host thread can be
 * descheduled for longer than a real half-bit, which would read as a line
 * error rather than the protocol fault a test is looking for. */

static atomic_int s_pin_sc = 1, s_pin_si = 0, s_pin_so = 0;
static atomic_int s_so_puts, s_si_reads;

/* Lockstep with the firmware's own pin accesses: after each SC edge, wait
 * (bounded) until gb.cpp has acted on it -- it drives SO once per low phase
 * and reads SI once per high phase. On the board the loop runs undisturbed on
 * core 1 and keeps up with the DMG; a host thread can be descheduled for
 * longer than a half-bit, and the test would then be measuring the host
 * scheduler instead of the protocol code. */
static void wait_counter(atomic_int *c, int from) {
    for (int i = 0; i < 20000 && atomic_load(c) == from; i++) sleep_ns(5000);
}
static uint8_t s_pr_tx[8192];
static size_t  s_pr_len, s_pr_pos;
static uint8_t s_pr_rx[8192];

/* gb.cpp's printer timeouts are loop counts (10M idle, 2M mid-packet) sized
 * for an RP2040, where one gpio_get() iteration is ~64 ns: 2M is ~128 ms. A
 * host does the iteration in under a nanosecond, which shrinks the mid-packet
 * timeout to ~1 ms and makes a scheduling hiccup abort a print. Charge each
 * read roughly what the RP2040 pays, so the vendored constants mean here what
 * they mean on the board. (The ESP32-S3 port calibrates the same way.) */
static void rp2040_read_cost(void) {
    int64_t t = zshim_now_us();
    for (volatile int i = 0; i < 12; i++) { (void)t; }
}

bool gblink_pin_get(gblink_signal_t sig) {
    rp2040_read_cost();
    switch (sig) {
    case GBLINK_SIG_SC: return atomic_load(&s_pin_sc);
    case GBLINK_SIG_SI: atomic_fetch_add(&s_si_reads, 1); return atomic_load(&s_pin_si);
    default:            return true;
    }
}

void gblink_pin_put(gblink_signal_t sig, bool high) {
    if (sig == GBLINK_SIG_SO) {
        atomic_store(&s_pin_so, high);
        atomic_fetch_add(&s_so_puts, 1);
    }
}

void gblink_pin_init(gblink_signal_t sig)            { (void)sig; }
void gblink_pin_dir(gblink_signal_t sig, bool out)   { (void)sig; (void)out; }
void gblink_pin_pull(gblink_signal_t sig, bool up)   { (void)sig; (void)up; }

static void *printer_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_mx);
        bool have = s_pr_pos < s_pr_len;
        uint8_t byte = have ? s_pr_tx[s_pr_pos] : 0;
        pthread_mutex_unlock(&s_mx);
        if (!have) { sleep_ns(2000000); continue; }

        uint8_t in = 0;
        for (int b = 7; b >= 0; b--) {
            atomic_store(&s_pin_si, (byte >> b) & 1);
            int puts = atomic_load(&s_so_puts);
            atomic_store(&s_pin_sc, 0);
            sleep_ns(61000);
            wait_counter(&s_so_puts, puts);
            int reads = atomic_load(&s_si_reads);
            atomic_store(&s_pin_sc, 1);
            in = (uint8_t)((in << 1) | (atomic_load(&s_pin_so) & 1));
            sleep_ns(61000);
            wait_counter(&s_si_reads, reads);
        }
        pthread_mutex_lock(&s_mx);
        s_pr_rx[s_pr_pos] = in;
        s_pr_pos++;
        bool done = s_pr_pos == s_pr_len;
        pthread_mutex_unlock(&s_mx);
        uint8_t ev[2] = { byte, in };
        sim_event(SIM_EV_GB_BYTE, ev, 2);
        if (done) sim_event(SIM_EV_GB_BYTE, NULL, 0);
        sleep_ns(200000); /* inter-byte gap */
    }
    return NULL;
}

/* ---- control ------------------------------------------------------------------------ */

void sim_gba_control(const uint8_t *d, size_t len) {
    if (len == 0) return;
    if (d[0] >= 0x20 && d[0] <= 0x3F) { /* the wireless adapter's game */
        sim_rfu_game_control(d, len);
        return;
    }
    pthread_mutex_lock(&s_mx);
    switch (d[0]) {
    case 0x01:
        if (len >= 2) {
            atomic_store(&s_model, d[1]);
            reset_link();
            s_mmbn_phase = 0;
            s_mmbn_gate = 0;
        }
        break;
    case 0x02:
        if (len >= 17 && s_q_count < 64) {
            int slot = (s_q_head + s_q_count) % 64;
            for (int i = 0; i < 8; i++)
                s_queue[slot][i] = (uint16_t)(d[1 + 2 * i] | (d[2 + 2 * i] << 8));
            s_q_count++;
        }
        break;
    case 0x03:
        if (len >= 2) s_connect_after = d[1];
        break;
    case 0x04:
        for (size_t i = 1; i < len && s_gb_count < sizeof s_gb_reply; i++) {
            s_gb_reply[(s_gb_head + s_gb_count) % sizeof s_gb_reply] = d[i];
            s_gb_count++;
        }
        break;
    case 0x05:
        s_pr_len = s_pr_pos = 0;
        for (size_t i = 1; i < len && s_pr_len < sizeof s_pr_tx; i++)
            s_pr_tx[s_pr_len++] = d[i];
        break;
    case 0x06:
        if (len >= 2) { atomic_store(&s_gbc_cable, d[1]); atomic_store(&s_wire_on_alt, d[1]); }
        break;
    case 0x07:
        if (len >= 2) atomic_store(&s_wire_on_alt, d[1]);
        break;
    }
    pthread_mutex_unlock(&s_mx);
}

void sim_gba_init(void) {
    reset_link();
    gbl_phy_install(&s_drv);
    pthread_t a, b;
    pthread_create(&a, NULL, bus_thread, NULL);
    pthread_create(&b, NULL, printer_thread, NULL);
    pthread_detach(a);
    pthread_detach(b);
}
