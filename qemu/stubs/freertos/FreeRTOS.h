#pragma once
#include <stdint.h>
#include <stddef.h>
/* ESP-IDF's FreeRTOS.h pulls this in transitively, so sources legitimately use
 * IRAM_ATTR without including esp_attr.h themselves. The stub matches that
 * rather than forcing a source change to satisfy the stub. */
#include "esp_attr.h"
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define configMAX_PRIORITIES 25
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(x) do { (void)(x); } while (0)
#define portYIELD_FROM_ISR(x) do { (void)(x); } while (0)

/* Spinlock critical sections (the PHY's parent transfers). */
typedef struct { int owner; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL(m)  do { (void)(m); } while (0)
