// gbagate replacement for upstream layers/serialLayer.hpp (see ../../PATCHES.md).
//
// Same class, same public surface, same wire format, in the same two
// instances: CDC-ACM to a host PC, and the bridge UART to a companion MCU.
// Upstream reads Zephyr UARTs from their interrupts; here bytes arrive through
// gblink_cdc_on_receive() / gblink_bridge_on_receive(), from task context, and
// the framing is the portable decoder in src/gblink_frame.c, so the parser that
// runs on the board is the one the host suite tests byte for byte.
//
//     | 0x47 0x42 | channel:1 | len:2 LE | payload[len] |
//        sync 'GB'    0=cmd,1=data,2=status
#pragma once

#include <cstdint>
#include <span>

#include <zephyr/kernel.h>

#include "transport.hpp"

extern "C" {
#include "gblink_frame.h"
}

class SerialLayer
{
public:
    static constexpr size_t maxPayload = GBLINK_FRAME_MAX_PAYLOAD;

    static constexpr uint8_t syncByte0 = GBLINK_SYNC0; // 'G'
    static constexpr uint8_t syncByte1 = GBLINK_SYNC1; // 'B'

    static constexpr uint8_t channelCommand = GBLINK_CH_COMMAND;
    static constexpr uint8_t channelData = GBLINK_CH_DATA;
    static constexpr uint8_t channelStatus = GBLINK_CH_STATUS;

    // USB CDC-ACM.
    static SerialLayer& getInstance();
    // The bridge UART to a companion MCU (gblink_bridge_write).
    static SerialLayer& getBridgeInstance();
    // gbagate (PATCHES.md P15): the WebSocket host on the board's WiFi.
    static SerialLayer& getNetInstance();

    // Upstream counts its UART receive-ring overflows here. Bytes are handed
    // over synchronously in this port, so there is no ring and this stays 0.
    static volatile uint32_t s_rxOverflows;

    SerialLayer(const SerialLayer&) = delete;
    SerialLayer& operator=(const SerialLayer&) = delete;
    SerialLayer(SerialLayer&&) = delete;
    SerialLayer& operator=(SerialLayer&&) = delete;

    bool sendStatus(std::span<const uint8_t, 2> data);
    bool sendData(std::span<const uint8_t> data);

    void setReceiveCommandHandler(Transport::ReceiveHandler handler, void* userData);
    void setReceiveDataHandler(Transport::ReceiveHandler handler, void* userData);

    // Bytes from the CDC stream, any fragmentation.
    void receiveBytes(std::span<const uint8_t> bytes);

    // gbagate's channel-3 traffic (link_proto over the same port). NULL to
    // drop it, which is also what upstream does with an unknown channel.
    using ExtensionHandler = void(*)(uint8_t channel, std::span<const uint8_t>, void*);
    void setExtensionHandler(ExtensionHandler handler, void* userData);
    bool sendFrame(uint8_t channel, std::span<const uint8_t> payload);

    uint32_t framesReceived() const { return m_rx.frames; }
    uint32_t framesRejected() const { return m_rx.oversize; }

private:
    using Writer = bool (*)(const uint8_t*, size_t);
    SerialLayer(Transport::Id id, Writer writer);

    const Transport::Id m_id;
    const Writer m_writer;

    void dispatchFrame(const gblink_frame_t& frame);

    struct ReceiveDelegate
    {
        Transport::ReceiveHandler handler = nullptr;
        void* userData = nullptr;
    };

    ReceiveDelegate m_commandHandler;
    ReceiveDelegate m_dataHandler;
    ExtensionHandler m_extHandler = nullptr;
    void* m_extUserData = nullptr;

    gblink_rx_t m_rx;
    struct k_mutex m_txMutex;
};
