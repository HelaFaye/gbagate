/* rl_patterns.h -- what the test sends. tools/la_check.py reads these lists
 * from this file, so the logic analyzer's check and the firmware agree. */
#ifndef RL_PATTERNS_H
#define RL_PATTERNS_H
#include <stdint.h>

/* Multi-Player words: the longest runs (0x0000: 17 bits low; 0xF87F: 8 high
 * mid-frame), alternation, single bits at each end, and some mixed. */
static const uint16_t RL_MP_WORDS[] = {
    0x0000, 0xFFFF, 0x5555, 0xAAAA, 0xF87F, 0x0FF0, 0x8001, 0x7FFE,
    0x1234, 0xBEEF, 0x00FF, 0xFF00, 0x0001, 0x8000, 0xC3A5, 0x6D9B,
};

/* Normal-mode words: bit 31 clear, so SO falls with SC's first fall. */
static const uint32_t RL_NORMAL_WORDS[] = {
    0x12345678, 0x7FFFFFFF, 0x00000000, 0x55AA55AA, 0x0000FFFF, 0x40000001, 0x2BADF00D, 0x7E7E7E7E,
};

/* ns a bit: 256 kHz and 2 MHz */
static const uint32_t RL_NORMAL_BIT_NS[] = { 3906, 500 };

#define RL_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif
