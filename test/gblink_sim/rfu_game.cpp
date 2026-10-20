/*
 * rfu_game.cpp -- a GBA game talking to the wireless adapter, for the host
 * simulator: pret's decompiled librfu (the STWI command layer, the SIO
 * interrupt handler and the Sio32ID exchange from pokefirered) running on the
 * GBA hardware model in sim_rfu.c, on a thread of its own that takes the
 * model's IRQs as signals.
 *
 * librfu is not part of gbagate. The Makefile fetches those three files from
 * pret/pokefirered at a pinned commit (fetch_librfu.sh) and builds them, and
 * this file with SIM_LIBRFU defined, as C++ (gba_shim/global.h says why);
 * without them the ops below answer "unavailable" and the wireless adapter
 * tests skip.
 *
 * The test drives librfu as a game's link code would, one call at a time,
 * with 'G' records (op, then arguments):
 *
 *   0x20 tries       AgbRFU_SoftReset(), then AgbRFU_checkID(tries)
 *   0x21 id args...  issue REQ id through its STWI_send_* function and wait
 *                    for it to end (STWI_poll_CommandEnd)
 *   0x22             AgbRFU_SoftReset() alone
 *   0x2F             report the wire's counters
 *   0x30 gname[13] uname[8] frames u16
 *                    HOST, through librfu's game API (librfu_rfu.c) as
 *                    FireRed's link code does: rfu_initializeAPI, checkID,
 *                    reset, configure, search for a child until one joins,
 *                    then `frames` frames of UNI data each way
 *   0x31 gname[13] frames u16
 *                    JOIN: search for a parent with that game name, connect,
 *                    then the same exchange
 *
 * and gets SIM_EV_RFU events back:
 *
 *   [0, op]                          librfu not built in
 *   [1, id s32 LE]                   checkID's result (0x8001 = an adapter)
 *   [2, id, error u16, words...]     a REQ ended: the adapter's ACK as the
 *                                    game received it (header word first)
 *   [3, cmd u16, words...]           librfu's slave callback: the adapter, as
 *                                    master, delivered a command (cmd is
 *                                    reqLength << 8 | id) with these words
 *   [4, id, error u16]               librfu's master callback outside a REQ
 *                                    (0xFF: the slave-mode exchange failed)
 *   [5, u32 x SIM_RFU_COUNTERS]      the wire's counters (sim_rfu.h)
 *   [8, phase, ...]                  a HOST/JOIN scenario's progress:
 *        1 id s32                    API up, checkID's result
 *        2 slot uname[8]             (host) a child joined
 *        3 id u16 gname[13] uname[8] (join) the parent was found
 *        4 slot                      (join) connected
 *        5 frames u16 good u16 bad u16
 *                                    the exchange: frames received in order
 *                                    with the partner's marker, and not
 *        0xFF step err u16           failed at step
 */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include <atomic>

#include "sim.h"
#include "sim_rfu.h"

#ifdef SIM_LIBRFU
#include "librfu.h"
#endif

#define OPS 16

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_ops[OPS][128];
static size_t  s_op_len[OPS];
static int     s_op_head, s_op_count;

static void emit(const uint8_t *d, size_t n) { sim_event(SIM_EV_RFU, d, n); }

void sim_rfu_game_control(const uint8_t *d, size_t len) {
    if (len == 0 || len > sizeof s_ops[0]) return;
    pthread_mutex_lock(&s_mx);
    if (s_op_count < OPS) {
        const int slot = (s_op_head + s_op_count) % OPS;
        memcpy(s_ops[slot], d, len);
        s_op_len[slot] = len;
        s_op_count++;
    }
    pthread_mutex_unlock(&s_mx);
}

static bool take_op(uint8_t *out, size_t *len) {
    pthread_mutex_lock(&s_mx);
    const bool any = s_op_count > 0;
    if (any) {
        memcpy(out, s_ops[s_op_head], s_op_len[s_op_head]);
        *len = s_op_len[s_op_head];
        s_op_head = (s_op_head + 1) % OPS;
        s_op_count--;
    }
    pthread_mutex_unlock(&s_mx);
    return any;
}

