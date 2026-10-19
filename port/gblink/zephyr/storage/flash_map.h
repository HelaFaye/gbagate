/*
 * zephyr/storage/flash_map.h -- the one flash partition persist.cpp uses.
 *
 * Upstream it is a raw 4 KB sector at 0x1FF000. Here it is a 4 KB byte store
 * the port supplies (an NVS blob on the ESP32-S3, RAM on a host), so persist.cpp
 * -- including its v6 -> v7 migration -- runs unmodified. Erase leaves 0xFF,
 * as NOR flash does, because that is what persist.cpp's magic check expects to
 * read from a never-written device.
 */
#ifndef GBAGATE_ZSHIM_FLASH_MAP_H
#define GBAGATE_ZSHIM_FLASH_MAP_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ZSHIM_STORAGE_SIZE 4096u
#define FIXED_PARTITION_ID(label) 0
struct flash_area {
    uint8_t  fa_id;
    size_t   fa_size;
};
int  flash_area_open(uint8_t id, const struct flash_area **fa);
void flash_area_close(const struct flash_area *fa);
int  flash_area_read(const struct flash_area *fa, off_t off, void *dst, size_t len);
int  flash_area_write(const struct flash_area *fa, off_t off, const void *src, size_t len);
int  flash_area_erase(const struct flash_area *fa, off_t off, size_t len);
#ifdef __cplusplus
}
#endif
#endif
