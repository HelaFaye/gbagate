/*
 * sim_rfu_wire.c -- the Wireless Adapter's link port when the GBA is another
 * process (rfu_wire.h): gblink_sim with GBLINK_RFU_WIRE=<file> runs the
 * vendored adapter mode on this driver instead of sim_rfu.c's built-in GBA,
 * and test/mgba's emulator, running the real game, is the GBA on the far
 * end of the shared block.
 *
 * Each function is sim_rfu.c's, with the GBA's state read from the block
 * rather than from the in-process model; the GBA side keeps the block
 * current from the emulator's own thread.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <zephyr/kernel.h>

#include "rfu_phy.h"
#include "rfu_wire.h"
#include "sim.h"
#include "zshim.h"

#define XFER_ADAPTER_US 213        /* 32 bits at upstream's 150 kHz            */
#define ARM_GRACE_US    20000      /* as sim_rfu.c: the GBA arms within this    */

static struct rfu_wire *W;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define LOCKED(body) do { rfu_wire_lock(W); body; rfu_wire_unlock(W); } while (0)

static void drv_enable(void) {
    LOCKED(W->enabled = true; W->adapter_master = false; W->staged_valid = false;
           W->sd_pulse_pending = false; W->sd_pulse_reported = W->sd_level);
}

static void drv_disable(void) {
    LOCKED(W->enabled = false; W->staged_valid = false);
}

static void drv_set_role(enum RfuRole role) {
    /* As sim_rfu.c: let the GBA's serial handler for the last word run (it
     * arms as slave) before SI is seeded low for the takeover. */
    if (role == RFU_ROLE_ADAPTER_MASTER) {
        const int64_t give_up = now_us() + ARM_GRACE_US;
        for (;;) {
            bool owed;
            LOCKED(owed = W->serial_owed);
            if (!owed || now_us() >= give_up) break;
            k_busy_wait(2);
        }
    }
    LOCKED(W->adapter_master = (role == RFU_ROLE_ADAPTER_MASTER); W->staged_valid = false; W->a_si = false);
}

static void drv_stage(uint32_t word) {
    LOCKED(W->staged = word; W->staged_valid = true);
}

static bool drv_exchange(uint32_t word, uint32_t *rx, uint32_t timeout_us) {
    (void)timeout_us;
    const int64_t give_up = now_us() + ARM_GRACE_US;
    bool waited = false;
    for (;;) {
        rfu_wire_lock(W);
        if (!W->enabled || !W->adapter_master) {
            rfu_wire_unlock(W);
            return false;
        }
        if (W->armed && !W->busy) {
            *rx = W->gba_tx;
            W->in_word = word;
            W->in_at = now_us() + XFER_ADAPTER_US;
            W->in_pending = true;
            W->busy = true;
            W->armed = false;
            W->adapter_words++;
            if (waited) W->late_arm++;
            rfu_wire_unlock(W);
            break;
        }
        if (now_us() >= give_up) {
            /* a GBA that is not listening: its idle SO level comes back */
            *rx = W->gba_so ? 0xFFFFFFFFu : 0;
            W->unarmed++;
            rfu_wire_unlock(W);
            break;
        }
        rfu_wire_unlock(W);
        waited = true;
        k_busy_wait(2);
    }
    k_busy_wait(XFER_ADAPTER_US);
    return true;
}

static void drv_drive_si(bool high) { LOCKED(W->a_si = high); }

static bool drv_so_high(void) {
    bool so;
    LOCKED(so = W->gba_so);
    return so;
}

static void drv_realign(void) { LOCKED(W->staged_valid = false); }

static bool drv_sd_reset_seen(uint8_t *pin) {
    bool seen;
    rfu_wire_lock(W);
    seen = W->sd_pulse_pending;
    W->sd_pulse_pending = false;
    if (!seen && W->sd_level && !W->sd_pulse_reported &&
        now_us() - W->sd_rise_us >= (int64_t)RFU_SD_HOLD_US) {
        seen = true;
        W->sd_pulse_reported = true;
    }
    rfu_wire_unlock(W);
    if (seen) *pin = 3;              /* upstream's RFU_PIN_SD_GBA */
    return seen;
}

static void drv_wait_reset_end(void) {
    const int64_t start = now_us();
    for (;;) {
        bool high;
        LOCKED(high = W->sd_level);
        if (!high || now_us() - start > RFU_SD_PULSE_MAX_US) break;
        k_busy_wait(2);
    }
    k_busy_wait(RFU_SD_SETTLE_US);
}

static uint8_t drv_lines(void) {
    uint8_t l;
    LOCKED(l = (uint8_t)((W->gba_so ? 1 : 0) | (rfu_wire_adapter_si(W) ? 2 : 0) | 4));
    return l;
}

static const gbl_rfu_phy_t s_drv = {
    .enable = drv_enable, .disable = drv_disable, .set_role = drv_set_role,
    .stage = drv_stage, .exchange = drv_exchange, .drive_si = drv_drive_si,
    .so_high = drv_so_high, .realign = drv_realign, .sd_reset_seen = drv_sd_reset_seen,
    .wait_reset_end = drv_wait_reset_end, .lines = drv_lines,
};

/* The adapter's transfer-done interrupt: each word the GBA clocked in, in
 * order, as soon as the GBA side has finished shifting it. */
static void *done_thread(void *arg) {
    (void)arg;
    for (;;) {
        bool have = false;
        uint32_t rx = 0;
        rfu_wire_lock(W);
        if (W->ring_tail != W->ring_head) {
            rx = W->ring[W->ring_tail % RFU_WIRE_RING];
            W->ring_tail++;
            have = true;
        }
        rfu_wire_unlock(W);
        if (have) gbl_rfu_word_done(rx);
        else k_busy_wait(2);
    }
    return NULL;
}

bool sim_rfu_wire_init(void) {
    const char *path = getenv("GBLINK_RFU_WIRE");
    if (!path || !*path) return false;
    const int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof *W) != 0) {
        perror("GBLINK_RFU_WIRE");
        exit(2);
    }
    W = mmap(NULL, sizeof *W, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (W == MAP_FAILED) {
        perror("GBLINK_RFU_WIRE mmap");
        exit(2);
    }
    if (W->magic != RFU_WIRE_MAGIC) {      /* the first side up sets it out */
        memset(W, 0, sizeof *W);
        atomic_flag_clear(&W->lock);
        W->gba_so = true;                  /* SO idles high */
        W->version = RFU_WIRE_VERSION;
        W->magic = RFU_WIRE_MAGIC;
    }
    gbl_rfu_install(&s_drv);
    pthread_t t;
    pthread_create(&t, NULL, done_thread, NULL);
    pthread_detach(t);
    return true;
}
