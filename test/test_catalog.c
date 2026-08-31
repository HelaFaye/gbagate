/*
 * test_catalog.c -- directory scanning, validation and staging.
 *
 *   cc -std=c11 -Wall -Wextra -o test_catalog test_catalog.c \
 *      rom_catalog.c gba_multiboot.c
 *
 * The simulated filesystem serves files a real FAT volume would: valid ROMs,
 * a text file, a macOS resource fork, an oversized dump, a truncated download.
 * Those are the ones that actually turn up on a drive people drag files onto.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_catalog.h"

static int g_fail = 0;

static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("  FAIL %-44s got %ld, want %ld\n", what, got, want);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-44s got \"%s\", want \"%s\"\n",
               what, got ? got : "(null)", want);
        g_fail++;
    }
}

/* ---- simulated volume --------------------------------------------------- */

typedef struct {
    const char *name;
    uint32_t    size;
    bool        valid_header;
    uint8_t     pad_byte;    /* trailing padding, 0 for none */
    uint32_t    real_bytes;  /* bytes before the padding starts */
    bool        unreadable;
} sim_file_t;

typedef struct {
    const sim_file_t *files;
    size_t            count;
    size_t            cursor;
} sim_fs_t;

/* Stand-in for the real logo. The genuine bytes are Nintendo's and are never
 * needed here: the check is a fingerprint comparison, so any consistent
 * pattern exercises it exactly as the real one would. */
static void fill_logo(uint8_t *buf, uint8_t seed) {
    for (int i = 0; i < ROM_LOGO_LEN; i++) {
        buf[ROM_LOGO_START + i] = (uint8_t)(seed + i * 7);
    }
}

static void fill_rom(uint8_t *buf, const sim_file_t *f) {
    memset(buf, 0, f->size);
    if (f->valid_header) {
        buf[0] = 0x2E; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0xEA;
        buf[0xB2] = 0x96;
        fill_logo(buf, strcmp(f->name, "wronglogo.mb") == 0 ? 0x11 : 0x51);
        memcpy(buf + 0xA0, "TESTROM     ", 12);
        memcpy(buf + 0xAC, "TSTE", 4);
        memcpy(buf + 0xB0, "01", 2);
        buf[0xBD] = rom_header_checksum(buf);
    } else {
        memcpy(buf, "not a rom at all", 16);
    }
    uint32_t body_end = f->real_bytes ? f->real_bytes : f->size;
    for (uint32_t i = MB_HEADER; i < body_end && i < f->size; i++) {
        buf[i] = (uint8_t)(0x40 + (i % 0x50));
    }
    if (f->pad_byte) {
        for (uint32_t i = body_end; i < f->size; i++) {
            buf[i] = f->pad_byte;
        }
    }
}

static bool fs_next(void *ctx, char *name, size_t cap, uint32_t *size) {
    sim_fs_t *fs = ctx;
    if (fs->cursor >= fs->count) {
        return false;
    }
    const sim_file_t *f = &fs->files[fs->cursor++];
    strncpy(name, f->name, cap - 1);
    name[cap - 1] = '\0';
    *size = f->size;
    return true;
}

