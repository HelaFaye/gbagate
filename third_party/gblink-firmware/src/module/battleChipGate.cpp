#include "battleChipGate.hpp"
#include "../linkStatus.hpp"

extern "C"
{
    #include "../layers/linkLayer.h"
}

void BattleChipGateModule::execute()
{
    m_cancel = false;

    {
        BattleChipGateSection section(m_gateType);
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
