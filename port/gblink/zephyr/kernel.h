/*
 * zephyr/kernel.h -- the slice of the Zephyr kernel API that the vendored
 * GB-Link firmware (third_party/gblink-firmware) uses, implemented over
 * FreeRTOS on the ESP32-S3 and over pthreads on a host.
 *
 * Why a shim rather than a port
 * -----------------------------
 * The GB-Link application layer -- every mode, the Gen 3 packet layer, the
 * Advance Wars and e-Reader proxies -- is the part of their firmware that has
 * been proven against real games. Rewriting it to gbagate's style would throw
 * that evidence away and buy a second set of bugs. Compiling it unchanged on a
 * small kernel shim keeps their behaviour byte-for-byte, and the same shim on
 * a host turns their firmware into something a test suite can drive.
 *
 * Semantics that matter, and how they are kept
 * --------------------------------------------
 *  - Objects live in place. The firmware constructs sections (and the
 *    semaphores inside them) on the stack for every link session, so nothing
 *    here allocates: each object carries backend storage inline.
 *  - "ISR context". Upstream, the PIO callbacks run in interrupts on a single
 *    core, and irq_lock() excludes them. Here the PHY invokes every callback
 *    while holding the same lock irq_lock() takes, so the exclusion upstream
 *    relies on still holds on two cores. See port/gblink/zshim.h.
 *  - Timeouts follow Zephyr: k_sem_take returns 0, -EBUSY for K_NO_WAIT, or
 *    -EAGAIN when a finite wait expires.
 */
#ifndef GBAGATE_ZSHIM_KERNEL_H
#define GBAGATE_ZSHIM_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- time --------------------------------------------------------------- */

typedef struct {
    int64_t us; /* < 0: forever, 0: no wait */
} k_timeout_t;

#define K_FOREVER   ((k_timeout_t){ -1 })
#define K_NO_WAIT   ((k_timeout_t){ 0 })
#define K_USEC(t)   ((k_timeout_t){ (int64_t)(t) })
#define K_MSEC(t)   ((k_timeout_t){ (int64_t)(t) * 1000 })
#define K_SECONDS(t) ((k_timeout_t){ (int64_t)(t) * 1000000 })

#ifdef __cplusplus
}
/* C++ cannot use compound literals in every context the firmware does
 * (default arguments, constexpr-ish initialisers), so give it real values. */
#undef K_FOREVER
#undef K_NO_WAIT
#undef K_USEC
#undef K_MSEC
#undef K_SECONDS
static constexpr k_timeout_t zshim_timeout(int64_t us) { return k_timeout_t{ us }; }
#define K_FOREVER    zshim_timeout(-1)
#define K_NO_WAIT    zshim_timeout(0)
#define K_USEC(t)    zshim_timeout((int64_t)(t))
#define K_MSEC(t)    zshim_timeout((int64_t)(t) * 1000)
#define K_SECONDS(t) zshim_timeout((int64_t)(t) * 1000000)
extern "C" {
#endif

int32_t  k_sleep(k_timeout_t timeout);
int32_t  k_msleep(int32_t ms);
void     k_busy_wait(uint32_t usec);
void     k_yield(void);
int64_t  k_uptime_get(void);
uint32_t k_uptime_get_32(void);

/* ---- locking ------------------------------------------------------------ */

/* One global recursive lock; see the header comment. The key is unused. */
unsigned int irq_lock(void);
void         irq_unlock(unsigned int key);

typedef long atomic_t;

/* Backend storage for a blocking primitive. Sized for the larger of a
 * FreeRTOS StaticSemaphore_t and a pthread mutex+condvar pair; each backend
 * checks the size with a static assertion. */
#define ZSHIM_OBJ_WORDS 24
typedef struct {
    union {
        uint64_t  align;
        uintptr_t words[ZSHIM_OBJ_WORDS];
    } u;
} zshim_obj_t;

struct k_sem {
    zshim_obj_t obj;
    unsigned int count;
    unsigned int limit;
};

int  k_sem_init(struct k_sem *sem, unsigned int initial, unsigned int limit);
int  k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void k_sem_give(struct k_sem *sem);
void k_sem_reset(struct k_sem *sem);
unsigned int k_sem_count_get(struct k_sem *sem);

struct k_mutex {
    struct k_sem sem;
};

int k_mutex_init(struct k_mutex *m);
int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout);
int k_mutex_unlock(struct k_mutex *m);

/* ---- message queues ----------------------------------------------------- */

struct k_msgq {
    char    *buffer;
    size_t   msg_size;
    uint32_t max_msgs;
    uint32_t head;
    uint32_t used;
};

#define K_MSGQ_DEFINE(name, size, max, align)                                 \
    static char zshim_msgq_buf_##name[(size) * (max)];                         \
    struct k_msgq name = { zshim_msgq_buf_##name, (size), (max), 0, 0 }

void     k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, uint32_t max_msgs);
/* Only K_NO_WAIT is used by the firmware, and only K_NO_WAIT is supported:
 * a put on a full queue returns -ENOMSG, a get on an empty one -ENOMSG. */
int      k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int      k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);
void     k_msgq_purge(struct k_msgq *q);
uint32_t k_msgq_num_used_get(struct k_msgq *q);

/* ---- timers ------------------------------------------------------------- */

struct k_timer;
typedef void (*k_timer_expiry_t)(struct k_timer *timer);
typedef void (*k_timer_stop_t)(struct k_timer *timer);

struct k_timer {
    zshim_obj_t      obj;
    k_timer_expiry_t expiry;
    k_timer_stop_t   stop;
    void            *user_data;
    volatile uint32_t generation;
};

void  k_timer_init(struct k_timer *t, k_timer_expiry_t expiry, k_timer_stop_t stop);
/* One-shot only (period must be K_NO_WAIT), which is all the firmware uses. */
void  k_timer_start(struct k_timer *t, k_timeout_t duration, k_timeout_t period);
void  k_timer_stop(struct k_timer *t);
static inline void  k_timer_user_data_set(struct k_timer *t, void *d) { t->user_data = d; }
static inline void *k_timer_user_data_get(const struct k_timer *t) { return t->user_data; }

/* ---- threads and init --------------------------------------------------- */

typedef void (*k_thread_entry_t)(void *p1, void *p2, void *p3);

typedef struct zshim_thread_def {
    const char              *name;
    k_thread_entry_t         entry;
    size_t                   stack;
    int                      prio;
    struct zshim_thread_def *next;
} zshim_thread_def_t;

void zshim_register_thread(zshim_thread_def_t *def);

/* Static threads are registered at load time and started by
 * zshim_start_threads() (see zshim.h), not by the loader. Upstream they start
 * before main(); starting them explicitly keeps them from running on a board
 * whose GB-Link personality is switched off. */
#ifdef __cplusplus
#define ZSHIM_CTOR(fn) static void fn(void); \
    static const int fn##_reg = (fn(), 0); \
    static void fn(void)
#else
#define ZSHIM_CTOR(fn) __attribute__((constructor)) static void fn(void)
#endif

#define K_THREAD_DEFINE(name, stack_size, entry_fn, p1, p2, p3, prio, opts, delay) \
    static zshim_thread_def_t zshim_thread_##name = {                          \
        #name, (k_thread_entry_t)(entry_fn), (stack_size), (prio), NULL };     \
    ZSHIM_CTOR(zshim_thread_reg_##name) {                                      \
        zshim_register_thread(&zshim_thread_##name);                           \
    }

#define printk printf

#define ARG_UNUSED(x) ((void)(x))

#ifdef __cplusplus
}
#endif

#endif /* GBAGATE_ZSHIM_KERNEL_H */
