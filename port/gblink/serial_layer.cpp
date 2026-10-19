// serial_layer.cpp -- SerialLayer for the gbagate GB-Link personality.
//
// Replaces upstream layers/serialLayer.cpp. The decoder is src/gblink_frame.c;
// what is kept from upstream is the routing rule in dispatchFrame(): a frame
// claims the serial transport only if it is on a real channel with a handler
// registered. A host's modem prober (ModemManager on Linux opens every new
// CDC-ACM port and types AT commands at it) can produce a parseable header by
// accident, and must not be able to steer replies away from WebUSB.
#include "layers/serialLayer.hpp"

extern "C" {
#include "gblink_port.h"
}

SerialLayer& SerialLayer::getInstance()
{
    static SerialLayer instance;
    return instance;
}

SerialLayer::SerialLayer()
{
    gblink_rx_init(&m_rx);
    k_mutex_init(&m_txMutex);
}

void SerialLayer::receiveBytes(std::span<const uint8_t> bytes)
{
    for (uint8_t b : bytes)
    {
        gblink_frame_t frame;
        if (gblink_rx_feed(&m_rx, b, &frame)) dispatchFrame(frame);
    }
}

void SerialLayer::dispatchFrame(const gblink_frame_t& frame)
{
    auto payload = std::span<const uint8_t>(frame.payload, frame.len);

    if (frame.channel == channelCommand && m_commandHandler.handler != nullptr) {
        Transport::setActive(Transport::Id::Serial);
        m_commandHandler.handler(payload, m_commandHandler.userData);
    } else if (frame.channel == channelData && m_dataHandler.handler != nullptr) {
        Transport::setActive(Transport::Id::Serial);
        m_dataHandler.handler(payload, m_dataHandler.userData);
    } else if (frame.channel >= GBLINK_CH_GBAGATE && m_extHandler != nullptr) {
        // Deliberately does not claim the transport: gbagate traffic sharing
        // the port must not move GB-Link replies off WebUSB.
        m_extHandler(frame.channel, payload, m_extUserData);
    }
}

bool SerialLayer::sendFrame(uint8_t channel, std::span<const uint8_t> payload)
{
    uint8_t frame[GBLINK_FRAME_MAX];
    const size_t n = gblink_frame_encode(channel, payload.data(), payload.size(),
                                         frame, sizeof frame);
    if (n == 0) return false;

    // One frame per write call, under a lock, so frames from the module
    // thread and the PHY's status path never interleave mid-frame.
    k_mutex_lock(&m_txMutex, K_FOREVER);
    const bool ok = gblink_cdc_write(frame, n);
    k_mutex_unlock(&m_txMutex);
    return ok;
}

bool SerialLayer::sendStatus(std::span<const uint8_t, 2> data)
{
    return sendFrame(channelStatus, data);
}

bool SerialLayer::sendData(std::span<const uint8_t> data)
{
    return sendFrame(channelData, data);
}

void SerialLayer::setReceiveCommandHandler(Transport::ReceiveHandler handler, void* userData)
{
    m_commandHandler = { handler, userData };
}

void SerialLayer::setReceiveDataHandler(Transport::ReceiveHandler handler, void* userData)
{
    m_dataHandler = { handler, userData };
}

void SerialLayer::setExtensionHandler(ExtensionHandler handler, void* userData)
{
    m_extHandler = handler;
    m_extUserData = userData;
}
