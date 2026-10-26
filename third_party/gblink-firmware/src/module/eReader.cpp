#include "eReader.hpp"
#include "../linkStatus.hpp"
#include "../sections/eReaderProtocolSection.hpp"

extern "C"
{
    #include "../layers/linkLayer.h"
}

EReaderModule::EReaderModule(erproto::Profile profile)
    : m_profile(profile)
{
    k_sem_init(&m_waitForLinkModeCommand, 0, 1);
    k_sem_init(&m_waitForStart, 0, 1);
}

void EReaderModule::cancel()
{
    const unsigned int key = irq_lock();   // gbagate (PATCHES.md P13): the section cannot go meanwhile
    m_cancel = true;
    k_sem_give(&m_waitForLinkModeCommand);
    k_sem_give(&m_waitForStart);
    if (m_currentSection) m_currentSection->cancel();
    irq_unlock(key);
}

bool EReaderModule::canHandle(uint8_t command)
{
    return (command & 0xF0) == 0x10;
}

void EReaderModule::execute()
{
    m_cancel = false;

    sendLinkStatus(LinkStatus::AwaitMode);
    k_sem_take(&m_waitForLinkModeCommand, K_FOREVER);
    if (m_cancel) return;

    sendLinkStatus(LinkStatus::HandshakeReceived);
    k_sem_take(&m_waitForStart, K_FOREVER);
    if (m_cancel) return;

    {
        EReaderProtocolSection section(m_profile);
        // A cancel between the check above and this assignment would miss the
        // section; re-check now that cancel() can reach it.
        // gbagate (PATCHES.md P13): set and cleared under the lock cancel() takes,
        // and cleared before the section is destroyed.
        unsigned int key = irq_lock();
        m_currentSection = &section;
        const bool cancelled = m_cancel;
        if (cancelled) m_currentSection = nullptr;
        irq_unlock(key);
        if (cancelled) return;
        section.armLink();
        sendLinkStatus(LinkStatus::LinkConnected);
        section.process();
        link_changeMode(DISABLED);
        link_releasePartnerPins();
        key = irq_lock();
        m_currentSection = nullptr;
        irq_unlock(key);
    }
}

void EReaderModule::receiveCommand(std::span<const uint8_t> command)
{
    switch (static_cast<LinkModeCommand>(command[0]))
    {
        case LinkModeCommand::SetModeSlave:
            k_sem_give(&m_waitForLinkModeCommand);
            break;

        case LinkModeCommand::StartHandshake:
            k_sem_give(&m_waitForStart);
            break;

        case LinkModeCommand::ConnectLink:
            break;

        default: break;
    }
}
