/* sim_rfu.h -- the wireless adapter's link port in the simulator (sim_rfu.c)
 * and the librfu game that drives it (rfu_game.c). */
#ifndef GBLINK_SIM_RFU_H
#define GBLINK_SIM_RFU_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sim_gba_irq_t)(void);

/* The GBA hardware model and the adapter driver; call before gblink_start. */
void sim_rfu_init(void);

/* The game thread: route its IRQs, then claim the CPU for them. */
void sim_gba_set_irq(sim_gba_irq_t serial, int timer, sim_gba_irq_t timer_fn);
void sim_gba_attach_cpu(void);

enum {
    SIM_RFU_GBA_WORDS,      /* GBA-clocked words the adapter answered      */
    SIM_RFU_ADAPTER_WORDS,  /* adapter-clocked words the GBA took          */
    SIM_RFU_UNSTAGED,       /* the GBA clocked before the adapter staged   */
    SIM_RFU_UNARMED,        /* the adapter clocked a GBA not listening     */
    SIM_RFU_CLOCK_CLASH,    /* both ends clocking                          */
    SIM_RFU_LATE_ARM,       /* the adapter's clock waited for the GBA's
                               thread to arm (host scheduling; see
                               drv_exchange)                               */
    SIM_RFU_COUNTERS
};
uint32_t sim_rfu_counter(int which);

/* rfu_game.cpp: 'G' control ops 0x20-0x3F. */
void sim_rfu_game_init(void);
void sim_rfu_game_control(const uint8_t *data, size_t len);
#ifdef __cplusplus
}
#endif
#endif
