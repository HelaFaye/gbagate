/*
 * rom_catalog.c -- see rom_catalog.h
 */

#include <string.h>

#include "rom_catalog.h"

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int casecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = lower(*a++), cb = lower(*b++);
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
    }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

bool rom_name_is_program(const char *name) {
    if (!name || !*name) {
        return false;
    }
    /* FAT hands back "." and ".." and, on some hosts, macOS resource forks
     * beginning "._" -- all of which would otherwise look like candidates. */
    if (name[0] == '.') {
        return false;
    }

    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || !dot[1]) {
        return false;
    }
    return casecmp(dot, ".mb") == 0 || casecmp(dot, ".gba") == 0;
}

/*
 * CRC32, the usual reflected variant with polynomial 0xEDB88320. Computed
 * bitwise rather than from a 1 KB table: this runs a few dozen times per
 * directory scan, never in a timing-critical path, and 1 KB of flash is worth
 * more than the microseconds.
 */
uint32_t rom_crc32(const uint8_t *data, size_t len) {
    if (!data) {
        return 0;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t rom_logo_fingerprint(const uint8_t *header) {
    if (!header) {
        return 0;
    }
    return rom_crc32(header + ROM_LOGO_START, ROM_LOGO_LEN);
}

uint8_t rom_header_checksum(const uint8_t *header) {
    if (!header) {
        return 0;
    }
    uint8_t sum = 0;
    for (int i = 0xA0; i <= 0xBC; i++) {
        sum = (uint8_t)(sum + header[i]);
    }
    return (uint8_t)(0u - (uint8_t)(sum + 0x19u));
}

rom_status_t rom_validate(const uint8_t *header, size_t header_len, uint32_t size) {
    if (size < MB_MIN_SIZE || size > MB_MAX_SIZE) {
        return ROM_BAD_SIZE;
    }
    if (!header || header_len < MB_HEADER) {
        return ROM_UNREADABLE;
    }
    /*
     * Two cheap structural checks. Byte 0xB2 is fixed at 0x96 on every GBA
     * image, and the entry point is an unconditional ARM branch, so its top
     * byte is 0xEA. Together they reject text files, archives and truncated
     * downloads without needing the logo bitmap.
     */
    if (header[0xB2] != 0x96) {
        return ROM_BAD_HEADER;
    }
    if (header[3] != 0xEA) {
        return ROM_BAD_HEADER;
    }
    /*
     * The complement byte is derived from the header's own title, game code
     * and version fields, so a mismatch means the header was edited by
     * something that did not recompute it -- a common result of hand-patching
     * a title string.
     */
    if (header[0xBD] != rom_header_checksum(header)) {
        return ROM_BAD_CHECKSUM;
    }
    return ROM_OK;
}

uint32_t rom_effective_size(const uint8_t *data, uint32_t size) {
    if (!data || size <= MB_HEADER) {
        return size;
    }
    uint32_t end = size;
    /* Many images are padded to a cartridge boundary with 0x00 or 0xFF. None
     * of it needs transferring. Never strip into the header. */
    while (end > MB_MIN_SIZE) {
        uint8_t b = data[end - 1];
        if (b != 0x00 && b != 0xFF) {
            break;
        }
        end--;
    }
    /* The protocol moves the payload in 16-byte groups. */
    uint32_t payload = end - MB_HEADER;
    payload = (payload + 0x0F) & ~0x0Fu;
    uint32_t out = payload + MB_HEADER;
    return out > size ? size : out;
}

static void insert_sorted(rom_catalog_t *cat, const rom_entry_t *e) {
    uint16_t i = cat->count;
    while (i > 0 && casecmp(cat->entries[i - 1].name, e->name) > 0) {
        cat->entries[i] = cat->entries[i - 1];
        i--;
    }
    cat->entries[i] = *e;
    cat->count++;
}

void rom_catalog_scan(rom_catalog_t *cat, const rom_io_t *io, void *ctx,
                      uint32_t logo_reference) {
    memset(cat, 0, sizeof *cat);
    cat->logo_reference = logo_reference;
    if (!io || !io->next) {
        return;
    }

    char name[ROM_NAME_MAX];
    uint32_t size = 0;

    while (io->next(ctx, name, sizeof name, &size)) {
        if (!rom_name_is_program(name)) {
            continue;
        }
        if (cat->count >= ROM_MAX_ENTRIES) {
            /* Keep scanning so the flag is accurate, but stop storing. */
            cat->truncated = true;
            continue;
        }

        rom_entry_t e;
        memset(&e, 0, sizeof e);
        /* memcpy with an explicit terminator rather than strncpy: the name
         * comes from a FAT directory and long-name entries can reach the
         * buffer length exactly, which strncpy would leave unterminated. */
        size_t n = strlen(name);
        if (n > sizeof e.name - 1) {
            n = sizeof e.name - 1;
        }
        memcpy(e.name, name, n);
        e.name[n] = '\0';
        e.size      = size;
        e.effective = size;

        /*
         * Check the size before reading, so a 64-byte truncated download
         * reports "wrong size" rather than "unreadable" -- the header read
         * would fail on it anyway, and the size is the useful diagnosis.
         */
        if (size < MB_MIN_SIZE || size > MB_MAX_SIZE) {
            e.status = ROM_BAD_SIZE;
        } else {
            uint8_t header[MB_HEADER];
            if (!io->read_head || !io->read_head(ctx, name, header, sizeof header)) {
                e.status = ROM_UNREADABLE;
            } else {
                e.status = rom_validate(header, sizeof header, size);
                /*
                 * Only meaningful once a reference exists, and only worth
                 * reporting on a file that is otherwise fine -- "bad logo" on
                 * top of "not a GBA image" tells the user nothing new.
                 */
                if (e.status == ROM_OK && logo_reference != 0 &&
                    rom_logo_fingerprint(header) != logo_reference) {
                    e.status = ROM_BAD_LOGO;
                }
            }
        }

        if (e.status == ROM_OK) {
            cat->playable++;
        } else {
            cat->rejected++;
        }
        insert_sorted(cat, &e);
    }
}

bool rom_stage(const rom_catalog_t *cat, uint16_t index, const rom_io_t *io,
               void *ctx, uint8_t *buf, size_t cap, uint32_t *out_len) {
    if (!cat || index >= cat->count || !io || !io->read_all || !buf) {
        return false;
    }
    const rom_entry_t *e = &cat->entries[index];
    if (e->status != ROM_OK) {
        return false;
    }
    /* Room for the file plus whatever padding alignment adds, so mb_send() can
     * read a whole trailing word without running off the end. */
    if (cap < mb_padded_size(e->size)) {
        return false;
    }

    uint32_t len = 0;
    if (!io->read_all(ctx, e->name, buf, cap, &len)) {
        return false;
    }
    if (len != e->size) {
        return false; /* the file changed under us, or a short read */
    }

    uint32_t effective = rom_effective_size(buf, len);
    /* Zero the gap so a stripped tail transfers as zeros rather than stale
     * bytes from a previously staged program. */
    if (effective > len) {
        memset(buf + len, 0, effective - len);
    }

    if (out_len) {
        *out_len = effective;
    }
    return true;
}

const char *rom_status_name(rom_status_t s) {
    switch (s) {
    case ROM_OK:         return "ok";
    case ROM_BAD_SIZE:   return "wrong size";
    case ROM_BAD_HEADER: return "not a GBA image";
    case ROM_BAD_CHECKSUM: return "bad header checksum";
    case ROM_BAD_LOGO:   return "bad logo - repair on PC";
    case ROM_UNREADABLE: return "unreadable";
    default:             return "?";
    }
}
