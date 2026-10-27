/*
 * rom_catalog.h -- browse and stage multiboot programs from the USB drive.
 *
 * The device exposes a FAT partition over USB MSC, so programs arrive by
 * drag-and-drop rather than by recompiling. This module turns that directory
 * into a list ui_list can show, validates each file well enough to refuse junk
 * before the user powers on a console, and stages a chosen one into RAM.
 *
 * Filesystem access is injected, so scanning, validation, truncation and the
 * staging rules are all host-testable.
 *
 * The staging rule
 * ----------------
 * The whole file is read into RAM and the handle closed *before* the multiboot
 * handshake starts. rom_stage() is that step, and mb_send() never sees a file
 * handle.
 *
 * Worth being precise about why, because the obvious reason is not quite the
 * real one. During multiboot the adapter is the SPI *master*, so a cache stall
 * only stretches a clock period and the GBA tolerates that -- unlike the
 * Multi-Play PHY, where we may be the child and a stall corrupts a frame
 * outright. The reasons here are less dramatic but still decisive:
 *
 *   - the storage and the link may share an SPI host, and a FATFS read in the
 *     middle of a transfer is bus contention, not just latency;
 *   - it keeps every filesystem error path out of the transfer, so a transfer
 *     can fail for exactly one reason at a time;
 *   - a 256 KB read costs a few tens of milliseconds once, against a
 *     per-word cost repeated 65,000 times.
 */

#ifndef ROM_CATALOG_H
#define ROM_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gba_multiboot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_MAX_ENTRIES 32
#define ROM_NAME_MAX    40

typedef enum {
    ROM_OK = 0,
    ROM_BAD_SIZE,    /* smaller than a header or larger than EWRAM */
    ROM_BAD_HEADER,  /* missing the fixed byte or a sane entry branch */
    ROM_BAD_CHECKSUM,/* header complement check does not match its contents */
    ROM_BAD_LOGO,    /* logo fingerprint differs from the reference */
    ROM_UNREADABLE   /* the filesystem would not give us the header */
} rom_status_t;

typedef struct {
    char         name[ROM_NAME_MAX];
    uint32_t     size;      /* bytes on disk                         */
    uint32_t     effective; /* after trailing padding, 16-byte aligned */
    rom_status_t status;
} rom_entry_t;

typedef struct {
    rom_entry_t entries[ROM_MAX_ENTRIES];
    uint16_t    count;
    uint16_t    playable;   /* how many have status ROM_OK */
    bool        truncated;  /* the directory held more than we can list */
    uint32_t    rejected;   /* files that looked like ROMs but failed  */

    /* Echo of the reference the scan was run with, for the UI to display. */
    uint32_t    logo_reference;
} rom_catalog_t;

/*
 * Directory iteration and file reading, injected.
 *
 * next():      fill name/size for the next entry, false when exhausted.
 * read_head(): read the first n bytes of a file.
 * read_all():  read a whole file into buf, up to cap.
 */
typedef struct {
    bool (*next)(void *ctx, char *name, size_t cap, uint32_t *size);
    bool (*read_head)(void *ctx, const char *name, uint8_t *buf, size_t n);
    bool (*read_all)(void *ctx, const char *name, uint8_t *buf, size_t cap,
                     uint32_t *out_len);
} rom_io_t;

/*
 * Scan the directory. Entries come back sorted by name, case-insensitively.
 *
 * logo_reference is the CRC32 of a known-good logo area, or 0 for "unknown".
 * Zero disables the logo check entirely: a device with no reference must not
 * reject every file, because a false rejection is worse than the failure it
 * would have prevented.
 *
 * It is a parameter rather than a field the caller pre-sets, because scan
 * fully overwrites the catalogue -- an earlier version read the field before
 * zeroing, which meant an uninitialised catalogue silently rejected every ROM.
 */
void rom_catalog_scan(rom_catalog_t *cat, const rom_io_t *io, void *ctx,
                      uint32_t logo_reference);

/* Is this filename one we care about? Exposed because the scanner and the USB
 * write-completion hook both need the same answer. */
bool rom_name_is_program(const char *name);

/*
 * Validate a 192-byte header plus a size.
 *
 * Deliberately does not check the Nintendo logo at 0x04..0x9F. Doing so would
 * mean embedding Nintendo's bitmap in our firmware, and it would tell us
 * nothing we can act on: if the logo is wrong the console rejects the image
 * itself, so the useful place to fix that is a repair tool on the user's own
 * machine with their own dump as the donor. See tools/fix_header.py.
 *
 * It does check the header complement byte at 0xBD, which is computed from the
 * header's own contents and so costs nothing to verify and catches corruption
 * the two structural byte checks would miss.
 */
rom_status_t rom_validate(const uint8_t *header, size_t header_len, uint32_t size);

/*
 * The complement check the GBA header carries at offset 0xBD:
 * 0x00 - (0x19 + sum of bytes 0xA0..0xBC), truncated to 8 bits.
 */
uint8_t rom_header_checksum(const uint8_t *header);

/* ---- logo fingerprint --------------------------------------------------
 *
 * The GBA verifies the Nintendo logo at 0x04..0x9F during multiboot, so an
 * image with a wrong logo transfers completely and is then rejected by the
 * console with no diagnostic -- slow, silent, and easy to blame on the
 * adapter. Catching it here means telling the user before they power anything
 * on.
 *
 * We store a CRC32 of those bytes, never the bytes. A 4-byte fingerprint is
 * not a reproduction of Nintendo's artwork; the logo itself stays out of this
 * repository and out of the firmware image. The reference value comes from a
 * dump the user already owns, either baked in at build time via
 * `tools/fix_header.py --fingerprint`, or computed on device from a donor file
 * placed on the USB drive.
 */

#define ROM_LOGO_START 0x04
#define ROM_LOGO_LEN   0x9C   /* 156 bytes, 0x04..0x9F inclusive */

uint32_t rom_crc32(const uint8_t *data, size_t len);

/* Fingerprint of a header's logo area. */
uint32_t rom_logo_fingerprint(const uint8_t *header);

static inline bool rom_logo_known(const rom_catalog_t *cat) {
    return cat->logo_reference != 0;
}

/*
 * Give image (a whole multiboot image, header first) the logo of donor (a
 * header from a program the user owns, on the drive), and recompute the
 * complement byte. For an image the firmware carries itself -- the memory
 * agent -- which cannot ship Nintendo's bytes. False, and image untouched,
 * if either is shorter than a header or the donor's logo area is blank.
 */
bool rom_borrow_logo(uint8_t *image, size_t image_len, const uint8_t *donor, size_t donor_len);

/* Trailing 0x00/0xFF padding removed, rounded back up to 16 bytes. */
uint32_t rom_effective_size(const uint8_t *data, uint32_t size);

/*
 * Read a catalogue entry into buf and close the file. Returns false if the
 * entry is unplayable, the buffer is too small, or the read fails. On success
 * *out_len is what to hand mb_send().
 */
bool rom_stage(const rom_catalog_t *cat, uint16_t index, const rom_io_t *io,
               void *ctx, uint8_t *buf, size_t cap, uint32_t *out_len);

const char *rom_status_name(rom_status_t s);

#ifdef __cplusplus
}
#endif

#endif /* ROM_CATALOG_H */
