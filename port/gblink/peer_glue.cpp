// peer_glue.cpp -- runs the wireless link cable (src/gbl_peer.c) against the
// local GB-Link firmware.
//
// Everything gbl_peer does happens on one thread. The firmware's replies
// (through Transport's local sink, from its module thread and PHY callbacks)
// and the radio's datagrams (from the radio driver's callback) are queued, and
// the thread drains the queue, ticks the state machine, and makes the
// injected calls into the firmware -- which are then made from a single
// thread, as USB makes them, and never from inside one of the firmware's own
// callbacks.
//
// Starting and stopping are requests to that thread too: gblink_peer_start()
// and gblink_peer_stop() record what is wanted and wait for the thread to
// carry it out. Done on the caller's thread they would race a tick or a
// set_mode() already under way, and could leave the firmware in a link mode
// with no one driving it.
#include <atomic>
#include <cstring>

#include <zephyr/kernel.h>

#include "layers/transport.hpp"

extern "C" {
#include "gbl_peer.h"
#include "gblink_port.h"
#include "zshim.h"
}

namespace
{
    enum : uint8_t { EV_STATUS = 1, EV_DATA = 2, EV_RADIO = 3 };

    struct Event
    {
        uint8_t kind;
        uint8_t len;
        bool hasSrc;
        uint8_t src[6];
        uint8_t data[GBL_PEER_MAX_DATAGRAM];
    };

    K_MSGQ_DEFINE(g_events, sizeof(Event), 96, 4);

    // What the callers want, and what the thread has done about it. A request
    // is a new generation; the thread applies the newest it finds.
    struct Wanted
    {
        bool run;
        uint8_t mode, variant;
        uint32_t nonce;
    };
    Wanted g_wanted;                       // under irq_lock
    std::atomic<uint32_t> g_wantedGen{0};
    std::atomic<uint32_t> g_doneGen{0};

    gbl_peer_t g_peer;                     // the thread's alone
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_threadStarted{false};
    struct k_sem g_wake;

    void post(uint8_t kind, const uint8_t* d, size_t n, const uint8_t* src = nullptr)
    {
        Event e{};
        e.kind = kind;
        e.len = static_cast<uint8_t>(n > sizeof e.data ? sizeof e.data : n);
        std::memcpy(e.data, d, e.len);
        if (src) { e.hasSrc = true; std::memcpy(e.src, src, 6); }
        k_msgq_put(&g_events, &e, K_NO_WAIT);   // a full queue drops, like a radio
        k_sem_give(&g_wake);
    }

    void localSink(bool status, std::span<const uint8_t> d, void*)
    {
        if (!g_running) return;
        if (status && d.size() == 2) gblink_peer_observe_status(static_cast<uint16_t>(d[0] | (d[1] << 8)));
        post(status ? EV_STATUS : EV_DATA, d.data(), d.size());
    }

    void injectCmd(void*, const uint8_t* c, size_t n) { Transport::injectCommand({ c, n }); }
    void injectData(void*, const uint8_t* d, size_t n) { Transport::injectData({ d, n }); }
    bool radioTx(void*, const uint8_t* d, size_t n) { return gblink_peer_radio_send(d, n); }

    // On the thread: bring the cable to what was last asked for.
    void apply()
    {
        const uint32_t gen = g_wantedGen.load();
        if (gen == g_doneGen.load()) return;
        const unsigned key = irq_lock();
        const Wanted w = g_wanted;
        irq_unlock(key);

        if (g_running)
        {
            g_running = false;
            gbl_peer_stop(&g_peer);                 // cancels the local mode
            Transport::setLocalSink(nullptr, nullptr);
        }
        k_msgq_purge(&g_events);
        if (w.run)
        {
            const gbl_peer_io_t io = { injectCmd, injectData, radioTx, nullptr };
            gbl_peer_init(&g_peer, &io, w.nonce, w.mode, w.variant);
            Transport::setLocalSink(localSink, nullptr);
            g_running = true;
            gbl_peer_start(&g_peer, k_uptime_get_32());
        }
        g_doneGen = gen;
    }

    void peerThread(void*, void*, void*)
    {
        for (;;)
        {
            k_sem_take(&g_wake, K_MSEC(5));
            apply();
            const uint32_t now = k_uptime_get_32();
            Event e;
            while (k_msgq_get(&g_events, &e, K_NO_WAIT) == 0)
            {
                if (!g_running) continue;
                if (e.kind == EV_STATUS && e.len == 2)
                    gbl_peer_on_status(&g_peer, static_cast<uint16_t>(e.data[0] | (e.data[1] << 8)), now);
                else if (e.kind == EV_DATA)
                    gbl_peer_on_data(&g_peer, e.data, e.len, now);
                else if (e.kind == EV_RADIO && gbl_peer_on_radio(&g_peer, e.data, e.len, now) && e.hasSrc)
                    gblink_peer_radio_accepted(e.src);
            }
            if (g_running) gbl_peer_tick(&g_peer, now);
        }
    }

    void request(bool run, uint8_t mode, uint8_t variant, uint32_t nonce)
    {
        if (!g_threadStarted.exchange(true))
        {
            k_sem_init(&g_wake, 0, 1);
            zshim_thread_start("gblink_peer", peerThread, 4096, 1);
        }
        const unsigned key = irq_lock();
        g_wanted = { run, mode, variant, nonce };
        const uint32_t gen = g_wantedGen.fetch_add(1) + 1;
        irq_unlock(key);
        k_sem_give(&g_wake);
        // Bounded: the thread applies it on its next pass (5 ms at most).
        for (int i = 0; i < 500 && static_cast<int32_t>(g_doneGen.load() - gen) < 0; i++)
            k_sleep(K_MSEC(1));
    }
}

// Defaults: nobody is watching, and the radio does not lock. The board's UI,
// its radio and the simulator override them.
extern "C" __attribute__((weak)) void gblink_peer_observe_status(uint16_t) {}
extern "C" __attribute__((weak)) void gblink_peer_radio_accepted(const uint8_t*) {}

extern "C" void gblink_peer_radio_rx(const uint8_t* d, size_t n)
{
    if (g_running && n <= GBL_PEER_MAX_DATAGRAM) post(EV_RADIO, d, n);
}

extern "C" void gblink_peer_radio_rx_from(const uint8_t* d, size_t n, const uint8_t src[6])
{
    if (g_running && n <= GBL_PEER_MAX_DATAGRAM) post(EV_RADIO, d, n, src);
}

extern "C" void gblink_peer_start(uint8_t mode, uint8_t variant, uint32_t nonce)
{
    request(true, mode, variant, nonce);
}

extern "C" void gblink_peer_stop(void)
{
    if (!g_threadStarted) return;
    request(false, 0, 0, 0);
}

extern "C" const gbl_peer_t* gblink_peer_state(void) { return &g_peer; }
