// celio_glue.cpp -- runs the on-board Celio client (src/celio.c) against the
// local GB-Link firmware, as peer_glue.cpp runs the wireless link cable.
//
// One thread owns the client. The firmware's replies (Transport's local sink)
// and the WebSocket's frames (the network driver's callbacks) are queued for
// it; it drains them, ticks the client, and makes every call into the
// firmware itself. Start and stop are requests the thread carries out, for
// the reason peer_glue.cpp gives: done on the caller's thread they would race
// a tick and could leave the adapter in a mode nobody drives.
//
// The cable and this client both drive the firmware as its local host, and
// there is one local sink. The UI runs one link at a time; the second to
// start would take the sink from the first.
#include <atomic>
#include <cstring>

#include <zephyr/kernel.h>

#include "layers/transport.hpp"

extern "C" {
#include "celio.h"
#include "gblink_port.h"
#include "zshim.h"
}

namespace
{
    enum : uint8_t { EV_STATUS = 1, EV_DATA = 2, EV_TEXT = 3, EV_OPEN = 4, EV_CLOSE = 5 };

    struct Small
    {
        uint8_t kind;
        uint8_t len;
        uint8_t data[64];
    };

    // Celio frames are one packet each, ~250 bytes for 32 words; a frame
    // longer than a slot is dropped and counted.
    constexpr size_t TEXT_SLOT = 600;
    struct Text
    {
        uint8_t kind;
        uint16_t len;
        char text[TEXT_SLOT];
    };

    K_MSGQ_DEFINE(g_small, sizeof(Small), 64, 4);
    K_MSGQ_DEFINE(g_texts, sizeof(Text), 12, 4);

    struct Wanted
    {
        bool run;
        char session[CELIO_SESSION_MAX + 1];
    };
    Wanted g_wanted;                       // under irq_lock
    std::atomic<uint32_t> g_wantedGen{0};
    std::atomic<uint32_t> g_doneGen{0};

    celio_t g_celio;                       // the thread's alone
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_threadStarted{false};
    std::atomic<uint32_t> g_overlong{0};
    struct k_sem g_wake;

    // What the UI reads: copied out by the thread after each pass.
    gblink_celio_view_t g_view;            // under irq_lock

    void postSmall(uint8_t kind, const uint8_t* d, size_t n)
    {
        Small e{};
        e.kind = kind;
        e.len = static_cast<uint8_t>(n > sizeof e.data ? sizeof e.data : n);
        if (d) std::memcpy(e.data, d, e.len);
        k_msgq_put(&g_small, &e, K_NO_WAIT);
        k_sem_give(&g_wake);
    }

    void postText(uint8_t kind, const char* t, size_t n)
    {
        if (n > TEXT_SLOT) { g_overlong++; return; }
        // On the caller's stack: the network driver's task has room for one,
        // and a copy under a lock would hold interrupts off for 600 bytes.
        Text e;
        e.kind = kind;
        e.len = static_cast<uint16_t>(n);
        if (n) std::memcpy(e.text, t, n);
        if (k_msgq_put(&g_texts, &e, K_NO_WAIT) != 0) g_overlong++;
        k_sem_give(&g_wake);
    }

    void localSink(bool status, std::span<const uint8_t> d, void*)
    {
        if (!g_running) return;
        postSmall(status ? EV_STATUS : EV_DATA, d.data(), d.size());
    }

    bool wsSend(void*, const char* t, size_t n) { return gblink_celio_ws_send(t, n); }
    void injectCmd(void*, const uint8_t* c, size_t n) { Transport::injectCommand({ c, n }); }
    void injectData(void*, const uint8_t* d) { Transport::injectData({ d, 64 }); }
    uint32_t rnd(void*) { return gblink_celio_random(); }

