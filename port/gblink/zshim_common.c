/*
 * zshim_common.c -- the backend-independent half of the Zephyr shim:
 * semaphores, mutexes, message queues, ring buffers, timers' bookkeeping,
 * flash map, and the init/thread registries.
 *
 * Every blocking primitive is a counter under the global lock plus one event
 * to sleep on, which is all the vendored code needs and keeps the two backends
 * down to a lock, an event and a timer.
 */
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/ring_buffer.h>

#include "zshim.h"

/* ---- locking ------------------------------------------------------------ */

unsigned int irq_lock(void) {
    zshim_os_init();
    zshim_lock();
    return 0;
}

void irq_unlock(unsigned int key) {
    (void)key;
    zshim_unlock();
}

/* ---- semaphores ----------------------------------------------------------- */

int k_sem_init(struct k_sem *sem, unsigned int initial, unsigned int limit) {
    zshim_os_init();
    if (limit == 0 || initial > limit) {
        return -EINVAL;
    }
    zshim_event_init(&sem->obj);
    sem->count = initial;
    sem->limit = limit;
    return 0;
}

int k_sem_take(struct k_sem *sem, k_timeout_t timeout) {
    int64_t deadline = timeout.us > 0 ? zshim_now_us() + timeout.us : 0;
    for (;;) {
        zshim_lock();
        if (sem->count > 0) {
            sem->count--;
            zshim_unlock();
            return 0;
        }
        zshim_unlock();

        if (timeout.us == 0) {
            return -EBUSY;
        }
        int64_t wait = -1;
        if (timeout.us > 0) {
            wait = deadline - zshim_now_us();
            if (wait <= 0) {
                return -EAGAIN;
            }
        }
        /* A give between the unlock above and this wait is not lost: the
         * event latches until consumed, and the loop re-checks the count. */
        zshim_event_wait(&sem->obj, wait);
    }
}

void k_sem_give(struct k_sem *sem) {
    zshim_lock();
    if (sem->count < sem->limit) {
        sem->count++;
    }
    zshim_unlock();
    zshim_event_signal(&sem->obj);
}

void k_sem_reset(struct k_sem *sem) {
    zshim_lock();
    sem->count = 0;
    zshim_unlock();
}

unsigned int k_sem_count_get(struct k_sem *sem) {
    zshim_lock();
    unsigned int c = sem->count;
    zshim_unlock();
    return c;
}

/* ---- mutexes -------------------------------------------------------------- */

/* Only used by the (replaced) serial layer; not recursive, which is fine for
 * that and is checked by nothing. */
int k_mutex_init(struct k_mutex *m) { return k_sem_init(&m->sem, 1, 1); }
int k_mutex_lock(struct k_mutex *m, k_timeout_t t) { return k_sem_take(&m->sem, t); }
int k_mutex_unlock(struct k_mutex *m) { k_sem_give(&m->sem); return 0; }

/* ---- message queues ------------------------------------------------------- */

void k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, uint32_t max_msgs) {
    zshim_os_init();
    q->buffer   = buffer;
    q->msg_size = msg_size;
    q->max_msgs = max_msgs;
    q->head     = 0;
    q->used     = 0;
}

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout) {
    (void)timeout; /* see kernel.h: K_NO_WAIT only */
    zshim_os_init();
    zshim_lock();
    if (q->used >= q->max_msgs) {
        zshim_unlock();
        return -ENOMSG;
    }
    uint32_t slot = (q->head + q->used) % q->max_msgs;
    memcpy(q->buffer + (size_t)slot * q->msg_size, data, q->msg_size);
    q->used++;
    zshim_unlock();
    return 0;
}

int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout) {
    (void)timeout;
    zshim_os_init();
    zshim_lock();
    if (q->used == 0) {
        zshim_unlock();
        return -ENOMSG;
    }
    memcpy(data, q->buffer + (size_t)q->head * q->msg_size, q->msg_size);
    q->head = (q->head + 1) % q->max_msgs;
    q->used--;
    zshim_unlock();
    return 0;
}

void k_msgq_purge(struct k_msgq *q) {
    zshim_os_init();
    zshim_lock();
    q->head = 0;
    q->used = 0;
    zshim_unlock();
}

uint32_t k_msgq_num_used_get(struct k_msgq *q) {
    zshim_os_init();
    zshim_lock();
    uint32_t n = q->used;
    zshim_unlock();
    return n;
}

/* ---- timers ---------------------------------------------------------------- */

void k_timer_init(struct k_timer *t, k_timer_expiry_t expiry, k_timer_stop_t stop) {
    zshim_os_init();
    memset(&t->obj, 0, sizeof t->obj);
    t->expiry     = expiry;
    t->stop       = stop;
    t->user_data  = NULL;
    t->generation = 0;
}

void k_timer_start(struct k_timer *t, k_timeout_t duration, k_timeout_t period) {
    (void)period;
    zshim_timer_arm(t, duration.us < 0 ? 0 : duration.us);
}

void k_timer_stop(struct k_timer *t) {
    zshim_timer_cancel(t);
    if (t->stop) {
        t->stop(t);
    }
}

/* ---- ring buffers ----------------------------------------------------------- */