static void nap_us(long us) {
    struct timespec ts = { 0, us * 1000 };
    nanosleep(&ts, NULL);
}

#ifdef SIM_LIBRFU

static struct RfuIntrStruct s_intr;
static IntrFunc s_irq_serial, s_irq_timer;

/* Callbacks run in the IRQ, i.e. in the signal handler: they only record,
 * and the game thread reports. */
#define SLAVE_EVENTS 8
static struct { uint16_t cmd; uint32_t words[0x1D]; } s_slave[SLAVE_EVENTS];
static std::atomic<int> s_slave_head, s_slave_tail;
static std::atomic<int> s_m_pending;      /* a REQ is waiting for its end */
static std::atomic<unsigned> s_m_async;  /* id << 16 | error, outside a REQ (+1) */

static void callback_m(int id, int error) {
    if (!s_m_pending.load()) s_m_async.store(((unsigned)(id & 0xFFFF) << 16 | (error & 0xFFFF)) + 1);
}

static void callback_s(u16 cmd) {
    const int tail = s_slave_tail.load();
    if ((tail + 1) % SLAVE_EVENTS == s_slave_head.load()) return;
    s_slave[tail].cmd = cmd;
    const uint32_t *rx = (const uint32_t *)gSTWIStatus->rxPacket;
    const unsigned n = ((cmd >> 8) & 0xFF) + 1;
    for (unsigned i = 0; i < n && i < 0x1D; i++) s_slave[tail].words[i] = rx[i];
    s_slave_tail.store((tail + 1) % SLAVE_EVENTS);
}

static u16 arg16(const uint8_t *a) { return (u16)(a[0] | (a[1] << 8)); }

/* Issue one REQ. Returns false for an id this harness does not send. */
static bool send_req(uint8_t id, const uint8_t *a, size_t n) {
    switch (id) {
    case ID_RESET_REQ:            STWI_send_ResetREQ(); break;
    case ID_LINK_STATUS_REQ:      STWI_send_LinkStatusREQ(); break;
    case ID_VERSION_STATUS_REQ:   STWI_send_VersionStatusREQ(); break;
    case ID_SYSTEM_STATUS_REQ:    STWI_send_SystemStatusREQ(); break;
    case ID_SLOT_STATUS_REQ:      STWI_send_SlotStatusREQ(); break;
    case ID_CONFIG_STATUS_REQ:    STWI_send_ConfigStatusREQ(); break;
    case ID_GAME_CONFIG_REQ:      if (n < 24) return false; STWI_send_GameConfigREQ(a, a + 16); break;
    case ID_SYSTEM_CONFIG_REQ:    if (n < 4) return false; STWI_send_SystemConfigREQ(arg16(a), a[2], a[3]); break;
    case ID_SC_START_REQ:         STWI_send_SC_StartREQ(); break;
    case ID_SC_POLL_REQ:          STWI_send_SC_PollingREQ(); break;
    case ID_SC_END_REQ:           STWI_send_SC_EndREQ(); break;
    case ID_SP_START_REQ:         STWI_send_SP_StartREQ(); break;
    case ID_SP_POLL_REQ:          STWI_send_SP_PollingREQ(); break;
    case ID_SP_END_REQ:           STWI_send_SP_EndREQ(); break;
    case ID_CP_START_REQ:         if (n < 2) return false; STWI_send_CP_StartREQ(arg16(a)); break;
    case ID_CP_POLL_REQ:          STWI_send_CP_PollingREQ(); break;
    case ID_CP_END_REQ:           STWI_send_CP_EndREQ(); break;
    case ID_DATA_TX_REQ:          STWI_send_DataTxREQ(a, (u8)n); break;
    case ID_DATA_TX_AND_CHANGE_REQ: STWI_send_DataTxAndChangeREQ(a, (u8)n); break;
    case ID_DATA_RX_REQ:          STWI_send_DataRxREQ(); break;
    case ID_MS_CHANGE_REQ:        STWI_send_MS_ChangeREQ(); break;
    case ID_DISCONNECT_REQ:       if (n < 1) return false; STWI_send_DisconnectREQ(a[0]); break;
    case ID_RESUME_RETRANSMIT_AND_CHANGE_REQ: STWI_send_ResumeRetransmitAndChangeREQ(); break;
    case ID_STOP_MODE_REQ:        STWI_send_StopModeREQ(); break;
    default: return false;
    }
    return true;
}

