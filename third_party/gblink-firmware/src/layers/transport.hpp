#pragma once

#include <cstdint>
#include <span>

// Tracks which transport (WebUSB, WebSerial/CDC-ACM, or the bridge UART to a
// companion MCU) the device should currently send status and data back on, and
// provides a dispatch layer so call sites don't need to know which transport
// is active.
namespace Transport
{
    // gbagate (PATCHES.md P8): Local is an in-firmware host -- the wireless
    // link cable (src/gbl_peer.c) drives the adapter through it exactly as a
    // PC would through USB.
    // gbagate (PATCHES.md P15): Net is a host on the board's WiFi, speaking
    // the CDC port's framing over a WebSocket (port/gblink/gblink_port.h).
    enum class Id : uint8_t { Usb, Serial, BridgeUart, Local, Net };

    using ReceiveHandler = void(*)(std::span<const uint8_t>, void*);

    // Each transport layer calls this from its receive path when a host frame
    // arrives, so the device replies on the same transport the host used.
    void setActive(Id id);
    Id active();

    // gbagate (PATCHES.md P11). The host whose frame is being handled
    // (setActive) starts a mode: the mode's status and data now go to it,
    // until another host starts one. So a second host only querying the
    // adapter -- a launcher polling GetFirmwareInfo while a companion MCU runs
    // it over the bridge UART -- neither takes the mode's traffic nor gets it.
    void claimForMode();
    Id owner();

    // gbagate (PATCHES.md P8). A host frame arrived on id: make it active and
    // say so -- unless the local host owns the adapter (a local sink is set),
    // in which case the frame is refused, so a PC cannot take over or cancel
    // the wireless link cable's session, nor draw its replies to USB.
    bool claim(Id id);

    // Register the same handler on every transport so the device accepts host
    // commands/data regardless of which one is open.
    void registerCommandHandler(ReceiveHandler handler, void* userData);
    void registerDataHandler(ReceiveHandler handler, void* userData);

    // A running mode's status and data: to the host that started it (P11).
    bool sendStatus(std::span<const uint8_t, 2> data);
    bool sendData(std::span<const uint8_t> data);

    // The answer to the command being handled: to the host that sent it (P11).
    bool sendReply(std::span<const uint8_t> data);

    // gbagate (PATCHES.md P8). The local host's receive side, and its way in:
    // inject* hand a command or data packet to the same handlers USB reaches,
    // and make Local the active transport so the replies come back to it.
    using LocalSink = void(*)(bool status, std::span<const uint8_t>, void*);
    void setLocalSink(LocalSink sink, void* userData);
    void injectCommand(std::span<const uint8_t> data);
    void injectData(std::span<const uint8_t> data);

    // gbagate (PATCHES.md P14). What the adapter is doing, for the device's
    // own screen: the mode Control is running (-1 between modes), the last
    // status word a mode reported and how many, and how many host frames each
    // transport delivered. Written from the firmware's threads, read from the
    // UI's; each value is one atomic word.
    void noteMode(int mode);
    int mode();
    uint16_t lastStatus();
    uint32_t statusCount();
    uint32_t frames(Id id);
}
