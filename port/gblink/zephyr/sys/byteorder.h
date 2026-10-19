/* zephyr/sys/byteorder.h -- both targets are little-endian. */
#ifndef GBAGATE_ZSHIM_BYTEORDER_H
#define GBAGATE_ZSHIM_BYTEORDER_H
#include <stdint.h>
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the GB-Link shim assumes a little-endian target"
#endif
#define sys_cpu_to_le16(x) ((uint16_t)(x))
#define sys_cpu_to_le32(x) ((uint32_t)(x))
#define sys_le16_to_cpu(x) ((uint16_t)(x))
#define sys_le32_to_cpu(x) ((uint32_t)(x))
#endif
