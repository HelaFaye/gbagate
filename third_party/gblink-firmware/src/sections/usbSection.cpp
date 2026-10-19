
#include "usbSection.hpp"

#include "../linkStatus.hpp"

void UsbSection::establishConncection()
{
    while (m_packetLayer.getReceivedHandshake() != LINK_SLAVE_HANDSHAKE) 
    {
        if (m_cancel) return; 
    }

    sendLinkStatus(LinkStatus::HandshakeReceived);

    // gbagate (PATCHES.md P3): isHandshakeEnabled() means "the last word we
    // sent was 0xB9A0". If ConnectLink lands before the transmit ISR has sent
    // one, the layer goes straight from 0xD15E to 0x8FFF and this loop never
    // ends. 0x8FFF going out implies the handshake was enabled, so accept it.
    while (!m_packetLayer.isHandshakeEnabled() && !m_packetLayer.isConnectSent())
    { 
        k_sleep(K_MSEC(50));
        if (m_cancel) return; 
    }

    switch (m_packetLayerMode)
    {
        case PacketLayer::Mode::master:
            while (m_packetLayer.getTransmittedHandshake() != LINK_MASTER_HANDSHAKE) 
            { 
                if (m_cancel) return; 
            }
            break;

        case PacketLayer::Mode::slave:
            while (m_packetLayer.getReceivedHandshake() != LINK_MASTER_HANDSHAKE) 
            { 
                if (m_cancel) return; 
            }
            break;
    }

    sendLinkStatus(LinkStatus::LinkConnected);
}

bool UsbSection::process()
{
    bool keepAlive = true;
    bool partnerReadyCloseLink = false;
    bool readyCloseLink = false;
    establishConncection();

    while(!m_cancel)
    {
        PacketLayer::TransiveResult result = m_packetLayer.awaitTransiveResults();

        bufferReceivedPackets(result.received);

        if ((result.received[0] == LINKCMD_SEND_HELD_KEYS) && (result.received[1] == LINK_KEY_CODE_EXIT_ROOM))
        {
            keepAlive = false;
        } 

        if (result.transmitted[0] == LINKCMD_SEND_HELD_KEYS && result.transmitted[1] == LINK_KEY_CODE_EXIT_ROOM)
        {
            keepAlive = false;
        }

        if (result.received[0] == LINKCMD_READY_CLOSE_LINK)
        {
            partnerReadyCloseLink = true;
        }

        if (result.transmitted[0] == LINKCMD_READY_CLOSE_LINK)
        {
            readyCloseLink = true;
        }

        if (partnerReadyCloseLink && readyCloseLink)
        {   
            break;
        }
    }

    flush();

    return keepAlive;
}