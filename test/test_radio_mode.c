#include "radio_mode.h"

#include <stdio.h>
#include <string.h>

static int failures;
static char logbuf[128];
static size_t loglen;

typedef struct {
    char tag;
    int start_error;
    int stop_error;
    radio_mode_manager_t *reenter;
    radio_mode_result_t reenter_result;
} fake_t;

static void record(char c) {
    if (loglen + 1 < sizeof logbuf) {
        logbuf[loglen++] = c;
        logbuf[loglen] = '\0';
    }
}

static int fake_start(void *ctx) {
    fake_t *f = ctx;
    record((char)(f->tag + 'a' - 'A'));
    if (f->reenter != NULL) {
        f->reenter_result = radio_mode_select(f->reenter, RADIO_MODE_OFF);
    }
    return f->start_error;
}

static int fake_stop(void *ctx) {
    fake_t *f = ctx;
    record(f->tag);
    return f->stop_error;
}

static void check(const char *name, int condition) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) failures++;
}

static void reset_log(void) {
    logbuf[0] = '\0';
    loglen = 0;
}

static void setup(radio_mode_manager_t *m, fake_t *online, fake_t *wireless, fake_t *sdr) {
    radio_mode_ops_t ops[RADIO_MODE_COUNT] = { 0 };
    *online = (fake_t){ .tag = 'O' };
    *wireless = (fake_t){ .tag = 'W' };
    *sdr = (fake_t){ .tag = 'S' };
    ops[RADIO_MODE_ONLINE] = (radio_mode_ops_t){ fake_start, fake_stop, online };
    ops[RADIO_MODE_WIRELESS] = (radio_mode_ops_t){ fake_start, fake_stop, wireless };
    ops[RADIO_MODE_SDR_RX] = (radio_mode_ops_t){ fake_start, fake_stop, sdr };
    radio_mode_manager_init(m, ops);
    reset_log();
}

static void test_exclusive_transitions(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    check("OFF -> ONLINE", radio_mode_select(&m, RADIO_MODE_ONLINE) == RADIO_MODE_OK &&
          radio_mode_current(&m) == RADIO_MODE_ONLINE);
    check("ONLINE -> WIRELESS stops before starting", radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_OK &&
          strcmp(logbuf, "oOw") == 0 && radio_mode_current(&m) == RADIO_MODE_WIRELESS);
    check("same-owner selection is a no-op", radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_OK &&
          strcmp(logbuf, "oOw") == 0);
    check("WIRELESS -> OFF", radio_mode_select(&m, RADIO_MODE_OFF) == RADIO_MODE_OK &&
          radio_mode_current(&m) == RADIO_MODE_OFF && strcmp(logbuf, "oOwW") == 0);
}

static void test_start_failure_restores_previous(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    reset_log();
    w.start_error = 17;
    check("failed target is cleaned up and previous owner restored",
          radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_START_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_ONLINE && strcmp(logbuf, "OwWo") == 0 &&
          m.last_driver_error == 17);
}

static void test_stop_failure_keeps_old_owner(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    reset_log();
    o.stop_error = 9;
    check("failed stop does not start the next owner",
          radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_STOP_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_ONLINE && strcmp(logbuf, "O") == 0 &&
          m.last_driver_error == 9);
}

static void test_rollback_failure_leaves_off(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    reset_log();
    w.start_error = 17;
    o.start_error = 18;
    check("failed start and failed rollback leave known-safe OFF",
          radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_ROLLBACK_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_OFF && strcmp(logbuf, "OwWoO") == 0 &&
          m.last_driver_error == 17 && m.last_rollback_error == 18);
}

static void test_cleanup_failure_faults_manager(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    reset_log();
    w.start_error = 17;
    w.stop_error = 19;
    check("failed partial-start cleanup enters FAULT and blocks new owners",
          radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_CLEANUP_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_FAULT &&
          radio_mode_select(&m, RADIO_MODE_ONLINE) == RADIO_MODE_BAD_STATE &&
          m.last_driver_error == 17 && m.last_cleanup_error == 19);
}

