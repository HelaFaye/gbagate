/*
 * agent.c -- the memory agent: a multiboot program that lets the board read
 * and write the GBA's memory over the link cable.
 *
 * Send it with Send Program (or the board's Memory applet does). It sets up
 * the link as a Normal-mode 32-bit SIO slave and runs agent_proto.h's core on
 * every word the board clocks in. Nothing else: no interrupts, no BIOS
 * calls, so it can be single-stepped in tests/agent_emu.py exactly as it
 * runs on a console.
 *
 * The screen is the backdrop colour (palette entry 0, display mode 0 with no
 * layers): blue while it waits for the board, green once the board has
 * pinged it. That entry is ordinary palette RAM, so the board writing it --
 * POKE16 0x05000000 -- changes the colour on the TV: the simplest visible
 * proof that a memory edit landed.
 *
 * It occupies EWRAM from 0x02000000 to the end of its .bss, its serving loop
 * a few hundred bytes at the start of IWRAM, and the top of IWRAM for its
 * stack. PING reports the first free EWRAM byte; the core refuses writes and
 * calls into any of the three.
 */
#include <stdint.h>

#define AG_LOAD32(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define AG_STORE32(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#define AG_STORE16(a, v) (*(volatile uint16_t *)(uintptr_t)(a) = (v))
#include "agent_proto.h"

#define REG_DISPCNT    (*(volatile uint16_t *)0x04000000)
#define REG_TM3CNT_L   (*(volatile uint16_t *)0x0400010C)
#define REG_TM3CNT_H   (*(volatile uint16_t *)0x0400010E)
#define REG_SIODATA32  (*(volatile uint32_t *)0x04000120)
#define REG_SIOCNT     (*(volatile uint16_t *)0x04000128)
#define REG_RCNT       (*(volatile uint16_t *)0x04000134)
#define REG_IME        (*(volatile uint16_t *)0x04000208)
#define BG_PALETTE     ((volatile uint16_t *)0x05000000)

/* SIOCNT, Normal mode: external clock (the board's), 32-bit, start. */
#define SIO_NORMAL32   0x1000u
#define SIO_START      0x0080u

#define RGB15(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))

/* Timer 3 at 16.78 MHz / 1024: one tick is 61 us. */
#define TICKS_PER_MS   16u
#define TIMEOUT_TICKS  (AG_TIMEOUT_US / 1000u * TICKS_PER_MS)

extern uint8_t __bss_end__[];
extern uint8_t __iwram_start__[], __iwram_end__[], __iwram_lma__[];

/*
 * The serving loop runs from IWRAM. From EWRAM -- a 16-bit bus with two wait
 * states, 6 cycles an ARM instruction -- one word took up to 30 us of the
 * 36 us the board leaves between words (test/agent_emu measured it); from
 * IWRAM it is a fraction of that. Its state is on the stack, in IWRAM too.
 */
/* No long_call: a BL reaches +-32 MB, and IWRAM is 16 MB above EWRAM. */
#define IWRAM_CODE __attribute__((section(".iwram"), noinline))

/* Stage the next reply and wait for the board's clock. */
static inline void arm(uint32_t reply) {
    REG_SIOCNT = SIO_NORMAL32;          /* stop, so the data can change */
    REG_SIODATA32 = reply;
    REG_SIOCNT = SIO_NORMAL32 | SIO_START;
}

IWRAM_CODE static void serve(uint32_t image_end) {
    ag_core_t core;
    ag_core_init(&core, (image_end + 15u) & ~15u);
    ag_protect(&core, 0x02000000u, image_end);                        /* the image  */
    ag_protect(&core, (uint32_t)(uintptr_t)__iwram_start__,
               (uint32_t)(uintptr_t)__iwram_end__);                   /* this code  */
    ag_protect(&core, 0x03007E00u, 0x03008000u);                      /* the stack  */

    REG_RCNT = 0;                       /* SIO, not general-purpose pins */
    arm(AG_IDLE);

    bool greeted = false;
    uint16_t last = REG_TM3CNT_L;
    for (;;) {
        if (REG_SIOCNT & SIO_START) {
            /* Waiting. A command left half-done this long means the board
             * lost its place: drop it, and answer IDLE again. */
            if ((uint16_t)(REG_TM3CNT_L - last) > TIMEOUT_TICKS && core.state != AGS_IDLE) {
                ag_timeout(&core);
                arm(AG_IDLE);
                last = REG_TM3CNT_L;
            }
            continue;
        }
        last = REG_TM3CNT_L;
        uint32_t in = REG_SIODATA32;
        arm(ag_step(&core, in));
        if (!greeted && core.cmd == AG_PING && core.commands) {
            BG_PALETTE[0] = RGB15(0, 20, 6);   /* green: the board is here */
            greeted = true;
        }
        if (core.call_pending) {
            core.call_pending = false;
            /* The status is already staged; the board waits for the call to
             * return before it clocks again. */
            ((void (*)(void))(uintptr_t)core.call_addr)();
        }
    }
}

int main(void) {
    REG_IME = 0;
    REG_DISPCNT = 0;                    /* mode 0, no layers: the backdrop */
    BG_PALETTE[0] = RGB15(0, 4, 20);    /* blue: waiting for the board */

    /* The serving loop up to IWRAM. No .data copy is needed (multiboot.ld),
     * but this one section does travel at another address. */
    volatile uint32_t *dst = (volatile uint32_t *)__iwram_start__;
    const uint32_t *src = (const uint32_t *)__iwram_lma__;
    while ((uintptr_t)dst < (uintptr_t)__iwram_end__) {
        *dst++ = *src++;
    }

    REG_TM3CNT_H = 0;
    REG_TM3CNT_L = 0;
    REG_TM3CNT_H = 0x0083;              /* enable, prescaler 1024 */

    serve((uint32_t)(uintptr_t)__bss_end__);
    for (;;) {
    }
}
