/*
 * sim_rfu.c -- the wireless adapter's link port in the host simulator: the
 * wires, the GBA hardware on the far end of them, and the adapter-side driver
 * (gbl_rfu_phy_t) the vendored wireless adapter mode runs on.
 *
 * The GBA end is hardware, not a protocol model. gba_io[] is the GBA's I/O
 * register block, and a thread watches and updates it the way the chip would:
 *
 *   SIOCNT / SIODATA32  Normal mode, 32 bits. Start with the internal clock
 *                       (bit 0) shifts SIODATA32 against what the adapter has
 *                       staged, in 16 us at 2 MHz or 125 us at 256 kHz; start
 *                       with the external clock arms it for the adapter to
 *                       clock. Bit 2 reads SI; bit 3 is SO while idle.
 *   RCNT                General-purpose mode drives SD: AgbRFU_SoftReset's
 *                       pulse.
 *   TMxCNT              Counters at the GBA's 16.78 MHz and prescalers;
 *                       writing L sets the reload, reading it the count.
 *   IE / IF / IME       Interrupts, delivered as SIGUSR1 to the thread running
 *                       the game, so a handler preempts game code where it
 *                       stands, as an IRQ does. Writing IF acknowledges.
 *
 * What runs as the game is pret's decompiled librfu (rfu_game.c), which is
 * the code FireRed, LeafGreen and Emerald run. Its STWI layer drives exactly
 * these registers, and nothing in this file knows the RFU protocol.
 *
 * The adapter end follows rfu_phy.h, i.e. upstream's RP2040 PIO programs:
 * SI low after a word until the next is staged, then SI = NOT SO; a GBA that
 * clocks before then gets nothing staged, as the PIO would give it.
 */
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "rfu_phy.h"
#include "sim.h"
#include "sim_rfu.h"
#include "zshim.h"

uint8_t gba_io[0x400] __attribute__((aligned(8)));

#define IO16(off) ((volatile uint16_t *)(gba_io + (off)))
#define IO32(off) ((volatile uint32_t *)(gba_io + (off)))

enum {
    R_TM0CNT = 0x100, R_SIODATA32 = 0x120, R_SIOCNT = 0x128, R_RCNT = 0x134,
    R_IE = 0x200, R_IF = 0x202, R_IME = 0x208
};
enum {
    CNT_INTERNAL = 0x0001, CNT_2MHZ = 0x0002, CNT_SI = 0x0004, CNT_SO = 0x0008,
    CNT_START = 0x0080, CNT_MODE = 0x3000, CNT_NORMAL32 = 0x1000, CNT_IRQ = 0x4000
};
enum { IRQ_TIMER0 = 1 << 3, IRQ_SERIAL = 1 << 7 };

#define GBA_HZ          16777216u
#define XFER_2MHZ_NS    16000      /* 32 bits at 2 MHz             */
#define XFER_256K_NS    125000     /* 32 bits at 256 kHz           */
#define XFER_ADAPTER_US 213        /* 32 bits at upstream's 150 kHz */

static pthread_mutex_t s_hw = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_hw_cv;      /* wakes the clock thread early */

/* ---- the adapter's side of the wires ---------------------------------------------- */

static bool         a_enabled;
static enum RfuRole a_role = RFU_ROLE_GBA_MASTER;
static bool         a_staged_valid;
static uint32_t     a_staged;
static bool         a_si;            /* ADAPTER_MASTER: what drive_si set */

/* ---- the GBA ------------------------------------------------------------------ */

static bool     g_busy;              /* a transfer is shifting           */
static int64_t  g_done_ns;
static uint32_t g_rx;                /* what SIODATA32 holds afterwards  */
static bool     g_armed;             /* external clock, start set        */
static bool     g_word_for_adapter;  /* this transfer reached the adapter */
static uint32_t g_adapter_rx;

static struct {
    bool     on;
    uint16_t ctl, reload;
    int64_t  base_ns;                /* count was base_count at base_ns  */
    uint32_t base_count;
} s_tm[4];

static atomic_uint s_if;
static atomic_int  s_irq_sent, s_in_irq;
static pthread_t   s_cpu;
static bool        s_cpu_set;
static sim_gba_irq_t s_irq_serial, s_irq_timer[4];

