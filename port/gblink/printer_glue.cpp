// printer_glue.cpp -- the board's own Game Boy Printer: GB-Link's printer
// mode (0x03, third_party/gblink-firmware/src/module/gb.cpp) driven by the
// board as its local host, its stream kept for the UI to decode
// (src/gbprinter.h, gbp_feed_stream) and save.
//
// The firmware's thread delivers the stream through Transport's local sink
// in sends of up to 64 bytes; with the local host it holds a picture back
// until the PRINT and then sends it in one burst (about 6 KB for 160 x 144).
// The sink copies it into a ring here, and the UI's thread drains it: one
// writer and one reader, so the two indexes are all the locking it needs.
// A burst the UI has not drained in time loses what does not fit, counted.
//
// Like celio_glue.cpp this takes the one local sink: the UI runs one link
// mode at a time.
#include <atomic>
#include <cstring>

#include "layers/transport.hpp"

extern "C" {
#include "gblink_port.h"
}

namespace
{
    constexpr uint32_t RING = 16384;            // the firmware's own buffer's size
    uint8_t g_ring[RING];
    std::atomic<uint32_t> g_head{0};            // written by the sink
    std::atomic<uint32_t> g_tail{0};            // by the reader
    std::atomic<uint32_t> g_lost{0};
    std::atomic<uint16_t> g_status{0};
    std::atomic<bool> g_running{false};

    void sink(bool status, std::span<const uint8_t> d, void*)
    {
        if (!g_running) return;
        if (status)
        {
            if (d.size() == 2) g_status = static_cast<uint16_t>(d[0] | (d[1] << 8));
            return;
        }
        uint32_t h = g_head.load(std::memory_order_relaxed);
        const uint32_t t = g_tail.load(std::memory_order_acquire);
        for (uint8_t b : d)
        {
            if (h - t == RING) { g_lost.fetch_add(1, std::memory_order_relaxed); continue; }
            g_ring[h % RING] = b;
            h++;
        }
        g_head.store(h, std::memory_order_release);
    }

    void cmd(std::initializer_list<uint8_t> c)
    {
        Transport::injectCommand({ c.begin(), c.size() });
    }
}

extern "C" void gblink_printer_start(void)
{
    if (g_running) return;
    g_tail = g_head.load();
    g_lost = 0;
    g_status = 0;
    g_running = true;
    Transport::setLocalSink(sink, nullptr);
    cmd({ 0x41 });              // SetVoltage5V: a Game Boy link (doc/GBLINK.md)
    cmd({ 0x00, 0x03 });        // SetMode printer
}

extern "C" void gblink_printer_stop(void)
{
    if (!g_running) return;
    cmd({ 0x01 });              // Cancel: the printer loop leaves on its next check
    cmd({ 0x40 });              // back to 3.3 V, the GBA's
    g_running = false;
    Transport::setLocalSink(nullptr, nullptr);
}

extern "C" size_t gblink_printer_read(uint8_t* out, size_t cap)
{
    const uint32_t h = g_head.load(std::memory_order_acquire);
    uint32_t t = g_tail.load(std::memory_order_relaxed);
    size_t n = 0;
    while (t != h && n < cap) out[n++] = g_ring[t++ % RING];
    g_tail.store(t, std::memory_order_release);
    return n;
}

extern "C" void gblink_printer_view(gblink_printer_view_t* out)
{
    out->running = g_running;
    out->status = g_status;
    out->lost = g_lost;
}
