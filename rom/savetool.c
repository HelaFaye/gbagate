/*
 * savetool.c -- a cartridge's save, read and written from the GBA itself,
 * for the board's Save Tool applet (doc/SAVETOOL.md).
 *
 * The board sends the memory agent (rom/agent.c), then this blob into EWRAM
 * at SVT_BASE with the agent's WRITE, and drives it through its control
 * block: it writes a command and its arguments, runs svt_main with the
 * agent's CALL, and reads the block back. Each command is short (a 4 KB
 * sector at most, about 100 ms), so no CALL outlasts the board's wait and
 * the board can show progress.
 *
 * Save memory is on the cartridge's 8-bit bus at 0x0E000000 and is only
 * ever accessed a byte at a time, from RAM -- this code runs from EWRAM, as
 * it must: flash commands cannot be issued while executing from the ROM.
 *
 *   SVT_DETECT   scan ROM [arg0, arg0 + arg1) for the save library's ID
 *                string (SRAM_V, SRAM_F_V, FLASH_V, FLASH512_V, FLASH1M_V,
 *                EEPROM_V); sets save_type and save_size when found
 *   SVT_FLASH_ID the flash chip's manufacturer and device (ID mode); a chip
 *                we know sets the size, over the library string
 *   SVT_READ     the whole save (save_size bytes) into the buffer
 *   SVT_WRITE    sector arg0 (4 KB) from buffer + arg0 * 4096: erase,
 *                program, read back; status says whether it matches
 *
 * EEPROM saves (a serial protocol through DMA) are detected and reported,
 * not read or written: no Generation 3 Pokemon game uses one.
 */
#include <stdint.h>

#include "savetool.h"

/* Every GBA address goes through SVT_PTR, so a host test can compile this
 * file against its own model of the GBA's memory (test/test_save_tool.c). */
#ifndef SVT_PTR
#define SVT_PTR(a) ((volatile uint8_t *)(uintptr_t)(a))
#endif
#define SAVE        SVT_PTR(0x0E000000u)
#define ROM8(off)   SVT_PTR(0x08000000u + (off))
#define REG_WAITCNT (*(volatile uint16_t *)SVT_PTR(0x04000204u))
#define REG_IME     (*(volatile uint16_t *)SVT_PTR(0x04000208u))

static void flash_cmd(uint8_t c) {
    SAVE[0x5555] = 0xAA;
    SAVE[0x2AAA] = 0x55;
    SAVE[0x5555] = c;
}

static void flash_bank(uint32_t bank) {
    flash_cmd(0xB0);
    SAVE[0] = (uint8_t)bank;
}

/* Wait until addr reads back want (DQ7 polling, as the chips document it),
 * or about `spins` reads. */
static int flash_wait(volatile uint8_t *addr, uint8_t want, uint32_t spins) {
    while (spins--) {
        if (*addr == want) return 1;
    }
    return 0;
}

static uint32_t flash_id(void) {
    flash_cmd(0x90);                                 /* ID mode */
    for (volatile int i = 0; i < 2000; i++) {}      /* tID: 20 ms on Atmel; plenty elsewhere */
    const uint32_t id = (uint32_t)SAVE[0] | (uint32_t)SAVE[1] << 8;
    flash_cmd(0xF0);                                 /* back to reading */
    flash_cmd(0xF0);
    for (volatile int i = 0; i < 2000; i++) {}
    return id;
}

/* The flash parts GBA cartridges carry, by ID (maker | device << 8), from
 * their data sheets: the 128 KB ones need bank switching. */
static uint32_t chip_size(uint32_t id) {
    switch (id) {
    case 0x1362:                     /* Sanyo LE26FV10N1TS        */
    case 0x09C2:                     /* Macronix MX29L010         */
        return 0x20000;
    case 0x3D1F:                     /* Atmel AT29LV512           */
    case 0x1B32:                     /* Panasonic MN63F805MNP     */
    case 0xD4BF:                     /* SST SST39LVF512           */
    case 0x1CC2:                     /* Macronix MX29L512         */
        return 0x10000;
    default:
        return 0;
    }
}

/* ---- detect --------------------------------------------------------------- */

static int match(const volatile uint8_t *p, const char *s) {
    for (; *s; s++, p++)
        if (*p != (uint8_t)*s) return 0;
    return 1;
}

static void detect(svt_block_t *b) {
    const uint32_t end = b->arg0 + b->arg1;
    for (uint32_t off = b->arg0 & ~3u; off < end && off < 0x02000000; off += 4) {
        const volatile uint8_t *p = ROM8(off);
        const uint32_t w = *(const volatile uint32_t *)p;
        /* "SRAM", "FLAS", "EEPR" as little-endian words */
        if (w == 0x4D415253u) {
            if (match(p, "SRAM_V") || match(p, "SRAM_F_V")) {
                b->save_type = SVT_SRAM;
                b->save_size = 0x8000;
                b->found_at = off;
                return;
            }
        } else if (w == 0x53414C46u) {
            if (match(p, "FLASH1M_V")) {
                b->save_type = SVT_FLASH128;
                b->save_size = 0x20000;
                b->found_at = off;
                return;
            }
            if (match(p, "FLASH512_V") || match(p, "FLASH_V")) {
                b->save_type = SVT_FLASH64;
                b->save_size = 0x10000;
                b->found_at = off;
                return;
            }
        } else if (w == 0x52504545u) {
            if (match(p, "EEPROM_V")) {
                b->save_type = SVT_EEPROM;
                b->save_size = 0;                    /* 512 B or 8 KB: not read here */
                b->found_at = off;
                return;
            }
        }
    }
}

