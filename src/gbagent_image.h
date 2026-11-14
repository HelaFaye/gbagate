/*
 * gbagent_image.h -- the memory agent's multiboot image (rom/agent.c),
 * embedded for the board to send. See src/gbagent_image.c.
 */
#ifndef GBAGENT_IMAGE_H
#define GBAGENT_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t  gbagent_image[];
extern const uint32_t gbagent_image_len;

#ifdef __cplusplus
}
#endif

#endif /* GBAGENT_IMAGE_H */