/* SD: AgbRFU_SoftReset's pulse, qualified by its width from the edges' times
 * (as the ESP32-S3 driver does from its edge interrupt) rather than by
 * catching it high at a poll, which a 1 ms poll can miss. */
static bool    sd_level, sd_pulse_pending, sd_pulse_reported;
static int64_t sd_rise_ns;
static atomic_uint s_counters[SIM_RFU_COUNTERS];

static int64_t now_ns(void) { return zshim_now_us() * 1000; }

/* The GBA's SO line. */
static bool gba_so(void) {
    const uint16_t rcnt = *IO16(R_RCNT);
    if (rcnt & 0x8000) return (rcnt & 0x80) ? (rcnt & 0x08) != 0 : true;
    return (*IO16(R_SIOCNT) & CNT_SO) != 0;
}

/* The GBA's SD line: driven only in general-purpose mode; the adapter pulls
 * it down otherwise. */
static bool gba_sd(void) {
    const uint16_t rcnt = *IO16(R_RCNT);
    return (rcnt & 0x8000) && (rcnt & 0x20) && (rcnt & 0x02);
}

/* The adapter's SI drive, which the GBA reads as SIOCNT bit 2. */
static bool adapter_si(void) {
    if (!a_enabled) return true;                 /* pulled up */
    if (a_role == RFU_ROLE_GBA_MASTER) return a_staged_valid && !gba_so();
    return a_si;
}

/* ---- timers ----------------------------------------------------------------------- */

static const uint32_t k_prescale[4] = { 1, 64, 256, 1024 };

/* Bring timer n's count up to now, taking its overflows. */
static void tm_advance(int n, int64_t now) {
    if (!s_tm[n].on) return;
    const uint64_t ticks = (uint64_t)(now - s_tm[n].base_ns) * GBA_HZ / 1000000000ull / k_prescale[s_tm[n].ctl & 3];
    if (ticks == 0) return;
    uint64_t c = s_tm[n].base_count + ticks;
    while (c > 0xFFFF) {
        if (s_tm[n].ctl & 0x40) atomic_fetch_or(&s_if, IRQ_TIMER0 << n);
        c = c - 0x10000 + s_tm[n].reload;
    }
    /* Rebase at the tick just counted, so fractions carry over. */
    s_tm[n].base_ns += (int64_t)(ticks * k_prescale[s_tm[n].ctl & 3] * 1000000000ull / GBA_HZ);
    s_tm[n].base_count = (uint32_t)c;
}

static void tm_write_ctl(int n, uint16_t v, int64_t now) {
    tm_advance(n, now);
    const bool on = (v & 0x80) != 0;
    if (on && !s_tm[n].on) {             /* 0 -> 1 loads the reload */
        s_tm[n].base_count = s_tm[n].reload;
        s_tm[n].base_ns = now;
    } else if (on) {
        s_tm[n].base_ns = now;           /* prescaler change: count so far kept */
    }
    s_tm[n].on = on;
    s_tm[n].ctl = v;
}

/* ---- SIO ------------------------------------------------------------------------------ */

/* A start bit was written: begin the transfer it asks for. */
static void sio_start(int64_t now) {
    const uint16_t cnt = *IO16(R_SIOCNT);
    if (g_busy || (*IO16(R_RCNT) & 0x8000) || (cnt & CNT_MODE) != CNT_NORMAL32 || !(cnt & CNT_START)) return;
    if (!(cnt & CNT_INTERNAL)) {
        g_armed = true;                  /* waits for the adapter's clock */
        return;
    }
    const uint32_t tx = *IO32(R_SIODATA32);
    g_armed = false;
    g_busy = true;
    g_done_ns = now + ((cnt & CNT_2MHZ) ? XFER_2MHZ_NS : XFER_256K_NS);
    if (a_enabled && a_role == RFU_ROLE_GBA_MASTER && a_staged_valid) {
        g_rx = a_staged;
        a_staged_valid = false;
        g_word_for_adapter = true;
        g_adapter_rx = tx;
        atomic_fetch_add(&s_counters[SIM_RFU_GBA_WORDS], 1);
    } else {
        /* Nothing staged: the PIO is parked at `pull` and shifts nothing; the
         * GBA reads the SI level it left. */
        g_rx = adapter_si() ? 0xFFFFFFFFu : 0;
        atomic_fetch_add(&s_counters[a_enabled && a_role == RFU_ROLE_ADAPTER_MASTER
                                         ? SIM_RFU_CLOCK_CLASH : SIM_RFU_UNSTAGED], 1);
    }
}

