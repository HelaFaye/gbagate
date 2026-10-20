/* zephyr/sys/reboot.h -- routed to the port (esp_restart / test hook). */
#ifndef GBAGATE_ZSHIM_REBOOT_H
#define GBAGATE_ZSHIM_REBOOT_H
#ifdef __cplusplus
extern "C" {
#endif
#define SYS_REBOOT_WARM 0
#define SYS_REBOOT_COLD 1
void sys_reboot(int type);
#ifdef __cplusplus
}
#endif
#endif
