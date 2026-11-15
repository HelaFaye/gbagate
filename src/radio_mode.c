#include "radio_mode.h"

#include <stddef.h>
#include <string.h>

static int valid_mode(radio_mode_t mode) {
    return mode >= RADIO_MODE_OFF && mode < RADIO_MODE_COUNT;
}

static int has_ops(const radio_mode_manager_t *m, radio_mode_t mode) {
    return mode == RADIO_MODE_OFF ||
           (m->ops[mode].start != NULL && m->ops[mode].stop != NULL);
}

static void changed(radio_mode_manager_t *m, radio_mode_t mode) {
    if (m->current != mode) {
        m->current = mode;
        m->generation++;
    }
}

void radio_mode_manager_init(radio_mode_manager_t *m,
                             const radio_mode_ops_t ops[RADIO_MODE_COUNT]) {
    if (m == NULL) return;
    memset(m, 0, sizeof *m);
    m->current = RADIO_MODE_OFF;
    m->capture_return = RADIO_MODE_OFF;
    if (ops != NULL) {
        memcpy(m->ops, ops, sizeof m->ops);
    }
}

/* Caller has set transitioning. A successful stop always moves ownership to
 * OFF before another client's start callback is invoked. */
static radio_mode_result_t switch_to(radio_mode_manager_t *m,
                                     radio_mode_t target,
                                     int rollback_on_start_failure) {
    const radio_mode_t previous = m->current;
    int rc;

    m->last_driver_error = 0;
    m->last_cleanup_error = 0;
    m->last_rollback_error = 0;

    if (previous == target) return RADIO_MODE_OK;
    if (!has_ops(m, target) || (previous != RADIO_MODE_OFF && !has_ops(m, previous))) {
        return RADIO_MODE_MISSING_CALLBACK;
    }

    if (previous != RADIO_MODE_OFF) {
        rc = m->ops[previous].stop(m->ops[previous].ctx);
        if (rc != 0) {
            m->last_driver_error = rc;
            return RADIO_MODE_STOP_FAILED; /* old owner must still be considered live */
        }
        changed(m, RADIO_MODE_OFF);
    }

    if (target == RADIO_MODE_OFF) return RADIO_MODE_OK;

    rc = m->ops[target].start(m->ops[target].ctx);
    if (rc == 0) {
        changed(m, target);
        return RADIO_MODE_OK;
    }
    m->last_driver_error = rc;

    /* start() is permitted to fail after partial setup. Release it before any
     * attempt to revive the previous owner; otherwise two clients could own
     * the RF block at once. */
    int cleanup_rc = m->ops[target].stop(m->ops[target].ctx);
    if (cleanup_rc != 0) {
        m->last_cleanup_error = cleanup_rc;
        changed(m, RADIO_MODE_FAULT);
        return RADIO_MODE_CLEANUP_FAILED;
    }

    if (!rollback_on_start_failure || previous == RADIO_MODE_OFF) {
        return rollback_on_start_failure ? RADIO_MODE_START_FAILED
                                         : RADIO_MODE_RESTORE_FAILED;
    }

    rc = m->ops[previous].start(m->ops[previous].ctx);
    if (rc == 0) {
        changed(m, previous);
        return RADIO_MODE_START_FAILED; /* requested transition failed; old owner restored */
    }
    m->last_rollback_error = rc;

    cleanup_rc = m->ops[previous].stop(m->ops[previous].ctx);
    if (cleanup_rc != 0) {
        m->last_cleanup_error = cleanup_rc;
        changed(m, RADIO_MODE_FAULT);
        return RADIO_MODE_CLEANUP_FAILED;
    }
    return RADIO_MODE_ROLLBACK_FAILED; /* both clients are known to be stopped */
}

static radio_mode_result_t enter_transition(radio_mode_manager_t *m,
                                            radio_mode_t target,
                                            int rollback_on_start_failure) {
    radio_mode_result_t result;
    if (m == NULL || !valid_mode(target)) return RADIO_MODE_INVALID;
    if (m->transitioning) return RADIO_MODE_BUSY;
    if (m->current == RADIO_MODE_FAULT) return RADIO_MODE_BAD_STATE;
    m->transitioning = 1;
    result = switch_to(m, target, rollback_on_start_failure);
    m->transitioning = 0;
    return result;
}