void ring_buf_init(struct ring_buf *rb, uint32_t size, uint8_t *data) {
    rb->buffer = data;
    rb->size   = size;
    rb->head   = 0;
    rb->tail   = 0;
    rb->claim  = 0;
}

void ring_buf_reset(struct ring_buf *rb) {
    rb->head  = 0;
    rb->tail  = 0;
    rb->claim = 0;
}

uint32_t ring_buf_size_get(struct ring_buf *rb)     { return rb->head - rb->tail; }
uint32_t ring_buf_capacity_get(struct ring_buf *rb) { return rb->size; }
uint32_t ring_buf_space_get(struct ring_buf *rb)    { return rb->size - (rb->head - rb->tail); }

uint32_t ring_buf_put(struct ring_buf *rb, const uint8_t *data, uint32_t size) {
    uint32_t space = ring_buf_space_get(rb);
    if (size > space) {
        size = space;
    }
    for (uint32_t i = 0; i < size; i++) {
        rb->buffer[(rb->head + i) % rb->size] = data[i];
    }
    rb->head += size;
    return size;
}

uint32_t ring_buf_peek(struct ring_buf *rb, uint8_t *data, uint32_t size) {
    uint32_t avail = ring_buf_size_get(rb);
    if (size > avail) {
        size = avail;
    }
    for (uint32_t i = 0; i < size; i++) {
        data[i] = rb->buffer[(rb->tail + i) % rb->size];
    }
    return size;
}

uint32_t ring_buf_get(struct ring_buf *rb, uint8_t *data, uint32_t size) {
    uint32_t avail = ring_buf_size_get(rb);
    if (size > avail) {
        size = avail;
    }
    if (data) {
        for (uint32_t i = 0; i < size; i++) {
            data[i] = rb->buffer[(rb->tail + i) % rb->size];
        }
    }
    rb->tail += size;
    return size;
}

/* Contiguous run from the tail, capped at the wrap point, as Zephyr's does. */
uint32_t ring_buf_get_claim(struct ring_buf *rb, uint8_t **data, uint32_t size) {
    uint32_t avail = ring_buf_size_get(rb) - rb->claim;
    uint32_t start = (rb->tail + rb->claim) % rb->size;
    uint32_t to_wrap = rb->size - start;
    if (size > avail) size = avail;
    if (size > to_wrap) size = to_wrap;
    *data = &rb->buffer[start];
    rb->claim += size;
    return size;
}

int ring_buf_get_finish(struct ring_buf *rb, uint32_t size) {
    if (size > rb->claim) {
        return -EINVAL;
    }
    rb->tail += size;
    rb->claim = 0;
    return 0;
}

/* ---- flash map ---------------------------------------------------------------- */

static const struct flash_area s_storage = { 0, ZSHIM_STORAGE_SIZE };

int flash_area_open(uint8_t id, const struct flash_area **fa) {
    if (id != 0) {
        return -ENOENT;
    }
    *fa = &s_storage;
    return 0;
}

void flash_area_close(const struct flash_area *fa) { (void)fa; }

static bool in_range(off_t off, size_t len) {
    return off >= 0 && (size_t)off <= ZSHIM_STORAGE_SIZE &&
           len <= ZSHIM_STORAGE_SIZE - (size_t)off;
}

int flash_area_read(const struct flash_area *fa, off_t off, void *dst, size_t len) {
    (void)fa;
    return in_range(off, len) ? zshim_storage_read((size_t)off, dst, len) : -EINVAL;
}

int flash_area_write(const struct flash_area *fa, off_t off, const void *src, size_t len) {
    (void)fa;
    return in_range(off, len) ? zshim_storage_write((size_t)off, src, len) : -EINVAL;
}

int flash_area_erase(const struct flash_area *fa, off_t off, size_t len) {
    (void)fa;
    return in_range(off, len) ? zshim_storage_erase((size_t)off, len) : -EINVAL;
}

/* ---- registries ------------------------------------------------------------------ */

static zshim_init_def_t   *s_inits;
static zshim_thread_def_t *s_threads;

void zshim_register_init(zshim_init_def_t *def) {
    def->next = s_inits;
    s_inits = def;
}

void zshim_register_thread(zshim_thread_def_t *def) {
    def->next = s_threads;
    s_threads = def;
}

void zshim_run_inits(void) {
    zshim_os_init();
    for (int level = PRE_KERNEL_1; level <= APPLICATION; level++) {
        for (int prio = 0; prio <= 99; prio++) {
            for (zshim_init_def_t *d = s_inits; d; d = d->next) {
                if (d->level == level && d->prio == prio) {
                    d->fn();
                }
            }
        }
    }
}

void zshim_start_threads(void) {
    zshim_os_init();
    for (zshim_thread_def_t *d = s_threads; d; d = d->next) {
        zshim_thread_start(d->name, d->entry, d->stack, d->prio);
    }
}

int32_t k_msleep(int32_t ms) {
    return k_sleep(K_MSEC(ms));
}

int64_t  k_uptime_get(void)    { return zshim_now_us() / 1000; }
uint32_t k_uptime_get_32(void) { return (uint32_t)(zshim_now_us() / 1000); }
