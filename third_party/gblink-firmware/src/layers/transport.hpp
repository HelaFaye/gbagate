#pragma once

#include <cstdint>
#include <span>

// Tracks which of the two host transports (WebUSB or WebSerial/CDC-ACM) the
// device should currently send status and data back on, and provides a tiny
// dispatch layer so call sites don't need to know which transport is active.
namespace Transport
{
    // gbagate (PATCHES.md P8): Local is an in-firmware host -- the wireless
    // link cable (src/gbl_peer.c) drives the adapter through it exactly as a
    // PC would through USB.
    enum class Id : uint8_t { Usb, Serial, Local };

    using ReceiveHandler = void(*)(std::span<const uint8_t>, void*);

    // Each transport layer calls this from its receive path when a host frame
    // arrives, so the device replies on the same transport the host used.
    void setActive(Id id);
    Id active();

    // Register the same handler on both transports so the device accepts host
    // commands/data regardless of which one is open.
    void registerCommandHandler(ReceiveHandler handler, void* userData);
    void registerDataHandler(ReceiveHandler handler, void* userData);

    // Route a send to whichever transport the host last spoke on.
    bool sendStatus(std::span<const uint8_t, 2> data);
    bool sendData(std::span<const uint8_t> data);

    // gbagate (PATCHES.md P8). The local host's receive side, and its way in:
    // inject* hand a command or data packet to the same handlers USB reaches,
    // and make Local the active transport so the replies come back to it.
    using LocalSink = void(*)(bool status, std::span<const uint8_t>, void*);
    void setLocalSink(LocalSink sink, void* userData);
    void injectCommand(std::span<const uint8_t> data);
    void injectData(std::span<const uint8_t> data);
}
