#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL (1<<11)
#define MALLOC_CAP_SPIRAM   (1<<10)
#define MALLOC_CAP_DMA      (1<<3)
#define MALLOC_CAP_8BIT     (1<<2)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, uint32_t caps) { (void)caps; return calloc(n, s); }
static inline void heap_caps_free(void *p) { free(p); }
