#pragma once
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz) { (void)n; (void)sz; return (QueueHandle_t)1; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *i, TickType_t t) { (void)q; (void)i; (void)t; return pdTRUE; }
static inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *i, BaseType_t *w) { (void)q; (void)i; (void)w; return pdTRUE; }
static inline BaseType_t xQueueOverwrite(QueueHandle_t q, const void *i) { (void)q; (void)i; return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *i, TickType_t t) { (void)q; (void)i; (void)t; return pdFALSE; }
