#include "link.hpp"
#include "../linkStatus.hpp"
#include "../callbacks/commands.hpp"

#include "syscalls/kernel.h"
#include "zephyr/kernel.h"
#include <zephyr/irq.h>


void LinkModule::execute()
{
    m_cancel = false;
    sendLinkStatus(LinkStatus::AwaitMode);
    k_sem_take(&m_waitForLinkModeCommand, K_FOREVER);
    bool keepAlive = true;

    while (keepAlive && !m_cancel)
    {
        {
            UsbSection section(m_packetLayerMode);
            // gbagate (PATCHES.md P13): cancel() and receiveCommand() run on
            // the host's thread and reach the section through this pointer, so
            // it is set and cleared under irq_lock, and cleared before the
            // section is destroyed, not after. A Cancel that came while the
            // section was being built cancels it now.
            unsigned int key = irq_lock();
            m_currentSection = &section;
            const bool cancelled = m_cancel;
            irq_unlock(key);
            if (cancelled) section.cancel();
            keepAlive = section.process();
            key = irq_lock();
            m_currentSection = nullptr;
            irq_unlock(key);
        }

        if (keepAlive)
        {
            sendLinkStatus(LinkStatus::LinkReconnecting);
            k_sleep(K_MSEC(400));
        }
    }
}

void LinkModule::receiveCommand(std::span<const uint8_t> command)
{
    switch (static_cast<LinkModeCommand>(command[0]))
    {
        case LinkModeCommand::SetModeMaster:
            m_packetLayerMode = PacketLayer::Mode::master;
            k_sem_give(&m_waitForLinkModeCommand);
            break;
        case LinkModeCommand::SetModeSlave:
            m_packetLayerMode = PacketLayer::Mode::slave;
            k_sem_give(&m_waitForLinkModeCommand);            
            break;
        // gbagate (PATCHES.md P2): before 0x10/0x11 there is no section, and
        // between sections (the 400 ms reconnect pause) there is none either;
        // upstream dereferenced null in both windows.
        case LinkModeCommand::StartHandshake:
        {
            const unsigned int key = irq_lock();   // P13: the section stays while used
            if (UsbSection* s = m_currentSection) s->startHandshake();
            irq_unlock(key);
            return;
        }
        case LinkModeCommand::ConnectLink:
        {
            const unsigned int key = irq_lock();
            if (UsbSection* s = m_currentSection) s->connectLink();
            irq_unlock(key);
            return;
        }
        default: break;
    }
}

