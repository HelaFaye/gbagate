/*
 * zshim_posix.c -- the Zephyr shim's kernel backend on a host, over pthreads.
 *
 * This is what lets the vendored GB-Link firmware run as an ordinary process:
 * its module thread, its "interrupt" callbacks (driven by sim_phy.c) and its
 * USB receive path (driven by the test) are real concurrent threads, so the
 * races the firmware guards against upstream are races here too.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <zephyr/kernel.h>
#include "zshim.h"

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             flag;
} posix_event_t;

_Static_assert(sizeof(posix_event_t) <= sizeof(zshim_obj_t),
               "zshim_obj_t too small for a pthread event");

static pthread_mutex_t s_lock;
static pthread_once_t  s_once = PTHREAD_ONCE_INIT;
static struct timespec s_boot;

static void os_init_once(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
    clock_gettime(CLOCK_MONOTONIC, &s_boot);
}

void zshim_os_init(void) { pthread_once(&s_once, os_init_once); }
void zshim_lock(void)    { zshim_os_init(); pthread_mutex_lock(&s_lock); }
void zshim_unlock(void)  { pthread_mutex_unlock(&s_lock); }

uint32_t k_cycle_get_32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec;
}

int64_t zshim_now_us(void) {
    zshim_os_init();
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)(t.tv_sec - s_boot.tv_sec) * 1000000 +
           (t.tv_nsec - s_boot.tv_nsec) / 1000;
}

void zshim_event_init(zshim_obj_t *obj) {
    posix_event_t *e = (posix_event_t *)obj;
    pthread_mutex_init(&e->m, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&e->c, &a);
    e->flag = 0;
}

bool zshim_event_wait(zshim_obj_t *obj, int64_t us) {
    posix_event_t *e = (posix_event_t *)obj;
    pthread_mutex_lock(&e->m);
    if (us < 0) {
        while (!e->flag) pthread_cond_wait(&e->c, &e->m);
    } else {
        struct timespec dl;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec  += us / 1000000;
        dl.tv_nsec += (us % 1000000) * 1000;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (!e->flag) {
            if (pthread_cond_timedwait(&e->c, &e->m, &dl) != 0) break;
        }
    }
    bool got = e->flag != 0;
    e->flag = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

void zshim_event_signal(zshim_obj_t *obj) {
    posix_event_t *e = (posix_event_t *)obj;
    pthread_mutex_lock(&e->m);
    e->flag = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}

/* ---- timers: one short-lived thread per arm; a generation counter makes a
 * cancelled or re-armed timer's old thread fall through harmlessly. ---------- */

typedef struct {
    struct k_timer *t;
    uint32_t        gen;
    int64_t         us;
} timer_job_t;

static void *timer_thread(void *arg) {
    timer_job_t job = *(timer_job_t *)arg;
    free(arg);
    struct timespec ts = { job.us / 1000000, (job.us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
    zshim_lock();
    bool live = job.t->generation == job.gen;
    zshim_unlock();
    if (live && job.t->expiry) {
        job.t->expiry(job.t);
    }
    return NULL;
}

void zshim_timer_arm(struct k_timer *t, int64_t us) {
    zshim_lock();
    uint32_t gen = ++t->generation;
    zshim_unlock();
    timer_job_t *job = malloc(sizeof *job);
    job->t = t; job->gen = gen; job->us = us;
    pthread_t th;
    pthread_create(&th, NULL, timer_thread, job);
    pthread_detach(th);
}

void zshim_timer_cancel(struct k_timer *t) {
    zshim_lock();
    t->generation++;
    zshim_unlock();
}

typedef struct { k_thread_entry_t entry; } thread_job_t;

static void *thread_main(void *arg) {
    thread_job_t job = *(thread_job_t *)arg;
    free(arg);
    job.entry(NULL, NULL, NULL);
    return NULL;
}

void zshim_thread_start(const char *name, k_thread_entry_t entry, size_t stack, int prio) {
    (void)name; (void)stack; (void)prio;
    thread_job_t *job = malloc(sizeof *job);
    job->entry = entry;
    pthread_t th;
    pthread_create(&th, NULL, thread_main, job);
    pthread_detach(th);
}

int32_t k_sleep(k_timeout_t timeout) {
    if (timeout.us < 0) {
        for (;;) pause();
    }
    struct timespec ts = { timeout.us / 1000000, (timeout.us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
    return 0;
}

void k_busy_wait(uint32_t usec) {
    int64_t end = zshim_now_us() + usec;
    while (zshim_now_us() < end) { /* spin */ }
}

void k_yield(void) { sched_yield(); }