    void publish()
    {
        gblink_celio_view_t v{};
        v.state = static_cast<int>(g_celio.state);
        std::memcpy(v.session, g_celio.session, sizeof v.session);
        std::memcpy(v.error, g_celio.error, sizeof v.error);
        v.link_status = g_celio.link_status;
        v.up = g_celio.sent;
        v.down = g_celio.received;
        v.lost = g_celio.dropped + g_overlong.load();
        v.reconnects = g_celio.reconnects;
        const unsigned key = irq_lock();
        g_view = v;
        irq_unlock(key);
    }

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
            celio_stop(&g_celio);           // sessionLeft, Cancel
            Transport::setLocalSink(nullptr, nullptr);
            gblink_celio_ws_connect(false);
        }
        k_msgq_purge(&g_small);
        k_msgq_purge(&g_texts);
        if (w.run)
        {
            const celio_io_t io = { wsSend, injectCmd, injectData, rnd, nullptr };
            celio_start(&g_celio, &io, w.session, k_uptime_get_32());
            Transport::setLocalSink(localSink, nullptr);
            g_running = true;
            gblink_celio_ws_connect(true);
        }
        publish();
        g_doneGen = gen;
    }

    void celioThread(void*, void*, void*)
    {
        for (;;)
        {
            k_sem_take(&g_wake, K_MSEC(5));
            apply();
            const uint32_t now = k_uptime_get_32();
            static Text t;
            while (k_msgq_get(&g_texts, &t, K_NO_WAIT) == 0)
            {
                if (!g_running) continue;
                if (t.kind == EV_OPEN) celio_on_open(&g_celio, now);
                else if (t.kind == EV_CLOSE) celio_on_close(&g_celio, now);
                else celio_on_text(&g_celio, t.text, t.len, now);
            }
            Small e;
            while (k_msgq_get(&g_small, &e, K_NO_WAIT) == 0)
            {
                if (!g_running) continue;
                if (e.kind == EV_STATUS && e.len == 2)
                    celio_on_status(&g_celio, static_cast<uint16_t>(e.data[0] | (e.data[1] << 8)), now);
                else if (e.kind == EV_DATA)
                    celio_on_data(&g_celio, e.data, e.len, now);
            }
            if (g_running)
            {
                celio_tick(&g_celio, now);
                publish();
            }
        }
    }

    void request(bool run, const char* session)
    {
        if (!g_threadStarted.exchange(true))
        {
            k_sem_init(&g_wake, 0, 1);
            zshim_thread_start("gblink_celio", celioThread, 6144, 1);
        }
        const unsigned key = irq_lock();
        g_wanted.run = run;
        std::memset(g_wanted.session, 0, sizeof g_wanted.session);
        if (session) std::strncpy(g_wanted.session, session, CELIO_SESSION_MAX);
        const uint32_t gen = g_wantedGen.fetch_add(1) + 1;
        irq_unlock(key);
        k_sem_give(&g_wake);
        for (int i = 0; i < 500 && static_cast<int32_t>(g_doneGen.load() - gen) < 0; i++)
            k_sleep(K_MSEC(1));
    }
}

// Defaults for an image with no network: nothing to send on.
extern "C" __attribute__((weak)) bool gblink_celio_ws_send(const char*, size_t) { return false; }
extern "C" __attribute__((weak)) void gblink_celio_ws_connect(bool) {}

extern "C" void gblink_celio_start(const char* session) { request(true, session); }

extern "C" void gblink_celio_stop(void)
{
    if (!g_threadStarted) return;
    request(false, nullptr);
}

extern "C" void gblink_celio_ws_open(void) { if (g_running) postText(EV_OPEN, nullptr, 0); }
extern "C" void gblink_celio_ws_closed(void) { if (g_running) postText(EV_CLOSE, nullptr, 0); }
extern "C" void gblink_celio_ws_text(const char* t, size_t n) { if (g_running) postText(EV_TEXT, t, n); }

extern "C" void gblink_celio_view(gblink_celio_view_t* out)
{
    const unsigned key = irq_lock();
    *out = g_view;
    irq_unlock(key);
    if (!g_threadStarted) out->state = CELIO_IDLE;
}
