#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline int xPortGetCoreID(void) { return 0; }
static inline BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t f, const char *n, uint32_t s, void *p,
    UBaseType_t pr, TaskHandle_t *h, BaseType_t core) {
    (void)f; (void)n; (void)s; (void)p; (void)pr; (void)h; (void)core; return pdPASS;
}
