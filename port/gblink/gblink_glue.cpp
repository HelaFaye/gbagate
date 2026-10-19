// gblink_glue.cpp -- joins gblink_port.h's C entry points to the vendored
// C++ firmware, and runs it.
//
// Upstream's main() is "construct Control, call executeMode() forever". It is
// compiled here unchanged, renamed, and run on its own thread so the rest of
// gbagate keeps app_main().
#include <zephyr/kernel.h>

#include "layers/serialLayer.hpp"
#include "layers/usbLayer.hpp"

extern "C" {
#include "gblink_port.h"
#include "zshim.h"
}

#define main gblink_firmware_main
#include "main.cpp"
#undef main

extern "C" void gblink_usb_on_receive(uint8_t ep_out, const uint8_t* data, size_t len)
{
    UsbLayer::getInstance().receive(ep_out, std::span<const uint8_t>(data, len));
}

extern "C" void gblink_usb_on_configured(bool configured)
{
    UsbLayer::getInstance().setConfigured(configured);
}

extern "C" void gblink_cdc_on_receive(const uint8_t* data, size_t len)
{
    SerialLayer::getInstance().receiveBytes(std::span<const uint8_t>(data, len));
}

static void control_thread(void*, void*, void*)
{
    gblink_firmware_main();
}

extern "C" void gblink_start(void)
{
    // Construct the singletons before anything can call into them from a USB
    // callback: function-local statics are thread-safe to construct, but a
    // receive racing the very first construction is still a receive dropped.
    (void)UsbLayer::getInstance();
    (void)SerialLayer::getInstance();
    (void)Hardware::getInstance();

    zshim_run_inits();
    zshim_start_threads();
    // Zephyr's main thread runs at priority 0, with the wrong-pin watchdog at
    // 12 below it; keep that ordering.
    zshim_thread_start("gblink", control_thread, 8192, 0);
}
