// gbagate replacement for upstream layers/usbLayer.hpp (see ../../PATCHES.md).
//
// Upstream's class drives Zephyr's legacy USB stack directly. This one keeps
// the exact public surface the rest of the vendored firmware uses --
// getInstance(), sendStatus(), sendData(), the two handler setters,
// endpointSize() -- and puts the endpoint I/O behind gblink_port.h, so the
// ESP32-S3's TinyUSB driver and the host simulator can both sit underneath.
//
// Behaviour kept from upstream, because the hosts depend on it:
//   - a send waits up to 100 ms for the previous transfer on that endpoint, so
//     DeviceReady followed at once by AwaitMode does not lose the second word
//     (the netplay bridge arms both waits before SetMode for this reason);
//   - receiving anything on either OUT endpoint makes USB the active transport,
//     so replies go back the way the request came;
//   - status sends always carry exactly 2 bytes.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>

#include "../hardware.hpp"
#include "../persist.hpp"
#include "transport.hpp"

extern "C" {
#include "gblink_port.h"
}

class UsbLayer
{
    static constexpr uint8_t commandOutEndpoint = GBLINK_EP_CMD_OUT;
    static constexpr uint8_t statusInEndpoint = GBLINK_EP_STATUS_IN;
    static constexpr uint8_t dataOutEndpoint = GBLINK_EP_DATA_OUT;
    static constexpr uint8_t dataInEndpoint = GBLINK_EP_DATA_IN;

    static constexpr uint16_t m_endpointSize = GBLINK_EP_SIZE;
    static constexpr uint32_t m_writeWaitMs = 100;

public:
    using UsbReceiveHandler = void(*)(std::span<const uint8_t>, void*);

    static UsbLayer& getInstance()
    {
        static UsbLayer instance;
        return instance;
    }

    UsbLayer(const UsbLayer&) = delete;
    UsbLayer& operator=(const UsbLayer&) = delete;
    UsbLayer(UsbLayer&&) = delete;
    UsbLayer& operator=(UsbLayer&&) = delete;

    bool sendStatus(std::span<const uint8_t, 2> data)
    {
        if (!m_configured) return false;
        return gblink_usb_write(statusInEndpoint, data.data(), data.size_bytes(), m_writeWaitMs);
    }

    bool sendData(std::span<const uint8_t> data)
    {
        if (!m_configured) return false;
        if (data.size() > m_endpointSize) return false;
        return gblink_usb_write(dataInEndpoint, data.data(), data.size_bytes(), m_writeWaitMs);
    }

    void setReceiveCommandHandler(const UsbReceiveHandler& handler, void* userData)
    {
        const unsigned key = irq_lock();
        m_command = { handler, userData };
        irq_unlock(key);
    }

    void setReceiveDataHandler(const UsbReceiveHandler& handler, void* userData)
    {
        const unsigned key = irq_lock();
        m_data = { handler, userData };
        irq_unlock(key);
    }

    static constexpr uint16_t endpointSize() { return m_endpointSize; }

    // Called from gblink_usb_on_configured(). Mirrors upstream's LED cue:
    // the idle colour once configured, dim red while powered but not enumerated.
    void setConfigured(bool configured)
    {
        m_configured = configured;
        if (configured) applyLedForSlot(LED_SLOT_IDLE);
        else Hardware::getInstance().setLED(5, 0, 0, true);
    }

    bool configured() const { return m_configured; }

    // Called from gblink_usb_on_receive().
    void receive(uint8_t endpoint, std::span<const uint8_t> data)
    {
        if (data.empty()) return; // a ZLP carries nothing to hand on
        Delegate d;
        {
            const unsigned key = irq_lock();
            d = (endpoint == commandOutEndpoint) ? m_command : m_data;
            irq_unlock(key);
        }
        Transport::setActive(Transport::Id::Usb);
        if (d.handler != nullptr) d.handler(data, d.userData);
    }

private:
    struct Delegate
    {
        UsbReceiveHandler handler = nullptr;
        void* userData = nullptr;
    };

    UsbLayer() = default;

    volatile bool m_configured = false;
    Delegate m_command;
    Delegate m_data;
};