static bool scenario_host(const uint8_t *gname, const uint8_t *uname, uint16_t frames);
static bool scenario_join(const uint8_t *gname, uint16_t frames);

static void run_op(const uint8_t *op, size_t len) {
    uint8_t ev[4 + 4 * 0x1E];
    switch (op[0]) {
    case 0x20: {
        AgbRFU_SoftReset();
        const s32 id = AgbRFU_checkID(len >= 2 ? op[1] : 8);
        ev[0] = 1;
        memcpy(&ev[1], &id, 4);
        emit(ev, 5);
        break;
    }
    case 0x21: {
        if (len < 2) return;
        s_m_pending.store(1);
        if (!send_req(op[1], op + 2, len - 2)) {
            s_m_pending.store(0);
            return;
        }
        const u16 err = STWI_poll_CommandEnd();
        s_m_pending.store(0);
        const uint32_t *rx = (const uint32_t *)gSTWIStatus->rxPacket;
        const unsigned n = err ? 0 : ((rx[0] >> 8) & 0xFF) + 1;
        ev[0] = 2;
        ev[1] = op[1];
        ev[2] = (uint8_t)err;
        ev[3] = (uint8_t)(err >> 8);
        for (unsigned i = 0; i < n && i < 0x1E; i++) memcpy(&ev[4 + 4 * i], &rx[i], 4);
        emit(ev, 4 + 4 * (n < 0x1E ? n : 0x1E));
        break;
    }
    case 0x22:
        AgbRFU_SoftReset();
        break;
    case 0x30:
        if (len >= 1 + 13 + 8 + 2)
            scenario_host(op + 1, op + 14, (uint16_t)(op[22] | (op[23] << 8)));
        break;
    case 0x31:
        if (len >= 1 + 13 + 2)
            scenario_join(op + 1, (uint16_t)(op[14] | (op[15] << 8)));
        break;
    case 0x2F: {
        /* the wire's counters (sim_rfu.h), u32 LE each */
        ev[0] = 5;
        for (int i = 0; i < SIM_RFU_COUNTERS; i++) {
            const uint32_t c = sim_rfu_counter(i);
            memcpy(&ev[1 + 4 * i], &c, 4);
        }
        emit(ev, 1 + 4 * SIM_RFU_COUNTERS);
        break;
    }
    }
}

/* ---- the game API: librfu_rfu.c, as FireRed drives it ----------------------- */

static u32 s_api[0x1000];              /* rfu_initializeAPI's buffer (host-sized) */
static std::atomic<unsigned> s_req_err;  /* last REQ callback with a non-zero result */

static void req_callback(u16 cmd, u16 result) {
    if (result) s_req_err.store(((unsigned)cmd << 16) | result);
}

static void ev8(uint8_t phase, const uint8_t *d, size_t n) {
    uint8_t ev[64];
    ev[0] = 8;
    ev[1] = phase;
    if (n > sizeof ev - 2) n = sizeof ev - 2;
    if (n) memcpy(&ev[2], d, n);
    emit(ev, 2 + n);
}

static bool fail(uint8_t step, unsigned err) {
    const uint8_t d[3] = { step, (uint8_t)err, (uint8_t)(err >> 8) };
    ev8(0xFF, d, sizeof d);
    return false;
}

static void frame(void) {
    nap_us(16700);                     /* the next VBlank */
    rfu_syncVBlank();
}

/* A REQ and its end; false (reported) if librfu says it failed. */
#define REQ(step, call)                                                       \
    do {                                                                      \
        s_req_err.store(0);                                                   \
        call;                                                                 \
        const u16 e_ = rfu_waitREQComplete();                                 \
        if (e_ || s_req_err.load()) return fail(step, e_ ? e_ : s_req_err.load()); \
    } while (0)

