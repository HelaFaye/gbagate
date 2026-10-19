/*
 * zshim_freertos.c -- the Zephyr shim's kernel backend on the ESP32-S3.
 *
 * Mapping, and why:
 *   global lock  a FreeRTOS recursive mutex, not a critical section. The
 *                callbacks run under it call k_sem_give and k_msgq_put, and no
 *                FreeRTOS API may be called inside a spinlock critical section.
 *   event        a static binary semaphore in the object's inline storage.
 *   timers       a small pool of esp_timer one-shots, keyed by the k_timer's
 *                address and a generation count, so a cancelled timer's late
 *                callback falls through instead of running.
 *   threads      FreeRTOS tasks on core 1, below the PHY: the GB-Link control
 *                thread bit-bangs the printer from inside its loop (gb.cpp),
 *                and core 0 is where WiFi and USB interrupts land.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <zephyr/kernel.h>
#include "zshim.h"

static const char *TAG = "zshim";

_Static_assert(sizeof(StaticSemaphore_t) <= sizeof(zshim_obj_t),
               "zshim_obj_t too small for a StaticSemaphore_t");

/* GB-Link threads: core 1, under the PHY task (configMAX_PRIORITIES - 1). */
#define ZSHIM_CORE      1
#define ZSHIM_PRIO_BASE (configMAX_PRIORITIES - 3)

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE      s_init_mux = portMUX_INITIALIZER_UNLOCKED;

void zshim_os_init(void) {
    if (s_lock) return;
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    portEXIT_CRITICAL(&s_init_mux);
}

void zshim_lock(void) {
    zshim_os_init();
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void zshim_unlock(void) {
    xSemaphoreGiveRecursive(s_lock);
}

int64_t zshim_now_us(void) { return esp_timer_get_time(); }

/* ---- events ---------------------------------------------------------------- */

static SemaphoreHandle_t ev(zshim_obj_t *obj) {
    return (SemaphoreHandle_t)(void *)obj; /* the handle is the static buffer */
}

void zshim_event_init(zshim_obj_t *obj) {
    memset(obj, 0, sizeof *obj);
    (void)xSemaphoreCreateBinaryStatic((StaticSemaphore_t *)(void *)obj);
}

static TickType_t us_to_ticks(int64_t us) {
    if (us < 0) return portMAX_DELAY;
    TickType_t t = (TickType_t)((us + (int64_t)portTICK_PERIOD_MS * 1000 - 1) /
                                ((int64_t)portTICK_PERIOD_MS * 1000));
    return t;
}

bool zshim_event_wait(zshim_obj_t *obj, int64_t us) {
    return xSemaphoreTake(ev(obj), us_to_ticks(us)) == pdTRUE;
}

void zshim_event_signal(zshim_obj_t *obj) {
    xSemaphoreGive(ev(obj));
}

/* ---- timers ------------------------------------------------------------------- */

#define TIMER_SLOTS 4

typedef struct {
    esp_timer_handle_t h;
    struct k_timer    *owner;
    uint32_t           gen;
} slot_t;

static slot_t s_slots[TIMER_SLOTS];

static void timer_cb(void *arg) {
    slot_t *s = arg;
    zshim_lock();
    struct k_timer *t = s->owner;
    bool live = t && t->generation == s->gen;
    zshim_unlock();
    if (live && t->expiry) t->expiry(t);
}

void zshim_timer_arm(struct k_timer *t, int64_t us) {
    zshim_lock();
    slot_t *s = NULL;
    for (int i = 0; i < TIMER_SLOTS && !s; i++) if (s_slots[i].owner == t) s = &s_slots[i];
    for (int i = 0; i < TIMER_SLOTS && !s; i++) if (!s_slots[i].owner) s = &s_slots[i];
    if (!s) {
        zshim_unlock();
        ESP_LOGE(TAG, "timer pool exhausted; k_timer_start dropped");
        return;
    }
    if (!s->h) {
        const esp_timer_create_args_t a = { .callback = timer_cb, .arg = s, .name = "ktimer" };
        if (esp_timer_create(&a, &s->h) != ESP_OK) { zshim_unlock(); return; }
    }
    esp_timer_stop(s->h); /* ignore "not running" */
    s->owner = t;
    s->gen = ++t->generation;
    esp_timer_start_once(s->h, us > 0 ? (uint64_t)us : 1);
    zshim_unlock();
}

void zshim_timer_cancel(struct k_timer *t) {
    zshim_lock();
    t->generation++;
    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (s_slots[i].owner == t) {
            esp_timer_stop(s_slots[i].h);
            s_slots[i].owner = NULL; /* slot free for the next k_timer */
        }
    }
    zshim_unlock();
}

/* ---- threads ---------------------------------------------------------------------- */

typedef struct {
    k_thread_entry_t entry;
} thread_arg_t;

static void task_main(void *arg) {
    thread_arg_t a = *(thread_arg_t *)arg;
    vPortFree(arg);
    a.entry(NULL, NULL, NULL);
    vTaskDelete(NULL);
}

void zshim_thread_start(const char *name, k_thread_entry_t entry, size_t stack, int prio) {
    /* Zephyr: lower number = more urgent. Map 0 (main) to the base and push
     * everything less urgent below it, never to idle. */
    int fr = ZSHIM_PRIO_BASE - (prio > 0 ? (prio > 4 ? 4 : prio) : 0);
    thread_arg_t *a = pvPortMalloc(sizeof *a);
    if (!a) return;
    a->entry = entry;
    /* Zephyr stack sizes are in bytes, as are ESP-IDF's; C++ with std::span
     * and the e-Reader tables needs headroom over the upstream numbers. */
    size_t bytes = stack < 3072 ? 3072 : stack;
    if (xTaskCreatePinnedToCore(task_main, name, bytes, a, fr, NULL, ZSHIM_CORE) != pdPASS) {
        vPortFree(a);
        ESP_LOGE(TAG, "could not start %s", name);
    }
}

/* ---- sleeping ------------------------------------------------------------------------ */

int32_t k_sleep(k_timeout_t timeout) {
    if (timeout.us < 0) {
        for (;;) vTaskDelay(portMAX_DELAY);
    }
    if (timeout.us == 0) {
        taskYIELD();
        return 0;
    }
    /* Sub-tick sleeps (the e-Reader's 200 us poll) spin rather than round up
     * to a whole 1 ms tick, which would slow its scan loop five-fold. */
    if (timeout.us < (int64_t)portTICK_PERIOD_MS * 1000) {
        esp_rom_delay_us((uint32_t)timeout.us);
        taskYIELD();
        return 0;
    }
    vTaskDelay(us_to_ticks(timeout.us));
    return 0;
}

void k_busy_wait(uint32_t usec) { esp_rom_delay_us(usec); }
void k_yield(void)              { taskYIELD(); }
