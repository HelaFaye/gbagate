#include "emu.hpp"
#include "../sections/sections.hpp"

void EmuModule::execute()
{
    NextSection nextSection = NextSection::setup;
    while (!m_cancel)
    {
        switch (nextSection)
        {
            case NextSection::setup:
            {
                TradeSetup tradeSetup(LINKTYPE_TRADE_SETUP);
                publish(&tradeSetup);
                nextSection = tradeSetup.process(); // -> exit / -> connection / -> cancel
                publish(nullptr);   // before tradeSetup goes
                break;
            }
            case NextSection::connection:
            {
                TradeConnection tradeConnection;
                publish(&tradeConnection);
                nextSection = tradeConnection.process(); // -> tradeLounge / -> disconnect / -> cancel
                publish(nullptr);   // before tradeConnection goes
                break;
            }
            case NextSection::disconnect:
            {
                TradeDisconnect tradeDisconnection;
                publish(&tradeDisconnection);
                nextSection = tradeDisconnection.process(); // -> connection / -> cancel
                publish(nullptr);   // before tradeDisconnection goes
                break;
            }
            case NextSection::lounge:
            {
                TradeLounge tradeLounge;
                publish(&tradeLounge);
                nextSection = tradeLounge.process(); // -> exit / -> connection / -> cancel
                publish(nullptr);   // before tradeLounge goes
                break;
            }
            case NextSection::cancel: [[fallthrough]];
            case NextSection::exit:
            {
                return;
            }
        }
    }
}