static bool api_up(void) {
    sim_gba_set_irq(NULL, 3, NULL);
    const u16 err = rfu_initializeAPI(s_api, sizeof s_api > 0xFFFF ? 0xFFFF : sizeof s_api, &s_irq_serial, FALSE);
    if (err) return fail(1, err);
    rfu_setTimerInterrupt(3, &s_irq_timer);
    sim_gba_set_irq(s_irq_serial, 3, s_irq_timer);
    rfu_setREQCallback(req_callback);
    const s32 id = (s32)rfu_REQBN_softReset_and_checkID();
    ev8(1, (const uint8_t *)&id, 4);
    if (id != RFU_ID) return fail(2, (unsigned)id);
    REQ(3, rfu_REQ_reset());
    REQ(4, rfu_REQ_configSystem(AVAIL_SLOT4, 4, 32));
    return true;
}

/* Frames of UNI data each way. Ours: marker, then a counter; theirs is
 * checked for the partner's marker and for counting up. */
static bool exchange(uint8_t slot, bool parent, uint16_t frames) {
    static uint8_t send[14], recv[32];
    const uint8_t mine = parent ? 0xA5 : 0x5A, theirs = parent ? 0x5A : 0xA5;
    memset(send, 0, sizeof send);
    memset(recv, 0, sizeof recv);
    if (rfu_setRecvBuffer(TYPE_UNI, slot, recv, sizeof recv)) return fail(20, 0);
    if (rfu_UNI_setSendData(1 << slot, send, sizeof send)) return fail(21, 0);
    uint16_t good = 0, bad = 0, last = 0;
    for (uint16_t f = 1; f <= frames; f++) {
        REQ(22, rfu_REQ_recvData());
        if (gRfuSlotStatusUNI[slot]->recv.newDataFlag) {
            const uint16_t n = (uint16_t)(recv[1] | (recv[2] << 8));
            if (recv[0] == theirs && n > last) { good++; last = n; }
            else bad++;
            rfu_UNI_clearRecvNewDataFlag(slot);
        }
        send[0] = mine;
        send[1] = (uint8_t)f;
        send[2] = (uint8_t)(f >> 8);
        rfu_UNI_readySendData(slot);
        REQ(23, rfu_REQ_sendData(FALSE));
        frame();
    }
    const uint16_t d[3] = { frames, good, bad };
    ev8(5, (const uint8_t *)d, sizeof d);
    return true;
}

static bool scenario_host(const uint8_t *gname, const uint8_t *uname, uint16_t frames) {
    if (!api_up()) return false;
    REQ(5, rfu_REQ_configGameData(0, 0x0202, gname, uname));
    REQ(6, rfu_REQ_startSearchChild());
    int slot = -1;
    for (int t = 0; t < 600 && slot < 0; t++) {     /* ten seconds of frames */
        REQ(7, rfu_REQ_pollSearchChild());
        if (gRfuLinkStatus->connSlotFlag)
            for (int i = 0; i < RFU_CHILD_MAX; i++)
                if (gRfuLinkStatus->connSlotFlag & (1 << i)) { slot = i; break; }
        frame();
    }
    if (slot < 0) return fail(8, 0);
    REQ(9, rfu_REQ_endSearchChild());
    uint8_t d[1 + RFU_USER_NAME_LENGTH];
    d[0] = (uint8_t)slot;
    memcpy(&d[1], gRfuLinkStatus->partner[slot].uname, RFU_USER_NAME_LENGTH);
    ev8(2, d, sizeof d);
    return exchange((uint8_t)slot, true, frames);
}