static const sim_file_t *find(sim_fs_t *fs, const char *name) {
    for (size_t i = 0; i < fs->count; i++) {
        if (strcmp(fs->files[i].name, name) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static bool fs_read_head(void *ctx, const char *name, uint8_t *buf, size_t n) {
    sim_fs_t *fs = ctx;
    const sim_file_t *f = find(fs, name);
    if (!f || f->unreadable || f->size < n) {
        return false;
    }
    uint8_t *tmp = malloc(f->size);
    fill_rom(tmp, f);
    memcpy(buf, tmp, n);
    free(tmp);
    return true;
}

static bool fs_read_all(void *ctx, const char *name, uint8_t *buf, size_t cap,
                        uint32_t *out_len) {
    sim_fs_t *fs = ctx;
    const sim_file_t *f = find(fs, name);
    if (!f || f->unreadable || f->size > cap) {
        return false;
    }
    fill_rom(buf, f);
    *out_len = f->size;
    return true;
}

static const rom_io_t IO = { fs_next, fs_read_head, fs_read_all };

/* ---- what a real drive looks like --------------------------------------- */

/* A file whose header is perfect but whose logo is not the real one. */
static const sim_file_t VOLUME[] = {
    { "zelda_demo.mb",   0x2000,  true,  0x00, 0x1000, false },
    { "aaa_first.gba",   0x1000,  true,  0x00, 0,      false },
    { "notes.txt",       0x40,    false, 0x00, 0,      false },
    { "._zelda_demo.mb", 0x1000,  false, 0x00, 0,      false }, /* macOS fork */
    { ".hidden.mb",      0x1000,  true,  0x00, 0,      false },
    { "README",          0x100,   false, 0x00, 0,      false },
    { "huge_retail.gba", 0x800000, true, 0x00, 0,      false }, /* 8 MB dump  */
    { "truncated.mb",    0x40,    true,  0x00, 0,      false },
    { "garbage.mb",      0x1000,  false, 0x00, 0,      false },
    { "unreadable.mb",   0x1000,  true,  0x00, 0,      true  },
    { "padded.mb",       0x4000,  true,  0xFF, 0x900,  false },
    { "MiXeDcAsE.MB",    0x1000,  true,  0x00, 0,      false },
    { "wronglogo.mb",    0x1000,  true,  0x00, 0,      false },
};

static sim_fs_t make_fs(void) {
    sim_fs_t fs = { VOLUME, sizeof VOLUME / sizeof VOLUME[0], 0 };
    return fs;
}

/* ---- tests -------------------------------------------------------------- */

static void test_names(void) {
    printf("filename filtering\n");

    check("plain .mb", rom_name_is_program("game.mb"), 1);
    check("plain .gba", rom_name_is_program("game.gba"), 1);
    check("uppercase", rom_name_is_program("GAME.MB"), 1);
    check("mixed case", rom_name_is_program("Game.Gba"), 1);
    check("dots in the stem", rom_name_is_program("my.cool.game.mb"), 1);

    check("text file", rom_name_is_program("notes.txt"), 0);
    check("no extension", rom_name_is_program("README"), 0);
    check("trailing dot", rom_name_is_program("game."), 0);
    check("extension only", rom_name_is_program(".mb"), 0);
    check("empty", rom_name_is_program(""), 0);
    check("null", rom_name_is_program(NULL), 0);

    /* FAT and macOS both leave things that would otherwise match. */
    check("dot directory", rom_name_is_program("."), 0);
    check("parent directory", rom_name_is_program(".."), 0);
    check("resource fork", rom_name_is_program("._game.mb"), 0);
    check("substring is not a suffix", rom_name_is_program("game.mbx"), 0);
}

static void test_validate(void) {
    printf("header validation\n");

    uint8_t hdr[MB_HEADER];
    memset(hdr, 0, sizeof hdr);
    hdr[3] = 0xEA;
    hdr[0xB2] = 0x96;
    memcpy(hdr + 0xA0, "TESTROM     ", 12);
    hdr[0xBD] = rom_header_checksum(hdr);

    check("good header", rom_validate(hdr, sizeof hdr, 0x1000), ROM_OK);

    /* The complement byte is derived from the header, so editing a title
     * without recomputing it must be caught. */
    hdr[0xA0] = 'X';
    check("edited title without recompute",
          rom_validate(hdr, sizeof hdr, 0x1000), ROM_BAD_CHECKSUM);
    hdr[0xBD] = rom_header_checksum(hdr);
    check("recomputed is valid again", rom_validate(hdr, sizeof hdr, 0x1000), ROM_OK);
    memcpy(hdr + 0xA0, "TESTROM     ", 12);
    hdr[0xBD] = rom_header_checksum(hdr);

    hdr[0xB2] = 0x00;
    check("missing fixed byte", rom_validate(hdr, sizeof hdr, 0x1000), ROM_BAD_HEADER);
    hdr[0xB2] = 0x96;

    hdr[3] = 0x00;
    check("entry is not a branch", rom_validate(hdr, sizeof hdr, 0x1000), ROM_BAD_HEADER);
    hdr[3] = 0xEA;

    /* Known-good vector: an all-zero header with only the fixed byte set has
     * a checksum of 0xE7, because 0x00 - 0x19 - 0x96 = 0x51... verify against
     * the formula rather than a magic number. */
    uint8_t zero[MB_HEADER];
    memset(zero, 0, sizeof zero);
    zero[0xB2] = 0x96;
    check("checksum is a pure function of 0xA0..0xBC",
          rom_header_checksum(zero), (uint8_t)(0u - (uint8_t)(0x96u + 0x19u)));
    check("null header checksum is safe", rom_header_checksum(NULL), 0);

    check("too small", rom_validate(hdr, sizeof hdr, 0x40), ROM_BAD_SIZE);
    check("larger than EWRAM", rom_validate(hdr, sizeof hdr, MB_MAX_SIZE + 1), ROM_BAD_SIZE);
    check("exactly EWRAM is fine", rom_validate(hdr, sizeof hdr, MB_MAX_SIZE), ROM_OK);
    check("short header", rom_validate(hdr, 8, 0x1000), ROM_UNREADABLE);
    check("null header", rom_validate(NULL, MB_HEADER, 0x1000), ROM_UNREADABLE);
}

static void test_padding_strip(void) {
    printf("padding\n");

    static uint8_t buf[0x4000];
    memset(buf, 0, sizeof buf);
    buf[3] = 0xEA;
    buf[0xB2] = 0x96;
    for (uint32_t i = MB_HEADER; i < 0x900; i++) {
        buf[i] = 0x5A;
    }
    memset(buf + 0x900, 0xFF, sizeof buf - 0x900);

    uint32_t eff = rom_effective_size(buf, sizeof buf);
    printf("  0x%zX on disk -> 0x%X effective\n", sizeof buf, eff);
    check("padding stripped", eff < 0x4000, 1);
    check("keeps the real bytes", eff >= 0x900, 1);
    check("16-byte aligned", (long)((eff - MB_HEADER) % 16), 0);

    /*
     * A file with no trailing padding at all must still come back aligned.
     * This is the case the fixtures above all missed: every one of them had
     * padding, so the rounded-up value was always below the file size and the
     * old size cap never bit. A 1992-byte ROM with no padding rounds up past
     * its own length, and capping there returned an unaligned result.
     */
    memset(buf, 0, sizeof buf);
    buf[3] = 0xEA;
    buf[0xB2] = 0x96;
    for (uint32_t i = MB_HEADER; i < 1992; i++) {
        buf[i] = (uint8_t)(0x30 + (i % 0x40));   /* never 0x00 or 0xFF */
    }
    uint32_t unpadded = rom_effective_size(buf, 1992);
    check("unpadded rom rounds up rather than being capped", unpadded, 2000);
    check("and is aligned", (long)((unpadded - MB_HEADER) % 16), 0);
    check("mb_padded_size agrees", (long)mb_padded_size(1992), (long)unpadded);

    /* A file that is all padding after the header must not strip into it. */
    memset(buf + MB_HEADER, 0x00, sizeof buf - MB_HEADER);
    check("never strips below the minimum",
          rom_effective_size(buf, sizeof buf) >= MB_MIN_SIZE, 1);

    /* Content ending in a legitimate 0xFF byte still gets it back via the
     * 16-byte round-up, so stripping cannot eat real data. */
    memset(buf, 0, sizeof buf);
    for (uint32_t i = MB_HEADER; i < 0x500; i++) buf[i] = 0xFF;
    buf[0x4FF] = 0xFF;
    check("round-up protects real trailing bytes",
          rom_effective_size(buf, 0x500) >= MB_MIN_SIZE, 1);
}

static void test_scan(void) {
    printf("directory scan\n");

    sim_fs_t fs = make_fs();
    rom_catalog_t cat;
    rom_catalog_scan(&cat, &IO, &fs, 0);

    printf("  %u listed, %u playable, %u rejected\n",
           cat.count, cat.playable, cat.rejected);

    /* Only files with the right extension are listed at all. */
    for (uint16_t i = 0; i < cat.count; i++) {
        if (!rom_name_is_program(cat.entries[i].name)) {
            printf("  FAIL listed a non-program: %s\n", cat.entries[i].name);
            g_fail++;
        }
    }
    /* 9 of the 13 files have a program extension and no leading dot. */
    check("only programs listed", cat.count, 9);
    check("playable count", cat.playable, 5);
    check("rejected counted", cat.rejected, 4);

    /* Sorted case-insensitively, so MiXeDcAsE lands where a user expects. */
    check_str("first entry", cat.entries[0].name, "aaa_first.gba");
    for (uint16_t i = 1; i < cat.count; i++) {
        /* strcasecmp equivalent, ASCII only */
        const char *a = cat.entries[i - 1].name, *b = cat.entries[i].name;
        int cmp = 0;
        while (*a && *b && !cmp) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            cmp = ca - cb; a++; b++;
        }
        if (cmp > 0) {
            printf("  FAIL out of order: %s before %s\n",
                   cat.entries[i - 1].name, cat.entries[i].name);
            g_fail++;
            break;
        }
    }

    /* Bad files are listed with a reason rather than silently hidden: a user
     * who dragged a file across needs to know why it will not run. */
    bool saw_oversize = false, saw_garbage = false, saw_unreadable = false;
    bool saw_truncated = false;
    for (uint16_t i = 0; i < cat.count; i++) {
        const rom_entry_t *e = &cat.entries[i];
        if (strcmp(e->name, "huge_retail.gba") == 0) {
            saw_oversize = e->status == ROM_BAD_SIZE;
        }
        if (strcmp(e->name, "garbage.mb") == 0) {
            saw_garbage = e->status == ROM_BAD_HEADER;
        }
        if (strcmp(e->name, "unreadable.mb") == 0) {
            saw_unreadable = e->status == ROM_UNREADABLE;
        }
        /* A 64-byte download is diagnosed by size, not by a failed read. */
        if (strcmp(e->name, "truncated.mb") == 0) {
            saw_truncated = e->status == ROM_BAD_SIZE;
        }
    }
    check("8 MB dump flagged by size", saw_oversize, 1);
    check("junk flagged by header", saw_garbage, 1);
    check("unreadable flagged", saw_unreadable, 1);
    check("truncated diagnosed by size", saw_truncated, 1);

    printf("  listing:\n");
    for (uint16_t i = 0; i < cat.count; i++) {
        printf("    %-18s %7u B  %s\n", cat.entries[i].name,
               cat.entries[i].size, rom_status_name(cat.entries[i].status));
    }
}

static void test_logo(void) {
    printf("logo fingerprint\n");

    uint8_t good[MB_HEADER], other[MB_HEADER];
    memset(good, 0, sizeof good);
    memset(other, 0, sizeof other);
    fill_logo(good, 0x51);
    fill_logo(other, 0x11);

    uint32_t ref = rom_logo_fingerprint(good);
    check("fingerprint is stable", rom_logo_fingerprint(good), (long)ref);
    check("different logo, different fingerprint",
          rom_logo_fingerprint(other) != ref, 1);
    check("null is safe", rom_logo_fingerprint(NULL), 0);

    /* It must cover the logo area and nothing else: editing the title or the
     * entry point must not move the fingerprint, or every rebuild would look
     * like a bad logo. */
    uint8_t edited[MB_HEADER];
    memcpy(edited, good, sizeof edited);
    memcpy(edited + 0xA0, "DIFFERENT   ", 12);
    edited[0] = 0x99;
    edited[0xBD] = rom_header_checksum(edited);
    check("fingerprint ignores the rest of the header",
          rom_logo_fingerprint(edited), (long)ref);

    /* CRC32 sanity against a published vector. */
    check("crc32 of \"123456789\"",
          (long)rom_crc32((const uint8_t *)"123456789", 9), 0xCBF43926);
    check("crc32 of nothing", (long)rom_crc32((const uint8_t *)"", 0), 0);

    /* ---- in the scan ---- */

    sim_fs_t fs = make_fs();
    rom_catalog_t cat;

    /* With no reference, the check must be silent: a false rejection is worse
     * than the failure it would have prevented. */
    rom_catalog_scan(&cat, &IO, &fs, 0);
    for (uint16_t i = 0; i < cat.count; i++) {
        if (cat.entries[i].status == ROM_BAD_LOGO) {
            check("no logo verdict without a reference", 0, 1);
            break;
        }
    }
    check("wronglogo passes when unknown", cat.playable, 5);

    /* With one, the odd file out is caught and nothing else changes. */
    fs = make_fs();
    rom_catalog_scan(&cat, &IO, &fs, ref);
    check("reference echoed for the UI", (long)cat.logo_reference, (long)ref);
    check("one fewer playable", cat.playable, 4);

    bool caught = false;
    for (uint16_t i = 0; i < cat.count; i++) {
        if (strcmp(cat.entries[i].name, "wronglogo.mb") == 0) {
            caught = cat.entries[i].status == ROM_BAD_LOGO;
        }
        /* A file that already failed for a better reason keeps that reason. */
        if (strcmp(cat.entries[i].name, "garbage.mb") == 0) {
            check("worse faults win", cat.entries[i].status, ROM_BAD_HEADER);
        }
    }
    check("bad logo caught", caught, 1);
    check_str("and explains itself", rom_status_name(ROM_BAD_LOGO),
              "bad logo - repair on PC");

    /* A bad-logo file must not be stageable: it would transfer fully and then
     * be rejected by the console. */
    static uint8_t buf[MB_MAX_SIZE];
    uint32_t len = 0;
    for (uint16_t i = 0; i < cat.count; i++) {
        if (strcmp(cat.entries[i].name, "wronglogo.mb") == 0) {
            check("refuses to stage a bad logo",
                  rom_stage(&cat, i, &IO, &fs, buf, sizeof buf, &len), 0);
        }
    }
}

static void test_truncation(void) {
    printf("more files than the catalogue holds\n");

    static sim_file_t many[ROM_MAX_ENTRIES + 8];
    static char names[ROM_MAX_ENTRIES + 8][ROM_NAME_MAX];
    for (size_t i = 0; i < sizeof many / sizeof many[0]; i++) {
        snprintf(names[i], ROM_NAME_MAX, "rom%02zu.mb", i);
        many[i].name = names[i];
        many[i].size = 0x1000;
        many[i].valid_header = true;
        many[i].pad_byte = 0;
        many[i].real_bytes = 0;
        many[i].unreadable = false;
    }

    sim_fs_t fs = { many, sizeof many / sizeof many[0], 0 };
    rom_catalog_t cat;
    rom_catalog_scan(&cat, &IO, &fs, 0);

    check("stops at capacity", cat.count, ROM_MAX_ENTRIES);
    check("truncation reported", cat.truncated, 1);
    /* The flag must be right even though scanning continued past the limit. */
    check("no overflow", cat.count <= ROM_MAX_ENTRIES, 1);
}

static void test_stage(void) {
    printf("staging into RAM\n");

    sim_fs_t fs = make_fs();
    rom_catalog_t cat;
    rom_catalog_scan(&cat, &IO, &fs, 0);

    static uint8_t buf[MB_MAX_SIZE];
    uint32_t len = 0;

    uint16_t padded_idx = 0xFFFF, bad_idx = 0xFFFF;
    for (uint16_t i = 0; i < cat.count; i++) {
        if (strcmp(cat.entries[i].name, "padded.mb") == 0) padded_idx = i;
        if (strcmp(cat.entries[i].name, "garbage.mb") == 0) bad_idx = i;
    }

    check("stages a good rom", rom_stage(&cat, padded_idx, &IO, &fs, buf,
                                         sizeof buf, &len), 1);
    printf("  padded.mb: 0x4000 on disk, staged 0x%X\n", len);
    check("padding not transferred", len < 0x4000, 1);
    check("header preserved", buf[0xB2], 0x96);
    check("16-byte aligned", (long)((len - MB_HEADER) % 16), 0);

    /* An invalid entry must be refused before a console is powered on. */
    check("refuses an invalid rom",
          rom_stage(&cat, bad_idx, &IO, &fs, buf, sizeof buf, &len), 0);

    /* A buffer that cannot hold the padded size must be refused, not
     * partially filled: mb_send reads whole words to the padded end. */
    check("refuses a short buffer",
          rom_stage(&cat, padded_idx, &IO, &fs, buf, 0x100, &len), 0);

    check("out of range index",
          rom_stage(&cat, 999, &IO, &fs, buf, sizeof buf, &len), 0);
    check("null buffer",
          rom_stage(&cat, padded_idx, &IO, &fs, NULL, sizeof buf, &len), 0);
}

/* The staged buffer must actually multiboot, which is the whole point. */
static void test_stage_then_send(void) {
    printf("staged rom feeds multiboot\n");

    sim_fs_t fs = make_fs();
    rom_catalog_t cat;
    rom_catalog_scan(&cat, &IO, &fs, 0);

    static uint8_t buf[MB_MAX_SIZE];
    uint32_t len = 0;
    uint16_t idx = 0xFFFF;
    for (uint16_t i = 0; i < cat.count; i++) {
        if (strcmp(cat.entries[i].name, "zelda_demo.mb") == 0) idx = i;
    }
    check("found it", idx != 0xFFFF, 1);
    check("staged", rom_stage(&cat, idx, &IO, &fs, buf, sizeof buf, &len), 1);

    /* mb_send validates size itself; confirm the staged length is acceptable
     * rather than duplicating the whole simulated GBA here. */
    check("length within multiboot limits",
          len >= MB_MIN_SIZE && len <= MB_MAX_SIZE, 1);
    check("padded size does not exceed EWRAM",
          (long)(mb_padded_size(len) <= MB_MAX_SIZE), 1);
    printf("  zelda_demo.mb staged to 0x%X, pads to 0x%zX\n",
           len, mb_padded_size(len));
}

int main(void) {
    test_names();
    test_validate();
    test_padding_strip();
    test_scan();
    test_logo();
    test_truncation();
    test_stage();
    test_stage_then_send();
    printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail != 0;
}