/* ---- read ----------------------------------------------------------------- */

static void read_all(svt_block_t *b) {
    uint8_t *buf = (uint8_t *)SVT_PTR(b->buf);
    if (b->save_type == SVT_SRAM || b->save_type == SVT_FLASH64) {
        for (uint32_t k = 0; k < b->save_size; k++) buf[k] = SAVE[k];
    } else if (b->save_type == SVT_FLASH128) {
        for (uint32_t bank = 0; bank < 2; bank++) {
            flash_bank(bank);
            for (uint32_t k = 0; k < 0x10000; k++) buf[bank * 0x10000 + k] = SAVE[k];
        }
        flash_bank(0);
    } else {
        b->status = SVT_ST_UNSUPPORTED;
        return;
    }
    b->status = SVT_ST_OK;
}

/* ---- write ---------------------------------------------------------------- */

static int is_atmel(uint32_t id) { return (id & 0xFF) == 0x1F; }

static uint32_t write_sector(svt_block_t *b) {
    const uint32_t sector = b->arg0;
    if (sector * SVT_SECTOR >= b->save_size) return SVT_ST_ARGS;
    const uint8_t *src = (const uint8_t *)SVT_PTR(b->buf) + sector * SVT_SECTOR;

    if (b->save_type == SVT_SRAM) {
        for (uint32_t k = 0; k < SVT_SECTOR; k++) SAVE[sector * SVT_SECTOR + k] = src[k];
    } else if (b->save_type == SVT_FLASH64 || b->save_type == SVT_FLASH128) {
        uint32_t off = sector * SVT_SECTOR;
        if (b->save_type == SVT_FLASH128) {
            flash_bank(off >> 16);
            off &= 0xFFFF;
        }
        volatile uint8_t *dst = &SAVE[off];
        if (is_atmel(b->chip_id)) {
            /* Atmel: 128-byte pages, programmed whole, no erase */
            for (uint32_t page = 0; page < SVT_SECTOR / 128; page++) {
                const uint16_t ime = REG_IME;
                REG_IME = 0;                         /* a page must not be interrupted */
                flash_cmd(0xA0);
                for (uint32_t k = 0; k < 128; k++) dst[page * 128 + k] = src[page * 128 + k];
                REG_IME = ime;
                if (!flash_wait(&dst[page * 128 + 127], src[page * 128 + 127], 400000))
                    return SVT_ST_TIMEOUT;
            }
        } else {
            /* Macronix, Sanyo, Panasonic: erase the 4 KB sector, then a byte at a time */
            flash_cmd(0x80);
            SAVE[0x5555] = 0xAA;
            SAVE[0x2AAA] = 0x55;
            *dst = 0x30;
            if (!flash_wait(dst, 0xFF, 2000000)) return SVT_ST_TIMEOUT;
            for (uint32_t k = 0; k < SVT_SECTOR; k++) {
                if (src[k] == 0xFF) continue;        /* erased already */
                flash_cmd(0xA0);
                dst[k] = src[k];
                if (!flash_wait(&dst[k], src[k], 20000)) return SVT_ST_TIMEOUT;
            }
        }
        if (b->save_type == SVT_FLASH128) flash_bank(0);
    } else {
        return SVT_ST_UNSUPPORTED;
    }

    /* read back */
    if (b->save_type == SVT_FLASH128) flash_bank((sector * SVT_SECTOR) >> 16);
    const uint32_t off = b->save_type == SVT_FLASH128 ? (sector * SVT_SECTOR) & 0xFFFF : sector * SVT_SECTOR;
    uint32_t bad = 0;
    for (uint32_t k = 0; k < SVT_SECTOR; k++) bad += SAVE[off + k] != src[k];
    if (b->save_type == SVT_FLASH128) flash_bank(0);
    b->mismatches = bad;
    return bad ? SVT_ST_VERIFY : SVT_ST_OK;
}

/* ---- entry ---------------------------------------------------------------- */

void svt_main(void) {
    svt_block_t *b = (svt_block_t *)SVT_PTR(SVT_BASE);
    const uint16_t wait = REG_WAITCNT;
    /* SRAM/flash: 8 wait states, as the cartridges need; ROM: 3,1 with prefetch */
    REG_WAITCNT = 0x4317;
    b->status = SVT_ST_OK;
    switch (b->cmd) {
    case SVT_DETECT:
        b->save_type = SVT_NONE;
        b->save_size = 0;
        detect(b);
        break;
    case SVT_FLASH_ID:
        b->chip_id = (b->save_type == SVT_FLASH64 || b->save_type == SVT_FLASH128) ? flash_id() : 0;
        /* The chip, not the game's library string, decides the size: some
         * games name FLASH512 on a 128 KB part, or the reverse. */
        b->chip_size = chip_size(b->chip_id);
        if (b->chip_size == 0x20000) { b->save_type = SVT_FLASH128; b->save_size = 0x20000; }
        if (b->chip_size == 0x10000) { b->save_type = SVT_FLASH64; b->save_size = 0x10000; }
        break;
    case SVT_READ:
        read_all(b);
        break;
    case SVT_WRITE:
        b->status = write_sector(b);
        break;
    default:
        b->status = SVT_ST_ARGS;
        break;
    }
    b->done++;
    REG_WAITCNT = wait;
}
