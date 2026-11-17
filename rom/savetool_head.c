/*
 * savetool_head.c -- the control block at the start of the Save Tool blob
 * (savetool.ld puts .svt_head first, at SVT_BASE).
 */
#include "savetool.h"

void svt_main(void);

__attribute__((section(".svt_head"), used))
svt_block_t svt_head = {
    .magic = SVT_MAGIC,
    .version = 1,
    .entry = (uint32_t)(uintptr_t)svt_main,   /* Thumb: the linker sets bit 0 */
    .buf = SVT_BUF,
};
