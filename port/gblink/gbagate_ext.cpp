// gbagate_ext.cpp -- see gbagate_ext.hpp and src/gbx_proto.h.
//
// The Battle Chip Gate mode is gbagate's chipgate.c (host-tested on its own)
// on GB-Link's link layer as a slave: GB-Link's PHY calls back once per word,
// chipgate_step() turns the game's word into the gate's next answer, and the
// answer is staged for the following word -- the same contract the gate has
// with gbagate's own link_source.
#include "gbagate_ext.hpp"

#include <cstring>

#include <zephyr/kernel.h>

#include "layers/transport.hpp"
#include "linkStatus.hpp"
#include "persist.hpp"

extern "C" {
#include "chipgate.h"
#include "gbx_proto.h"
#include "layers/linkLayer.h"
}

namespace
{
    // ---- 0x4F ---------------------------------------------------------------

    void sendExtendedInfo()
    {
        uint8_t r[GBX_INFO_LEN] = {};
        r[0] = GBX_CMD_GET_EXTENDED_INFO;
        r[1] = 'G'; r[2] = 'B'; r[3] = 'X';
        r[4] = GBX_EXT_VERSION;
        r[5] = GBAGATE_VERSION_MAJOR;
        r[6] = GBAGATE_VERSION_MINOR;
        r[7] = GBAGATE_VERSION_PATCH;
        const uint32_t caps = gbagate_ext_caps() | GBX_CAP_CHIPGATE;
        r[8] = static_cast<uint8_t>(caps);
        r[9] = static_cast<uint8_t>(caps >> 8);
        r[10] = static_cast<uint8_t>(caps >> 16);
        r[11] = static_cast<uint8_t>(caps >> 24);
        r[12] = gbagate_ext_board();
        r[13] = gbagate_ext_image();
        Transport::sendReply(std::span<const uint8_t>(r, sizeof r));
    }

    // ---- 0x4E ---------------------------------------------------------------

    void sendPhyStats()
    {
        uint32_t v[GBX_PHY_STATS] = {};
        gbagate_ext_phy_stats(v);
        v[5] = link_getReceivedWordCount();
        v[6] = k_uptime_get_32();
        uint8_t r[GBX_PHY_STATS_LEN] = {};
        r[0] = GBX_CMD_GET_PHY_STATS;
        r[1] = 1;
        for (unsigned i = 0; i < GBX_PHY_STATS; i++)
            for (unsigned b = 0; b < 4; b++)
                r[2 + 4 * i + b] = static_cast<uint8_t>(v[i] >> (8 * b));
        Transport::sendReply(std::span<const uint8_t>(r, sizeof r));
    }

    // ---- mode 0x80: Battle Chip Gate ----------------------------------------------

    // Static, like upstream's protocol sections, so a late PHY callback after
    // the mode ends writes into live storage rather than a dead stack frame.
    chipgate_t g_gate;
    uint16_t   g_next = CG_WORD_CONTINUE;
    volatile uint32_t g_lastWordMs;
    volatile bool g_sessionUp;
    volatile uint16_t g_consumed;   // chip id waiting to be reported, 0 if none
    uint8_t    g_game;

    cg_game_t gameFromVariant(uint8_t v)
    {
        if (v >= 1 && v <= CG_GAME_COUNT) return static_cast<cg_game_t>(v - 1);
        return CG_GAME_BN6_US;
    }

    NextTransmit gateTransmit(void*)
    {
        // Timing is the parent's business; the field is ignored in SLAVE.
        return { g_next, 0 };
    }

    void gateReceive(uint16_t rx, void*)
    {
        const uint16_t sending = g_gate.chip_id;
        g_next = chipgate_step(&g_gate, rx);
        if (chipgate_take_consumed(&g_gate)) g_consumed = sending;
        g_lastWordMs = k_uptime_get_32();
    }

    class ChipGateModule : public IModule
    {
    public:
        explicit ChipGateModule(uint8_t variant)
        {
            k_sem_init(&m_wake, 0, 1);
            g_game = static_cast<uint8_t>(gameFromVariant(variant));
            const unsigned key = irq_lock();
            chipgate_init(&g_gate, static_cast<cg_game_t>(g_game));
            g_next = g_gate.gate_id;
            g_consumed = 0;
            g_sessionUp = false;
            g_lastWordMs = 0;
            irq_unlock(key);
        }

