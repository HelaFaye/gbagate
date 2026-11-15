/*
 * radio_mode.h -- exclusive ownership contract for the ESP32-S3 radio.
 *
 * This module is platform-neutral. ESP-IDF clients provide the start/stop
 * callbacks; the manager enforces ordering, rollback, and capture restoration.
 * Callers must serialize access (the intended firmware caller is the app/UI
 * task on core 0). It deliberately has no FreeRTOS dependency.
 */
#ifndef RADIO_MODE_H
#define RADIO_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_MODE_OFF = 0,
    RADIO_MODE_ONLINE,
    RADIO_MODE_WIRELESS,
    RADIO_MODE_SDR_RX,
    /* A callback failed while releasing a partially started client. The
     * physical owner is unknown; do not start another client until reboot or
     * an explicit platform recovery has made the radio safe. */
    RADIO_MODE_FAULT,
    RADIO_MODE_COUNT
} radio_mode_t;

typedef enum {
    RADIO_MODE_OK = 0,
    RADIO_MODE_INVALID,
    RADIO_MODE_BUSY,
    RADIO_MODE_BAD_STATE,
    RADIO_MODE_MISSING_CALLBACK,
    RADIO_MODE_STOP_FAILED,
    RADIO_MODE_START_FAILED,
    RADIO_MODE_ROLLBACK_FAILED,
    RADIO_MODE_CLEANUP_FAILED,
    RADIO_MODE_RESTORE_FAILED
} radio_mode_result_t;

typedef int (*radio_mode_action_fn)(void *ctx);

typedef struct {
    /* Return 0 on success. Start and stop must be idempotent. A failed start
     * may have partially acquired resources, so stop must clean up a partial
     * start too. A nonzero stop result must mean the owner is still intact;
     * a callback must not report failure after partially relinquishing the RF
     * block unless it has internally restored its ownership before returning. */
    radio_mode_action_fn start;
    radio_mode_action_fn stop;
    void *ctx;
} radio_mode_ops_t;

typedef struct {
    radio_mode_ops_t ops[RADIO_MODE_COUNT];
    radio_mode_t current;
    radio_mode_t capture_return;
    unsigned long generation;
    int last_driver_error;
    int last_cleanup_error;
    int last_rollback_error;
    unsigned char capture_return_valid;
    unsigned char transitioning;
} radio_mode_manager_t;

/* Initializes an OFF manager and copies the callback table. OFF needs no
 * callbacks. Every selectable client requires both start and stop callbacks. */
void radio_mode_manager_init(radio_mode_manager_t *manager,
                             const radio_mode_ops_t ops[RADIO_MODE_COUNT]);

/* Select OFF, ONLINE, or WIRELESS. SDR_RX is entered only with
 * radio_mode_capture_begin() and left only with radio_mode_capture_end().
 * Re-selecting the current mode is a successful no-op. */
radio_mode_result_t radio_mode_select(radio_mode_manager_t *manager,
                                      radio_mode_t target);

/* Temporarily takes ownership for finite SDR receive capture. The previous
 * owner (including OFF) is remembered and restored by capture_end(). */
radio_mode_result_t radio_mode_capture_begin(radio_mode_manager_t *manager);
radio_mode_result_t radio_mode_capture_end(radio_mode_manager_t *manager);

radio_mode_t radio_mode_current(const radio_mode_manager_t *manager);
const char *radio_mode_name(radio_mode_t mode);
const char *radio_mode_result_name(radio_mode_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_MODE_H */