static bool scenario_join(const uint8_t *gname, uint16_t frames) {
    if (!api_up()) return false;
    static const uint8_t uname[RFU_USER_NAME_LENGTH] = { 'J', 'O', 'I', 'N' };
    REQ(5, rfu_REQ_configGameData(0, 0x0202, gname, uname));
    REQ(10, rfu_REQ_startSearchParent());
    int found = -1;
    for (int t = 0; t < 600 && found < 0; t++) {
        REQ(11, rfu_REQ_pollSearchParent());
        for (int i = 0; i < gRfuLinkStatus->findParentCount; i++)
            if (!memcmp(gRfuLinkStatus->partner[i].gname, gname, RFU_GAME_NAME_LENGTH)) { found = i; break; }
        frame();
    }
    if (found < 0) return fail(12, gRfuLinkStatus->findParentCount);
    const RfuTgtData p = gRfuLinkStatus->partner[found];
    REQ(13, rfu_REQ_endSearchParent());
    uint8_t d[2 + RFU_GAME_NAME_LENGTH + RFU_USER_NAME_LENGTH];
    memcpy(d, &p.id, 2);
    memcpy(&d[2], p.gname, RFU_GAME_NAME_LENGTH);
    memcpy(&d[2 + RFU_GAME_NAME_LENGTH], p.uname, RFU_USER_NAME_LENGTH);
    ev8(3, d, sizeof d);

    REQ(14, rfu_REQ_startConnectParent(p.id));
    u8 status = 0xFF, slot = 0;
    for (int t = 0; t < 300; t++) {
        REQ(15, rfu_REQ_pollConnectParent());
        rfu_getConnectParentStatus(&status, &slot);
        if (status != 1) break;                     /* 1: still connecting */
        frame();
    }
    if (status != 0) return fail(16, status);
    REQ(17, rfu_REQ_endConnectParent());
    ev8(4, &slot, 1);
    return exchange(slot, false, frames);
}

static void report_callbacks(void) {
    while (s_slave_head.load() != s_slave_tail.load()) {
        const int h = s_slave_head.load();
        uint8_t ev[3 + 4 * 0x1D];
        const unsigned n = ((s_slave[h].cmd >> 8) & 0xFF) + 1;
        ev[0] = 3;
        ev[1] = (uint8_t)s_slave[h].cmd;
        ev[2] = (uint8_t)(s_slave[h].cmd >> 8);
        for (unsigned i = 0; i < n && i < 0x1D; i++) memcpy(&ev[3 + 4 * i], &s_slave[h].words[i], 4);
        emit(ev, 3 + 4 * (n < 0x1D ? n : 0x1D));
        s_slave_head.store((h + 1) % SLAVE_EVENTS);
    }
    const unsigned m = s_m_async.exchange(0);
    if (m) {
        const unsigned v = m - 1;
        const uint8_t ev[4] = { 4, (uint8_t)(v >> 16), (uint8_t)v, (uint8_t)(v >> 8) };
        emit(ev, sizeof ev);
    }
}

static void *game_thread(void *arg) {
    (void)arg;
    sim_gba_attach_cpu();
    REG_IME = 1;
    STWI_init_all(&s_intr, &s_irq_serial, FALSE);
    STWI_init_timer(&s_irq_timer, 3);
    sim_gba_set_irq(s_irq_serial, 3, s_irq_timer);
    STWI_set_Callback_M((void *)callback_m);
    STWI_set_Callback_S(callback_s);

    uint8_t op[128];
    size_t len;
    for (;;) {
        if (take_op(op, &len)) run_op(op, len);
        report_callbacks();
        nap_us(200);
    }
    return NULL;
}

void sim_rfu_game_init(void) {
    pthread_t t;
    pthread_create(&t, NULL, game_thread, NULL);
    pthread_detach(t);
}

#else /* no librfu */

static void *game_thread(void *arg) {
    (void)arg;
    uint8_t op[128];
    size_t len;
    for (;;) {
        if (take_op(op, &len)) {
            const uint8_t ev[2] = { 0, op[0] };
            emit(ev, sizeof ev);
        }
        nap_us(1000);
    }
    return NULL;
}

void sim_rfu_game_init(void) {
    pthread_t t;
    pthread_create(&t, NULL, game_thread, NULL);
    pthread_detach(t);
}

#endif
