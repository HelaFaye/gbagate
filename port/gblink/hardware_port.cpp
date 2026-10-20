// hardware_port.cpp -- Hardware (upstream hardware.cpp) over gblink_port.h.
//
// Upstream drives a WS2812 on GP16 and a pair of load switches on GP11/GP12.
// Those are board parts, so the class forwards to the port, which knows what
// this board actually has fitted.
#include "hardware.hpp"

extern "C" {
#include "gblink_port.h"
}

Hardware& Hardware::getInstance()
{
    static Hardware instance;
    return instance;
}

Hardware::Hardware() {}

void Hardware::setVoltage3V3() { gblink_hw_set_vcc_5v(false); }
void Hardware::setVoltage5V()  { gblink_hw_set_vcc_5v(true); }

void Hardware::setLED(uint8_t r, uint8_t g, uint8_t b, bool on)
{
    gblink_hw_set_led(r, g, b, on);
}

void Hardware::rebootToBootloader() { gblink_hw_reboot_to_bootloader(); }
void Hardware::reboot()             { gblink_hw_reboot(); }

// Upstream's private helpers; declared in the vendored header, unused here.
void Hardware::ws2812Init() {}
void Hardware::ws2812Set(uint32_t) {}
