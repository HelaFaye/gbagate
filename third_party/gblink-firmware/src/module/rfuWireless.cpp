
#include "rfuWireless.hpp"
#include "../linkStatus.hpp"

void RfuWirelessModule::execute()
{
    m_cancel = false;

    // The adapter has to be live on the GBA link immediately: the game runs
    // AgbRFU_checkID and enters the Union Room before any network partner
    // exists, so it cannot wait for the pairing handshake. The relay attaches
    // later, as a partner joins (receiveCommand). The master/slave role is
    // irrelevant: the protocol core is symmetric and hosting is decided
    // in-game.
    sendLinkStatus(LinkStatus::AwaitMode);

    {
        RfuProtocolSection section(m_role);
        // gbagate (PATCHES.md P13): set and cleared under the lock cancel() takes,
        // and cleared before the section is destroyed.
        unsigned int key = irq_lock();
        m_currentSection = &section;
        const bool cancelled = m_cancel;
        if (cancelled) m_currentSection = nullptr;
        irq_unlock(key);
        if (cancelled) return;
        rfuLink_enable();
        section.process();
        // Stop the PIO before the section destructor deregisters the done
        // callback, so the PIO ISR cannot fire mid-deregistration.
        rfuLink_disable();
        key = irq_lock();
        m_currentSection = nullptr;
        irq_unlock(key);
    }
}

void RfuWirelessModule::receiveCommand(std::span<const uint8_t> command)
{
    // Progress the pairing handshake without blocking the already-running GBA
    // link, so the relay is established once a partner is present.
    switch (static_cast<LinkModeCommand>(command[0]))
    {
        case LinkModeCommand::SetModeMaster:
        case LinkModeCommand::SetModeSlave:
            sendLinkStatus(LinkStatus::HandshakeReceived);
            break;

        case LinkModeCommand::StartHandshake:
            sendLinkStatus(LinkStatus::LinkConnected);
            break;

        case LinkModeCommand::ConnectLink:
            // Ignored: the RFU protocol core handles link establishment
            break;

        default: break;
    }
}
