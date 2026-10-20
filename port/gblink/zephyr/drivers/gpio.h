/*
 * zephyr/drivers/gpio.h -- included by packetLayer.hpp and tradeSetup.cpp, but
 * nothing they compile on the RP2040 path uses it (the GPIO calls are in the
 * STM32-only masterClock.hpp). Deliberately empty: if a vendored file starts
 * using Zephyr GPIO, the build should fail here rather than silently link a
 * stand-in.
 */
#ifndef GBAGATE_ZSHIM_DRIVERS_GPIO_H
#define GBAGATE_ZSHIM_DRIVERS_GPIO_H
#include <zephyr/kernel.h>
#endif
