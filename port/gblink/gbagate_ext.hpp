// gbagate_ext.hpp -- gbagate's modes and commands inside GB-Link's dispatcher.
//
// GB-Link's Control (vendored) calls these two from its default cases
// (PATCHES.md P7); the protocol they implement is src/gbx_proto.h.
#pragma once

#include <cstdint>
#include <span>

#include "module/moduleInterface.hpp"

namespace gbagate
{
    // Run a mode upstream does not define. Blocks until the mode is left, as
    // upstream's modes do in Control::executeMode(). *current is set while
    // the mode runs so Control routes its commands and cancels to it.
    void executeMode(uint8_t mode, uint8_t variant, IModule** current);

    // A 0x4x command upstream does not define.
    void hardwareCommand(std::span<const uint8_t> data);
}

// Supplied by the board (or the simulator): GBX_CAP_* bits, GBX_BOARD_*, and
// the image (variant_id_t).
extern "C" uint32_t gbagate_ext_caps(void);
extern "C" uint8_t  gbagate_ext_board(void);
extern "C" uint8_t  gbagate_ext_image(void);