static void test_capture_restores_previous_owner(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    reset_log();
    check("capture begins after Online is stopped",
          radio_mode_capture_begin(&m) == RADIO_MODE_OK &&
          radio_mode_current(&m) == RADIO_MODE_SDR_RX && strcmp(logbuf, "Os") == 0 &&
          m.capture_return_valid && m.capture_return == RADIO_MODE_ONLINE);
    check("ordinary switch cannot steal capture ownership",
          radio_mode_select(&m, RADIO_MODE_WIRELESS) == RADIO_MODE_BAD_STATE);
    check("capture end restores the exact prior owner",
          radio_mode_capture_end(&m) == RADIO_MODE_OK &&
          radio_mode_current(&m) == RADIO_MODE_ONLINE && !m.capture_return_valid &&
          strcmp(logbuf, "OsSo") == 0);
}

static void test_capture_from_off_and_restore_failure(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    check("capture from OFF returns to OFF", radio_mode_capture_begin(&m) == RADIO_MODE_OK &&
          radio_mode_capture_end(&m) == RADIO_MODE_OK && radio_mode_current(&m) == RADIO_MODE_OFF &&
          strcmp(logbuf, "sS") == 0);
    reset_log();
    (void)radio_mode_select(&m, RADIO_MODE_ONLINE);
    (void)radio_mode_capture_begin(&m);
    reset_log();
    o.start_error = 23;
    check("restore failure does not silently restart SDR", radio_mode_capture_end(&m) == RADIO_MODE_RESTORE_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_OFF && !m.capture_return_valid && strcmp(logbuf, "SoO") == 0);
}

static void test_capture_stop_can_be_retried(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    (void)radio_mode_select(&m, RADIO_MODE_WIRELESS);
    (void)radio_mode_capture_begin(&m);
    reset_log();
    s.stop_error = 31;
    check("failed capture stop retains capture ownership and return target",
          radio_mode_capture_end(&m) == RADIO_MODE_STOP_FAILED &&
          radio_mode_current(&m) == RADIO_MODE_SDR_RX && m.capture_return_valid &&
          m.capture_return == RADIO_MODE_WIRELESS);
    s.stop_error = 0;
    reset_log();
    check("capture end can be retried after stop failure",
          radio_mode_capture_end(&m) == RADIO_MODE_OK &&
          radio_mode_current(&m) == RADIO_MODE_WIRELESS && strcmp(logbuf, "Sw") == 0);
}


static void test_invalid_and_missing_modes(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    check("SDR cannot be selected through the ordinary mode API",
          radio_mode_select(&m, RADIO_MODE_SDR_RX) == RADIO_MODE_INVALID &&
          radio_mode_current(&m) == RADIO_MODE_OFF);
    check("invalid enum values are rejected",
          radio_mode_select(&m, (radio_mode_t)99) == RADIO_MODE_INVALID);
    memset(&m.ops[RADIO_MODE_SDR_RX], 0, sizeof m.ops[RADIO_MODE_SDR_RX]);
    check("capture is refused until an SDR client registers both callbacks",
          radio_mode_capture_begin(&m) == RADIO_MODE_MISSING_CALLBACK &&
          radio_mode_current(&m) == RADIO_MODE_OFF);
}

static void test_reentrancy_is_rejected(void) {
    radio_mode_manager_t m;
    fake_t o, w, s;
    setup(&m, &o, &w, &s);
    o.reenter = &m;
    check("reentrant transition is rejected while callback is active",
          radio_mode_select(&m, RADIO_MODE_ONLINE) == RADIO_MODE_OK &&
          o.reenter_result == RADIO_MODE_BUSY && radio_mode_current(&m) == RADIO_MODE_ONLINE);
}

int main(void) {
    test_exclusive_transitions();
    test_start_failure_restores_previous();
    test_stop_failure_keeps_old_owner();
    test_rollback_failure_leaves_off();
    test_cleanup_failure_faults_manager();
    test_capture_restores_previous_owner();
    test_capture_from_off_and_restore_failure();
    test_capture_stop_can_be_retried();
    test_invalid_and_missing_modes();
    test_reentrancy_is_rejected();
    printf("\n%s (%d failure(s))\n", failures ? "FAILURES" : "all checks passed", failures);
    return failures != 0;
}
