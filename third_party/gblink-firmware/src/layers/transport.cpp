#include "transport.hpp"

#include "usbLayer.hpp"
#include "serialLayer.hpp"

namespace Transport
{
    namespace
    {
        Id g_active = Id::Usb;

        // gbagate (PATCHES.md P8)
        ReceiveHandler g_cmdHandler = nullptr;
        void* g_cmdUserData = nullptr;
        ReceiveHandler g_dataHandler = nullptr;
        void* g_dataUserData = nullptr;
        LocalSink g_localSink = nullptr;
        void* g_localUserData = nullptr;
    }

    void setActive(Id id) { g_active = id; }
    Id active() { return g_active; }

    void registerCommandHandler(ReceiveHandler handler, void* userData)
    {
        g_cmdHandler = handler;
        g_cmdUserData = userData;
        UsbLayer::getInstance().setReceiveCommandHandler(handler, userData);
        SerialLayer::getInstance().setReceiveCommandHandler(handler, userData);
        SerialLayer::getBridgeInstance().setReceiveCommandHandler(handler, userData);
    }

    void registerDataHandler(ReceiveHandler handler, void* userData)
    {
        g_dataHandler = handler;
        g_dataUserData = userData;
        UsbLayer::getInstance().setReceiveDataHandler(handler, userData);
        SerialLayer::getInstance().setReceiveDataHandler(handler, userData);
        SerialLayer::getBridgeInstance().setReceiveDataHandler(handler, userData);
    }

    bool sendStatus(std::span<const uint8_t, 2> data)
    {
        if (g_active == Id::Local)
        {
            if (g_localSink) g_localSink(true, data, g_localUserData);
            return g_localSink != nullptr;
        }
        if (g_active == Id::Serial) return SerialLayer::getInstance().sendStatus(data);
        if (g_active == Id::BridgeUart) return SerialLayer::getBridgeInstance().sendStatus(data);
        return UsbLayer::getInstance().sendStatus(data);
    }

    bool sendData(std::span<const uint8_t> data)
    {
        if (g_active == Id::Local)
        {
            if (g_localSink) g_localSink(false, data, g_localUserData);
            return g_localSink != nullptr;
        }
        if (g_active == Id::Serial) return SerialLayer::getInstance().sendData(data);
        if (g_active == Id::BridgeUart) return SerialLayer::getBridgeInstance().sendData(data);
        return UsbLayer::getInstance().sendData(data);
    }

    void setLocalSink(LocalSink sink, void* userData)
    {
        g_localSink = sink;
        g_localUserData = userData;
    }

    void injectCommand(std::span<const uint8_t> data)
    {
        setActive(Id::Local);
        if (g_cmdHandler) g_cmdHandler(data, g_cmdUserData);
    }

    void injectData(std::span<const uint8_t> data)
    {
        setActive(Id::Local);
        if (g_dataHandler) g_dataHandler(data, g_dataUserData);
    }
}
