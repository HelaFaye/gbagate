/*
 * ldn_link_host.c -- GB-Link-Switch-LDN's link task (firmware/common/pico_link.c),
 * unmodified, on a host, with its UART on stdin/stdout.
 *
 *     cc -I LDN/firmware/tools/host_stubs -I LDN/firmware/common \
 *        -DLDN_PICO_LINK='"LDN/firmware/common/pico_link.c"' ldn_link_host.c -lpthread
 *     ldn_link_host RUN_MS
 *
 * LDN's own host test (firmware/tools/pico_link_test.c) runs pico_link.c
 * against a scripted adapter in simulated time. This runs it in real time
 * against whatever is on the other end of the pipes: test_ldn.py puts the
 * gbagate simulator's bridge UART there. FreeRTOS is threads, one tick is
 * one millisecond as on the boards.
 *
 * The main thread stands in for LDN's control task: it pops frames with
 * pico_link_poll_inbound and reports, on stderr, one line per frame and one
 * per state change:
 *
 *     frame CH HEX
 *     state await=N gba=N wrong=N mode_sent=N rx=N tx=N resync=N dropped=N
 *
 * LDN is AGPL-3.0. Nothing of it is in gbagate; run.sh clones it.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include LDN_PICO_LINK

/* ---- FreeRTOS, in threads -------------------------------------------- */

static struct timespec s_t0;

TickType_t xTaskGetTickCount(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (TickType_t)((t.tv_sec - s_t0.tv_sec) * 1000 + (t.tv_nsec - s_t0.tv_nsec) / 1000000);
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)(ticks ? ticks : 1) * 1000); }
void vTaskDelete(TaskHandle_t task) { (void)task; }

static pthread_t s_thread;
static void (*s_task_fn)(void *);
static void *s_task_arg;

static void *task_thread(void *unused)
{
    (void)unused;
    s_task_fn(s_task_arg);
    return NULL;
}

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *arg, int priority,
                       TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    s_task_fn = task;
    s_task_arg = arg;
    if (pthread_create(&s_thread, NULL, task_thread, NULL)) return pdFALSE;
    *handle = (TaskHandle_t)&s_thread;
    return pdTRUE;
}

struct host_queue { pthread_mutex_t lock; uint32_t length, item_size, count, head; uint8_t *items; };

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    q->length = length;
    q->item_size = item_size;
    q->items = calloc(length, item_size);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    BaseType_t ok = pdFALSE;
    pthread_mutex_lock(&q->lock);
    if (q->count < q->length)
    {
        memcpy(q->items + ((q->head + q->count++) % q->length) * q->item_size, item, q->item_size);
        ok = pdTRUE;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    BaseType_t ok = pdFALSE;
    pthread_mutex_lock(&q->lock);
    if (q->count)
    {
        memcpy(item, q->items + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->length;
        --q->count;
        ok = pdTRUE;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

/* ---- the UART: stdin in, stdout out ------------------------------------ */

int uart_driver_install(int uart, int rx, int tx, int size, void *queue, int flags)
{
    (void)uart; (void)rx; (void)tx; (void)size; (void)queue; (void)flags;
    return ESP_OK;
}
int uart_driver_delete(int uart) { (void)uart; return ESP_OK; }
int uart_param_config(int uart, const uart_config_t *config) { (void)uart; (void)config; return ESP_OK; }
int uart_set_pin(int uart, int tx, int rx, int rts, int cts) { (void)uart; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
int uart_intr_config(int uart, const uart_intr_config_t *config) { (void)uart; (void)config; return ESP_OK; }
int uart_wait_tx_done(int uart, TickType_t wait) { (void)uart; (void)wait; return ESP_OK; }
int gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return 1; }

static volatile int s_eof;

int uart_read_bytes(int uart, void *buffer, uint32_t length, TickType_t wait)
{
    (void)uart;
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (s_eof || poll(&p, 1, (int)wait) <= 0) { if (s_eof) vTaskDelay(wait); return 0; }
    ssize_t n = read(0, buffer, length);
    if (n <= 0) { s_eof = 1; return 0; }
    return (int)n;
}

static pthread_mutex_t s_out = PTHREAD_MUTEX_INITIALIZER;

int uart_write_bytes(int uart, const void *bytes, size_t length)
{
    (void)uart;
    pthread_mutex_lock(&s_out);
    size_t done = 0;
    while (done < length)
    {
        ssize_t n = write(1, (const uint8_t *)bytes + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    pthread_mutex_unlock(&s_out);
    return (int)done;
}

/* ---- the control task -------------------------------------------------- */

int main(int argc, char **argv)
{
    const TickType_t run_ms = argc > 1 ? (TickType_t)atoi(argv[1]) : 10000;
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pico_link_start();

    char last[160] = "";
    uint8_t frame[HEADER_LENGTH + PICO_LINK_MAX_PAYLOAD];
    size_t length;
    while (xTaskGetTickCount() < run_ms && !s_eof)
    {
        while (pico_link_poll_inbound(frame, sizeof(frame), &length))
        {
            fprintf(stderr, "frame %u ", frame[2]);
            for (size_t i = HEADER_LENGTH; i < length; ++i) fprintf(stderr, "%02x", frame[i]);
            fputc('\n', stderr);
        }
        uint32_t rx, tx, bytes, resync, dropped;
        pico_link_stats(&rx, &tx, &bytes, &resync, &dropped);
        char now[160];
        snprintf(now, sizeof(now), "state await=%d gba=%d wrong=%d mode_sent=%u rx=%u tx=%u resync=%u dropped=%u",
                 pico_link_await_mode(), pico_link_gba_active(), pico_link_wrong_cable(),
                 (unsigned)pico_link_mode_sent(), (unsigned)rx, (unsigned)tx, (unsigned)resync, (unsigned)dropped);
        /* rx and tx change with every frame; report the rest when they change */
        if (strcmp(strstr(now, " resync"), strstr(last, " resync") ? strstr(last, " resync") : "") ||
            strncmp(now, last, (size_t)(strstr(now, " rx=") - now)))
            fprintf(stderr, "%s\n", now);
        strcpy(last, now);
        vTaskDelay(5);
    }
    pico_link_stop();
    uint32_t rx, tx, bytes, resync, dropped;
    pico_link_stats(&rx, &tx, &bytes, &resync, &dropped);
    fprintf(stderr, "end await=%d gba=%d wrong=%d mode_sent=%u rx=%u tx=%u resync=%u dropped=%u\n",
            pico_link_await_mode(), pico_link_gba_active(), pico_link_wrong_cable(),
            (unsigned)pico_link_mode_sent(), (unsigned)rx, (unsigned)tx, (unsigned)resync, (unsigned)dropped);
    return 0;
}
