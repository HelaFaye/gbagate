/*
 * zshim.h -- what the Zephyr shim needs from whoever hosts it.
 *
 * Two hosts exist: the ESP32-S3 firmware (zshim_freertos.c plus the
 * port/esp32s3/gblink_*.c drivers) and the host test harness
 * (test/gblink_sim/). Everything the vendored GB-Link code can reach that is
 * not plain kernel bookkeeping comes through here, so this header is also the
 * complete list of what a new board has to supply.
 */
#ifndef GBAGATE_ZSHIM_H
#define GBAGATE_ZSHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- kernel backend (zshim_freertos.c / zshim_posix.c) ------------------- */

/* Bring up the global lock. Idempotent; every shim entry point calls it. */
void zshim_os_init(void);

/* The lock irq_lock() takes. Recursive. The PHY takes it around every
 * callback into the vendored code, so "callbacks are atomic with respect to
 * irq_lock() sections" holds on two cores as it does on the RP2040's one. */
void zshim_lock(void);
void zshim_unlock(void);

/* Monotonic microseconds since boot. k_uptime_get() and every timeout are
 * derived from it. */
int64_t zshim_now_us(void);

/* Binary-signal primitive the semaphore is built from: wait until signalled
 * or the timeout passes (us < 0 forever). Returns true if signalled. */
void zshim_event_init(zshim_obj_t *obj);
bool zshim_event_wait(zshim_obj_t *obj, int64_t us);
void zshim_event_signal(zshim_obj_t *obj);

/* One-shot timer driving k_timer. */
void zshim_timer_arm(struct k_timer *t, int64_t us);
void zshim_timer_cancel(struct k_timer *t);

/* Start a thread. prio is Zephyr's (lower is more urgent, may be negative). */
void zshim_thread_start(const char *name, k_thread_entry_t entry, size_t stack, int prio);

/* ---- lifecycle (zshim_common.c) ----------------------------------------- */

/* Run every SYS_INIT hook, then start every K_THREAD_DEFINE thread. Call
 * once, before the GB-Link control thread starts. */
void zshim_run_inits(void);
void zshim_start_threads(void);

/* ---- persistent storage (one 4 KB partition) ----------------------------- */

/* Read len bytes at off. Bytes never written read as 0xFF. */
int zshim_storage_read(size_t off, void *dst, size_t len);
/* Write len bytes at off, persisting before returning. */
int zshim_storage_write(size_t off, const void *src, size_t len);
/* Reset [off, off+len) to 0xFF. */
int zshim_storage_erase(size_t off, size_t len);

/* ---- link-port signals for the Pico SDK GPIO calls in gb.cpp ------------- */

/*
 * Named after the RP2040 GPIO numbers the vendored code uses for them, so the
 * Pico SDK compat header can pass the numbers straight through.
 */
typedef enum {
    GBLINK_SIG_SC     = 0,
    GBLINK_SIG_SI     = 1,
    GBLINK_SIG_SO     = 2,
    GBLINK_SIG_SD     = 3,  /* SD as a GBA cable carries it          */
    GBLINK_SIG_SD_ALT = 4   /* SD as GB-Link wires a GBC cable's pin  */
} gblink_signal_t;

void gblink_pin_init(gblink_signal_t sig);
void gblink_pin_dir(gblink_signal_t sig, bool output);
void gblink_pin_put(gblink_signal_t sig, bool high);
bool gblink_pin_get(gblink_signal_t sig);
void gblink_pin_pull(gblink_signal_t sig, bool up);

#ifdef __cplusplus
}
#endif

#endif /* GBAGATE_ZSHIM_H */