radio_mode_result_t radio_mode_select(radio_mode_manager_t *m, radio_mode_t target) {
    if (m == NULL || !valid_mode(target) || target == RADIO_MODE_FAULT ||
        target == RADIO_MODE_COUNT || target == RADIO_MODE_SDR_RX) {
        return RADIO_MODE_INVALID;
    }
    if (m->transitioning) return RADIO_MODE_BUSY;
    if (m->current == RADIO_MODE_SDR_RX || m->capture_return_valid) return RADIO_MODE_BAD_STATE;
    return enter_transition(m, target, 1);
}

radio_mode_result_t radio_mode_capture_begin(radio_mode_manager_t *m) {
    radio_mode_result_t result;
    radio_mode_t previous;
    if (m == NULL) return RADIO_MODE_INVALID;
    if (m->transitioning) return RADIO_MODE_BUSY;
    if (m->current == RADIO_MODE_FAULT || m->current == RADIO_MODE_SDR_RX ||
        m->capture_return_valid) return RADIO_MODE_BAD_STATE;
    if (!has_ops(m, RADIO_MODE_SDR_RX)) return RADIO_MODE_MISSING_CALLBACK;

    previous = m->current;
    m->transitioning = 1;
    result = switch_to(m, RADIO_MODE_SDR_RX, 1);
    if (result == RADIO_MODE_OK) {
        m->capture_return = previous;
        m->capture_return_valid = 1;
    }
    m->transitioning = 0;
    return result;
}

radio_mode_result_t radio_mode_capture_end(radio_mode_manager_t *m) {
    radio_mode_result_t result;
    radio_mode_t restore;
    if (m == NULL) return RADIO_MODE_INVALID;
    if (m->transitioning) return RADIO_MODE_BUSY;
    if (m->current == RADIO_MODE_FAULT || m->current != RADIO_MODE_SDR_RX ||
        !m->capture_return_valid) return RADIO_MODE_BAD_STATE;

    restore = m->capture_return;
    m->transitioning = 1;
    result = switch_to(m, restore, 0); /* never roll back into SDR on restore failure */
    if (result == RADIO_MODE_OK || result == RADIO_MODE_RESTORE_FAILED ||
        result == RADIO_MODE_CLEANUP_FAILED) {
        m->capture_return = RADIO_MODE_OFF;
        m->capture_return_valid = 0;
    }
    m->transitioning = 0;
    return result;
}

radio_mode_t radio_mode_current(const radio_mode_manager_t *m) {
    return m != NULL ? m->current : RADIO_MODE_FAULT;
}

const char *radio_mode_name(radio_mode_t mode) {
    switch (mode) {
    case RADIO_MODE_OFF: return "off";
    case RADIO_MODE_ONLINE: return "online";
    case RADIO_MODE_WIRELESS: return "wireless";
    case RADIO_MODE_SDR_RX: return "sdr-rx";
    case RADIO_MODE_FAULT: return "fault";
    default: return "invalid";
    }
}

const char *radio_mode_result_name(radio_mode_result_t result) {
    switch (result) {
    case RADIO_MODE_OK: return "ok";
    case RADIO_MODE_INVALID: return "invalid";
    case RADIO_MODE_BUSY: return "busy";
    case RADIO_MODE_BAD_STATE: return "bad-state";
    case RADIO_MODE_MISSING_CALLBACK: return "missing-callback";
    case RADIO_MODE_STOP_FAILED: return "stop-failed";
    case RADIO_MODE_START_FAILED: return "start-failed; previous owner restored when applicable";
    case RADIO_MODE_ROLLBACK_FAILED: return "rollback-failed; radio is off";
    case RADIO_MODE_CLEANUP_FAILED: return "cleanup-failed; ownership unknown";
    case RADIO_MODE_RESTORE_FAILED: return "restore-failed; radio is off";
    default: return "invalid";
    }
}
