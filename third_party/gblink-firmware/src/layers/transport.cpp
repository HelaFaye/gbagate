#include "transport.hpp"

#include "usbLayer.hpp"
#include "serialLayer.hpp"

namespace Transport
{
    namespace
    {
        // gbagate (PATCHES.md P11): the requester and the mode's owner apart.
        // The host whose frame is being handled: replies go back to it.
        Id g_active = Id::Usb;
        // The host whose SetMode started the running mode: the mode's status
        // and data go to it, whichever host spoke since.
        Id g_owner = Id::Usb;

        // gbagate (PATCHES.md P8)
        ReceiveHandler g_cmdHandler = nullptr;
        void* g_cmdUserData = nullptr;
        ReceiveHandler g_dataHandler = nullptr;
        void* g_dataUserData = nullptr;
        LocalSink g_localSink = nullptr;
        void* g_localUserData = nullptr;

        bool sendStatusTo(Id id, std::span<const uint8_t, 2> data)
        {
            if (id == Id::Local)
            {
                if (g_localSink) g_localSink(true, data, g_localUserData);
                return g_localSink != nullptr;
            }
            if (id == Id::Serial) return SerialLayer::getInstance().sendStatus(data);
            if (id == Id::BridgeUart) return SerialLayer::getBridgeInstance().sendStatus(data);
            return UsbLayer::getInstance().sendStatus(data);
        }

        bool sendDataTo(Id id, std::span<const uint8_t> data)
        {
            if (id == Id::Local)
            {
                if (g_localSink) g_localSink(false, data, g_localUserData);
                return g_localSink != nullptr;
            }
            if (id == Id::Serial) return SerialLayer::getInstance().sendData(data);
            if (id == Id::BridgeUart) return SerialLayer::getBridgeInstance().sendData(data);
            return UsbLayer::getInstance().sendData(data);
        }
    }

    void setActive(Id id) { g_active = id; }
    Id active() { return g_active; }

    void claimForMode() { g_owner = g_active; }
    Id owner() { return g_owner; }

    // gbagate (PATCHES.md P8)
    bool claim(Id id)
    {
        if (g_localSink != nullptr && id != Id::Local) return false;
        g_active = id;
        return true;
    }

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

    bool sendStatus(std::span<const uint8_t, 2> data) { return sendStatusTo(g_owner, data); }
    bool sendData(std::span<const uint8_t> data) { return sendDataTo(g_owner, data); }
    bool sendReply(std::span<const uint8_t> data) { return sendDataTo(g_active, data); }

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
