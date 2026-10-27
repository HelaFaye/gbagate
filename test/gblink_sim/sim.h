/* sim.h -- shared between the host simulator's parts. */
#ifndef GBLINK_SIM_H
#define GBLINK_SIM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SIM_EV_VCC         = 1,  /* [5v?]                                   */
    SIM_EV_LED         = 2,  /* [r g b on]                              */
    SIM_EV_REBOOT      = 3,
    SIM_EV_BOOTLOADER  = 4,
    SIM_EV_FLASH_WRITE = 5,
    SIM_EV_PHY_MODE    = 6,  /* [gbl_phy_mode_t, sd_alt]                */
    SIM_EV_WORD        = 7,  /* [tx lo hi, rx lo hi] one link word      */
    SIM_EV_GB_BYTE     = 8,  /* [tx, rx] one GB SPI byte (+ u32 us when the
                                link mode clocks it; the printer's GB does) */
    SIM_EV_READY       = 9,  /* the firmware thread is running          */
    SIM_EV_PEER_STATUS = 10, /* [lo hi] status seen by the wireless cable */
    SIM_EV_RFU         = 11, /* the librfu game: see rfu_game.c           */
    SIM_EV_STATE       = 12  /* gblink_state(), on an 'I' record: [configured,
                              * mode i8, owner, status lo hi, u32 statuses,
                              * u32 frames x4]                              */
};

void sim_emit(char kind, uint8_t ep, const uint8_t *data, size_t len);
void sim_event(uint8_t code, const void *data, size_t len);
void sim_storage_init(const char *path);

/* sim_gba.c */
void sim_gba_init(void);
void sim_gba_control(const uint8_t *data, size_t len);
uint32_t sim_gba_words(void);   /* Multi-Play words the bus model completed */
#ifdef __cplusplus
}
#endif
#endif
