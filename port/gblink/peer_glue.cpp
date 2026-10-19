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
        uint8_t data[GBL_PEER_MAX_DATAGRAM];
    };

    K_MSGQ_DEFINE(g_events, sizeof(Event), 96, 4);

    gbl_peer_t g_peer;
    volatile bool g_running;
    volatile bool g_threadStarted;
    struct k_sem g_wake;

    void post(uint8_t kind, const uint8_t* d, size_t n)
    {
        Event e{};
        e.kind = kind;
        e.len = static_cast<uint8_t>(n > sizeof e.data ? sizeof e.data : n);
        std::memcpy(e.data, d, e.len);
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

    void peerThread(void*, void*, void*)
    {
        for (;;)
        {
            k_sem_take(&g_wake, K_MSEC(5));
            const uint32_t now = k_uptime_get_32();
            Event e;
            while (k_msgq_get(&g_events, &e, K_NO_WAIT) == 0)
            {
                if (!g_running) continue;
                if (e.kind == EV_STATUS && e.len == 2)
                    gbl_peer_on_status(&g_peer, static_cast<uint16_t>(e.data[0] | (e.data[1] << 8)), now);
                else if (e.kind == EV_DATA)
                    gbl_peer_on_data(&g_peer, e.data, e.len, now);
                else if (e.kind == EV_RADIO)
                    gbl_peer_on_radio(&g_peer, e.data, e.len, now);
            }
            if (g_running) gbl_peer_tick(&g_peer, now);
        }
    }
}

// Default: nobody is watching. The board's UI and the simulator override it.
extern "C" __attribute__((weak)) void gblink_peer_observe_status(uint16_t) {}

extern "C" void gblink_peer_radio_rx(const uint8_t* d, size_t n)
{
    if (g_running && n <= GBL_PEER_MAX_DATAGRAM) post(EV_RADIO, d, n);
}

extern "C" void gblink_peer_start(uint8_t mode, uint8_t variant, uint32_t nonce)
{
    if (!g_threadStarted)
    {
        k_sem_init(&g_wake, 0, 1);
        zshim_thread_start("gblink_peer", peerThread, 4096, 1);
        g_threadStarted = true;
    }
    const gbl_peer_io_t io = { injectCmd, injectData, radioTx, nullptr };
    g_running = false;
    k_msgq_purge(&g_events);
    gbl_peer_init(&g_peer, &io, nonce, mode, variant);
    Transport::setLocalSink(localSink, nullptr);
    g_running = true;
    gbl_peer_start(&g_peer, k_uptime_get_32());
}

extern "C" void gblink_peer_stop(void)
{
    if (!g_running) return;
    g_running = false;
    gbl_peer_stop(&g_peer);
    Transport::setLocalSink(nullptr, nullptr);
}

extern "C" const gbl_peer_t* gblink_peer_state(void) { return &g_peer; }