        void execute()
        {
            applyLedForSlot(LED_SLOT_GBA);
            link_detectCableType();
            link_setTransmitCallback(&gateTransmit, nullptr);
            link_setReceiveCallback(&gateReceive, nullptr);
            link_setTransiveDoneCallback(nullptr, nullptr);
            // A gate is a child by definition: the game is the parent.
            link_changeMode(SLAVE);
            sendLinkStatus(LinkStatus::AwaitMode);

            while (!m_cancel)
            {
                k_sem_take(&m_wake, K_MSEC(20));
                report();
            }

            link_changeMode(DISABLED);
            link_setTransmitCallback(nullptr, nullptr);
            link_setReceiveCallback(nullptr, nullptr);
        }

        void cancel() override
        {
            m_cancel = true;
            k_sem_give(&m_wake);
        }

        bool canHandle(uint8_t command) override { return (command & 0xF0) == 0x50; }

        void receiveCommand(std::span<const uint8_t> c) override
        {
            uint8_t r[GBX_STATE_LEN] = {};
            r[0] = c[0];
            size_t n = 2;
            switch (c[0])
            {
                case GBX_CMD_INSERT_CHIP:
                {
                    if (c.size() < 3) return;
                    const uint16_t id = static_cast<uint16_t>(c[1] | (c[2] << 8));
                    const unsigned key = irq_lock();
                    r[1] = chipgate_load(&g_gate, id) ? 1 : 0;
                    irq_unlock(key);
                    break;
                }
                case GBX_CMD_REMOVE_CHIP:
                {
                    const unsigned key = irq_lock();
                    chipgate_clear(&g_gate);
                    irq_unlock(key);
                    r[1] = 1;
                    break;
                }
                case GBX_CMD_GET_STATE:
                {
                    const unsigned key = irq_lock();
                    r[1] = static_cast<uint8_t>(g_gate.state);
                    r[2] = g_gate.slot;
                    r[3] = static_cast<uint8_t>(g_gate.chip_id);
                    r[4] = static_cast<uint8_t>(g_gate.chip_id >> 8);
                    r[5] = static_cast<uint8_t>(g_game + 1);
                    std::memcpy(&r[6], &g_gate.sessions, 4);
                    std::memcpy(&r[10], &g_gate.frames, 4);
                    irq_unlock(key);
                    n = GBX_STATE_LEN - 1;
                    break;
                }
                case GBX_CMD_SET_GAME:
                {
                    if (c.size() < 2) return;
                    const unsigned key = irq_lock();
                    g_game = static_cast<uint8_t>(gameFromVariant(c[1]));
                    chipgate_init(&g_gate, static_cast<cg_game_t>(g_game));
                    g_next = g_gate.gate_id;
                    irq_unlock(key);
                    r[1] = static_cast<uint8_t>(g_game + 1);
                    break;
                }
                default:
                    return;
            }
            Transport::sendReply(std::span<const uint8_t>(r, n));
        }

    private:
        // Session edges and chip completions, from the module thread: the PHY
        // callbacks only record them.
        void report()
        {
            bool up;
            uint16_t sent;
            uint32_t last;
            {
                const unsigned key = irq_lock();
                up = chipgate_connected(&g_gate);
                sent = g_consumed;
                g_consumed = 0;
                last = g_lastWordMs;
                irq_unlock(key);
            }
            // A game that stops clocking mid-session (cable pulled, menu left)
            // leaves the gate ACTIVE forever; call it gone after a second.
            if (up && last && k_uptime_get_32() - last > 1000)
            {
                const unsigned key = irq_lock();
                chipgate_reset_link(&g_gate);
                chipgate_clear(&g_gate);
                irq_unlock(key);
                up = false;
            }
            if (up != g_sessionUp)
            {
                g_sessionUp = up;
                sendLinkStatus(up ? LinkStatus::LinkConnected : LinkStatus::LinkReconnecting);
            }
            if (sent)
            {
                const uint8_t evt[3] = { GBX_EVT_CHIP_SENT, static_cast<uint8_t>(sent),
                                         static_cast<uint8_t>(sent >> 8) };
                Transport::sendData(std::span<const uint8_t>(evt, sizeof evt));
            }
        }

        volatile bool m_cancel = false;
        struct k_sem m_wake;
    };
}

namespace gbagate
{
    void executeMode(uint8_t mode, uint8_t variant, IModule** current)
    {
        switch (mode)
        {
            case GBX_MODE_CHIPGATE:
            {
                ChipGateModule gate(variant);
                *current = &gate;
                gate.execute();
                *current = nullptr;
                sendLinkStatus(LinkStatus::LinkClosed);
                break;
            }
            default:
                break;
        }
    }

    void hardwareCommand(std::span<const uint8_t> data)
    {
        if (data.empty()) return;
        if (data[0] == GBX_CMD_GET_EXTENDED_INFO) sendExtendedInfo();
        else if (data[0] == GBX_CMD_GET_PHY_STATS) sendPhyStats();
    }
}