static void sd_update(int64_t now) {
    const bool sd = gba_sd();
    if (sd && !sd_level) {
        sd_rise_ns = now;
        sd_pulse_reported = false;
    } else if (!sd && sd_level && !sd_pulse_reported &&
               now - sd_rise_ns >= (int64_t)RFU_SD_HOLD_US * 1000) {
        sd_pulse_pending = true;
    }
    sd_level = sd;
}

/* ---- the register hooks (global.h's GbaReg) ----------------------------------------- */

/* Raise the IRQ if one is due. From the game's own thread (an IME/IE write)
 * the signal is blocked until the model is let go, so it is taken right
 * after the write that enabled it, as on the GBA. */
static void irq_check(void) {
    if (s_cpu_set && (*IO16(R_IME) & 1) && (atomic_load(&s_if) & *IO16(R_IE)) &&
        !atomic_exchange(&s_irq_sent, 1)) {
        pthread_kill(s_cpu, SIGUSR1);
    }
}

/* The game's thread must not take its IRQ while it holds the model: the
 * handler's own register accesses would find it mid-update. */
static void model_lock(sigset_t *old) {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, old);
    pthread_mutex_lock(&s_hw);
}

static void model_unlock(const sigset_t *old) {
    pthread_mutex_unlock(&s_hw);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

uint32_t sim_gba_io_read(uintptr_t addr, unsigned size, uint32_t stored) {
    const unsigned off = (unsigned)(addr - (uintptr_t)gba_io);
    sigset_t old;
    model_lock(&old);
    uint32_t v = stored;
    if (off >= R_TM0CNT && off < R_TM0CNT + 16 && (off & 3) == 0 && (size == 2 || size == 4)) {
        /* L reads the count; a 32-bit read is the count with H above it. */
        const int n = (int)(off - R_TM0CNT) / 4;
        tm_advance(n, now_ns());
        v = (uint16_t)s_tm[n].base_count;
        if (size == 4) v |= (uint32_t)s_tm[n].ctl << 16;
    } else if (off == R_SIOCNT) {
        v = (*IO16(R_SIOCNT) & ~CNT_SI) | (adapter_si() ? CNT_SI : 0);
        if (size == 1) v &= 0xFF;
    } else if (off == R_IF) {
        v = atomic_load(&s_if);
    }
    model_unlock(&old);
    return v;
}

void sim_gba_io_write(uintptr_t addr, unsigned size, uint32_t value) {
    const unsigned off = (unsigned)(addr - (uintptr_t)gba_io);
    const int64_t now = now_ns();
    sigset_t old;
    model_lock(&old);
    if (off >= R_TM0CNT && off < R_TM0CNT + 16 && (off & 3) == 0 && size == 4) {
        /* A 32-bit write sets the reload and the control together. */
        const int n = (int)(off - R_TM0CNT) / 4;
        s_tm[n].reload = (uint16_t)value;
        tm_write_ctl(n, (uint16_t)(value >> 16), now);
        pthread_cond_signal(&s_hw_cv);
    } else if (off >= R_TM0CNT && off < R_TM0CNT + 16 && size == 2) {
        const int n = (int)(off - R_TM0CNT) / 4;
        if (off & 2) tm_write_ctl(n, (uint16_t)value, now);
        else s_tm[n].reload = (uint16_t)value;
        pthread_cond_signal(&s_hw_cv);              /* a new overflow time */
    } else if (off == R_IF) {
        atomic_fetch_and(&s_if, ~value);                  /* write 1 to acknowledge */
    } else if (off == R_SIOCNT || off == R_SIOCNT + 1) {
        uint16_t cnt = *IO16(R_SIOCNT);
        if (size == 2) cnt = (uint16_t)value;
        else if (off == R_SIOCNT) cnt = (uint16_t)((cnt & 0xFF00) | (value & 0xFF));
        else cnt = (uint16_t)((cnt & 0x00FF) | ((value & 0xFF) << 8));
        if (g_busy) cnt |= CNT_START;                     /* busy reads back set */
        *IO16(R_SIOCNT) = cnt;
        if (!(cnt & CNT_START) || (cnt & CNT_INTERNAL)) g_armed = false;
        sio_start(now);
        pthread_cond_signal(&s_hw_cv);              /* a transfer to finish */
    } else if (off == R_RCNT && size == 2) {
        *IO16(R_RCNT) = (uint16_t)value;
        sd_update(now);
    } else {
        memcpy(gba_io + off, &value, size);
        if (off == R_IME || off == R_IE) irq_check();
    }
    model_unlock(&old);
}

/* ---- the GBA's clock: what happens between register accesses ------------------------- */

/* When the clock thread next has something to do: a transfer ending, or a
 * timer that interrupts overflowing. */
static int64_t next_deadline(int64_t now) {
    int64_t t = now + 5000000;
    if (g_busy && g_done_ns < t) t = g_done_ns;
    for (int n = 0; n < 4; n++) {
        if (!s_tm[n].on || !(s_tm[n].ctl & 0x40)) continue;
        const uint64_t left = 0x10000 - s_tm[n].base_count;
        const int64_t at = s_tm[n].base_ns +
            (int64_t)(left * k_prescale[s_tm[n].ctl & 3] * 1000000000ull / GBA_HZ);
        if (at < t) t = at;
    }
    return t;
}

/* The GBA's clock: transfers ending and timers overflowing between register
 * accesses. It sleeps to the next of those (or until a register write starts
 * one) rather than spinning, so the threads it shares the host with -- the
 * game's above all -- are not crowded off the CPUs. */
static void *hw_thread(void *arg) {
    (void)arg;
    prctl(PR_SET_TIMERSLACK, 1UL);
    pthread_mutex_lock(&s_hw);
    for (;;) {
        const int64_t now = now_ns();
        bool deliver = false;
        uint32_t deliver_rx = 0;
        for (int n = 0; n < 4; n++) tm_advance(n, now);
        if (g_busy && now >= g_done_ns) {
            *IO32(R_SIODATA32) = g_rx;
            __atomic_fetch_and(IO16(R_SIOCNT), (uint16_t)~CNT_START, __ATOMIC_SEQ_CST);
            if (*IO16(R_SIOCNT) & CNT_IRQ) atomic_fetch_or(&s_if, IRQ_SERIAL);
            g_busy = false;
            if (g_word_for_adapter) {
                deliver = true;
                deliver_rx = g_adapter_rx;
                g_word_for_adapter = false;
            }
        }
        if (deliver) {
            /* The adapter's transfer-done interrupt, outside the model's
             * lock: the callback stages its answer through the driver. */
            pthread_mutex_unlock(&s_hw);
            gbl_rfu_word_done(deliver_rx);
            pthread_mutex_lock(&s_hw);
        }
        irq_check();

        const int64_t wait_ns = next_deadline(now_ns()) - now_ns();
        if (wait_ns > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t at = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + wait_ns;
            ts.tv_sec = at / 1000000000;
            ts.tv_nsec = at % 1000000000;
            pthread_cond_timedwait(&s_hw_cv, &s_hw, &ts);
        }
    }
    return NULL;
}

/* The IRQ, on the game's thread, dispatched as FireRed's IntrMain does it
 * (pokefirered src/crt0.s): the highest-priority pending interrupt only, its
 * IF bit acknowledged, and -- while its handler runs -- IE narrowed to the
 * interrupts allowed to nest (VCount, HBlank, serial, timer 3, STWI's timer,
 * Game Pak) minus the one being handled, with IRQs enabled again. librfu
 * depends on that: a REQ callback inside the serial handler sends the next
 * command (STWI_start_Command sets serial back in IE) and waits for it,
 * and only nested serial interrupts can finish it. IE and IME are restored
 * on the way out. The signal is not deferred (SA_NODEFER), so a nested IRQ
 * is a nested signal; it is blocked only while this code edits the
 * registers. */
enum { IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2, IRQ_GAMEPAK = 1 << 13 };
static const uint16_t k_irq_priority[] = {
    IRQ_VCOUNT, IRQ_SERIAL, IRQ_TIMER0 << 3, IRQ_HBLANK, IRQ_VBLANK,
    IRQ_TIMER0, IRQ_TIMER0 << 1, IRQ_TIMER0 << 2,
};
#define IRQ_NESTABLE (IRQ_VCOUNT | IRQ_HBLANK | IRQ_SERIAL | (IRQ_TIMER0 << 3) | IRQ_GAMEPAK)

static void irq_handler(int sig) {
    (void)sig;
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    atomic_store(&s_irq_sent, 0);
    uint16_t bit = 0;
    if (*IO16(R_IME) & 1) {
        const unsigned pending = atomic_load(&s_if) & *IO16(R_IE);
        for (size_t k = 0; k < sizeof k_irq_priority / sizeof k_irq_priority[0] && !bit; k++)
            if (pending & k_irq_priority[k]) bit = k_irq_priority[k];
    }
    if (!bit) {
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        return;
    }
    atomic_fetch_and(&s_if, ~(unsigned)bit);
    const uint16_t ie = *IO16(R_IE), ime = *IO16(R_IME);
    *IO16(R_IE) = (uint16_t)(ie & ~bit & IRQ_NESTABLE);
    *IO16(R_IME) = 1;
    atomic_fetch_add(&s_in_irq, 1);
    irq_check();                                  /* one that may nest now */
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (bit == IRQ_SERIAL) { if (s_irq_serial) s_irq_serial(); }
    else
        for (int n = 0; n < 4; n++)
            if (bit == (IRQ_TIMER0 << n) && s_irq_timer[n]) s_irq_timer[n]();

    pthread_sigmask(SIG_BLOCK, &block, NULL);
    atomic_fetch_sub(&s_in_irq, 1);
    *IO16(R_IE) = ie;
    *IO16(R_IME) = ime;
    irq_check();                                  /* one that came due meanwhile */
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void sim_gba_set_irq(sim_gba_irq_t serial, int timer, sim_gba_irq_t timer_fn) {
    s_irq_serial = serial;
    if (timer >= 0 && timer < 4) s_irq_timer[timer] = timer_fn;
}

void sim_gba_attach_cpu(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = irq_handler;
    sa.sa_flags = SA_RESTART | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    s_cpu = pthread_self();
    s_cpu_set = true;
}

void sim_gba_cpufill16(uint16_t value, void *dst, size_t size) {
    uint16_t *p = dst;
    for (size_t i = 0; i < size / 2; i++) p[i] = value;
}

void sim_gba_cpufill32(uint32_t value, void *dst, size_t size) {
    uint32_t *p = dst;
    for (size_t i = 0; i < size / 4; i++) p[i] = value;
}

uint32_t sim_rfu_counter(int which) { return atomic_load(&s_counters[which]); }

/* ---- the adapter driver -------------------------------------------------------------- */

static void drv_enable(void) {
    pthread_mutex_lock(&s_hw);
    a_enabled = true;
    a_role = RFU_ROLE_GBA_MASTER;
    a_staged_valid = false;
    sd_pulse_pending = false;
    sd_pulse_reported = sd_level;
    pthread_mutex_unlock(&s_hw);
}

static void drv_disable(void) {
    pthread_mutex_lock(&s_hw);
    a_enabled = false;
    a_staged_valid = false;
    pthread_mutex_unlock(&s_hw);
}

static void drv_set_role(enum RfuRole role) {
    pthread_mutex_lock(&s_hw);
    a_role = role;
    a_staged_valid = false;
    a_si = false;            /* upstream seeds SI low on the takeover */
    pthread_mutex_unlock(&s_hw);
}

static void drv_stage(uint32_t word) {
    pthread_mutex_lock(&s_hw);
    a_staged = word;
    a_staged_valid = true;
    pthread_mutex_unlock(&s_hw);
}

/* The adapter clocks a word. A real GBA arms for it microseconds after the
 * handshake steps the section waits on, and the section's fixed waits (150 us
 * before the first word, 250 us between words) are calibrated against that;
 * here the GBA is a host thread that can be descheduled for longer. So the
 * clock waits, up to ARM_GRACE_US, for the GBA to arm, and counts the wait
 * (SIM_RFU_LATE_ARM) apart from a word lost to a GBA that never armed
 * (SIM_RFU_UNARMED). */
#define ARM_GRACE_US 2000

static bool drv_exchange(uint32_t word, uint32_t *rx, uint32_t timeout_us) {
    (void)timeout_us;
    const int64_t give_up = zshim_now_us() + ARM_GRACE_US;
    bool waited = false;
    pthread_mutex_lock(&s_hw);
    while (a_enabled && a_role == RFU_ROLE_ADAPTER_MASTER && !(g_armed && !g_busy) &&
           zshim_now_us() < give_up) {
        waited = true;
        pthread_mutex_unlock(&s_hw);
        k_busy_wait(2);
        pthread_mutex_lock(&s_hw);
    }
    if (!a_enabled || a_role != RFU_ROLE_ADAPTER_MASTER) {
        pthread_mutex_unlock(&s_hw);
        return false;
    }
    if (g_armed && !g_busy) {
        *rx = *IO32(R_SIODATA32);
        g_rx = word;
        g_busy = true;
        g_armed = false;
        g_word_for_adapter = false;
        g_done_ns = now_ns() + XFER_ADAPTER_US * 1000;
        pthread_cond_signal(&s_hw_cv);
        atomic_fetch_add(&s_counters[SIM_RFU_ADAPTER_WORDS], 1);
        if (waited) atomic_fetch_add(&s_counters[SIM_RFU_LATE_ARM], 1);
    } else {
        /* Clocking a GBA that is not listening shifts nothing into it; what
         * comes back is its idle SO level. */
        *rx = gba_so() ? 0xFFFFFFFFu : 0;
        atomic_fetch_add(&s_counters[SIM_RFU_UNARMED], 1);
    }
    pthread_mutex_unlock(&s_hw);
    k_busy_wait(XFER_ADAPTER_US);
    return true;
}

static void drv_drive_si(bool high) {
    pthread_mutex_lock(&s_hw);
    a_si = high;
    pthread_mutex_unlock(&s_hw);
}

static bool drv_so_high(void) {
    pthread_mutex_lock(&s_hw);
    const bool so = gba_so();
    pthread_mutex_unlock(&s_hw);
    return so;
}

static void drv_realign(void) {
    pthread_mutex_lock(&s_hw);
    a_staged_valid = false;
    pthread_mutex_unlock(&s_hw);
}

static bool drv_sd_reset_seen(uint8_t *pin) {
    pthread_mutex_lock(&s_hw);
    bool seen = sd_pulse_pending;
    sd_pulse_pending = false;
    if (!seen && sd_level && !sd_pulse_reported &&
        now_ns() - sd_rise_ns >= (int64_t)RFU_SD_HOLD_US * 1000) {
        seen = true;                 /* still high, already long enough */
        sd_pulse_reported = true;
    }
    pthread_mutex_unlock(&s_hw);
    if (seen) *pin = 3;              /* upstream's RFU_PIN_SD_GBA */
    return seen;
}

static void drv_wait_reset_end(void) {
    const int64_t start = zshim_now_us();
    for (;;) {
        pthread_mutex_lock(&s_hw);
        const bool high = sd_level;
        pthread_mutex_unlock(&s_hw);
        if (!high || zshim_now_us() - start > RFU_SD_PULSE_MAX_US) break;
        k_busy_wait(2);
    }
    k_busy_wait(RFU_SD_SETTLE_US);
}

static uint8_t drv_lines(void) {
    pthread_mutex_lock(&s_hw);
    const uint8_t l = (uint8_t)((gba_so() ? 1 : 0) | (adapter_si() ? 2 : 0) | 4);
    pthread_mutex_unlock(&s_hw);
    return l;
}

static const gbl_rfu_phy_t s_drv = {
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

void sim_rfu_init(void) {
    /* The GBA's power-on state: SIO in Normal mode, SO idle high. */
    *IO16(R_SIOCNT) = CNT_SO;
    gbl_rfu_install(&s_drv);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s_hw_cv, &ca);
    pthread_t t;
    pthread_create(&t, NULL, hw_thread, NULL);
    pthread_detach(t);
}
