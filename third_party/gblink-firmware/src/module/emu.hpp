#include <span>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#include "moduleInterface.hpp"
#include "../sections/section.hpp"


class EmuModule : public IModule
{
public:
    EmuModule() {}
    
    void execute();

    void receiveCommand(std::span<const uint8_t> command) override {}

    bool canHandle(uint8_t command) override { return (command & 0xF0) == 0x20; }

    void cancel() override
    { 
        const unsigned int key = irq_lock();   // gbagate (PATCHES.md P13): the section cannot go meanwhile
        if (m_currentSection != nullptr) m_currentSection->cancel();
        m_cancel = true;
        irq_unlock(key);
    }

private:
    bool m_cancel = false;
    Section* m_currentSection = nullptr;

    // Set the section cancel() reaches, under its lock; a Cancel that came
    // while the section was being built cancels it now. Cleared (nullptr)
    // before each section is destroyed.
    void publish(Section* section)
    {
        const unsigned int key = irq_lock();
        m_currentSection = section;
        const bool cancelled = m_cancel;
        irq_unlock(key);
        if (section != nullptr && cancelled) section->cancel();
    }
};