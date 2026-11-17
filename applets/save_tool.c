/*
 * save_tool.c -- a cartridge's save, backed up to the microSD card and
 * restored from it, through the GBA itself (doc/SAVETOOL.md).
 *
 * An add-on: it is in the menu only when its GBA half, savetool/savetool.bin
 * (rom/savetool.c, built by `make -C rom savetool`), is on the card. Then:
 *
 *   1. the GBA is turned on holding START + SELECT, the cartridge in: the
 *      BIOS skips the cartridge and waits for a multiboot program;
 *   2. the board sends the memory agent, and the blob after it;
 *   3. the blob finds the save library the game names (SRAM or flash), and
 *      the flash chip, which sets the size;
 *   4. Back up: the whole save, to saves/<game code>-<n>.sav;
 *      Restore: a .sav of the right size from saves/, written sector by
 *      sector, each verified on the GBA -- and the save it replaces backed up
 *      first, always.
 *
 * Between the two, the .sav is an ordinary file: PKHeX (or anything else)
 * edits it on a computer, with the card in a reader or through the board's
 * USB drive. Every step runs from tick(), one step a tick, so the page shows
 * progress and nothing blocks the UI for more than a sector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applet.h"
#include "../rom/savetool.h"

#define BLOB_PATH  "savetool/savetool.bin"
#define SAVE_DIR   "saves"
#define BLOB_MAX   8192
#define SPAN       0x400000u              /* ROM scanned per DETECT call */
#define ROM_MAX    0x2000000u
#define MAX_FILES  24

enum {
    S_IDLE,          /* instructions; confirm to start                   */
    S_START,         /* send the agent, next tick                         */
    S_LOAD,          /* the blob onto the GBA                             */
    S_DETECT,        /* find the save library, a span a tick               */
    S_ID,            /* the flash chip                                    */
    S_HEADER,        /* the game's title and code                         */
    S_READY,         /* the menu                                          */
    S_BACKUP_READ,   /* the save into the GBA's buffer                    */
    S_BACKUP_COPY,   /* ... and out to the card, 4 KB a tick              */
    S_PICK,          /* which .sav to restore                             */
    S_CONFIRM,       /* sure?                                             */
    S_WRITE,         /* a sector a tick                                   */
    S_FAILED,
};

enum { ACT_BACKUP, ACT_RESTORE, ACT_COUNT };
static const char *const k_actions[ACT_COUNT] = { "Back up to card", "Restore from card" };
static const char *const k_types[] = { "none", "SRAM 32 KB", "flash 64 KB", "flash 128 KB", "EEPROM" };

static int          s_state;
static uint8_t      s_cursor;
static uint32_t     s_entry;              /* svt_main on the GBA               */
static svt_block_t  s_blk;
static uint32_t     s_span, s_chunk;
static char         s_title[13], s_code[5];
static char         s_path[sizeof SAVE_DIR + APPLET_FILE_NAME];           /* the file being written or read    */
static char         s_restore[sizeof SAVE_DIR + APPLET_FILE_NAME];        /* the file to restore, once picked  */
static bool         s_restore_after;      /* this backup comes before a restore */
static char         s_msg[sizeof SAVE_DIR + APPLET_FILE_NAME + 16];
static bool         s_msg_good;
static char         s_files[MAX_FILES][APPLET_FILE_NAME];
static size_t       s_nfiles;
static uint32_t     s_words[SVT_SECTOR / 4];

static void fail(const char *why) {
    snprintf(s_msg, sizeof s_msg, "%s", why);
    s_msg_good = false;
    s_state = S_FAILED;
}

static void say(bool good, const char *fmt, const char *arg) {
    snprintf(s_msg, sizeof s_msg, fmt, arg);
    s_msg_good = good;
}

/* ---- the blob's control block ------------------------------------------- */

static bool block_read(mba_client_t *c) {
    uint32_t w[sizeof(svt_block_t) / 4];
    if (mba_read(c, SVT_BASE, w, sizeof w / 4) != MBA_OK) return false;
    memcpy(&s_blk, w, sizeof s_blk);
    return true;
}

/* Run one command; the block comes back in s_blk. run_us: how long it may
 * take on the GBA before the agent is asked whether it is back. */
static bool command(mba_client_t *c, uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t run_us) {
    const uint32_t args[3] = { cmd, arg0, arg1 };
    const uint32_t before = s_blk.done;
    if (mba_write(c, SVT_BASE + 16, args, 3) != MBA_OK) return false;
    if (mba_call(c, s_entry, run_us) != MBA_OK) return false;
    return block_read(c) && s_blk.done == before + 1;
}

/* ---- files ----------------------------------------------------------------- */

/* saves/<code>-<n>.sav, n one past the highest there */
static void next_backup_path(const applet_env_t *env) {
    char names[MAX_FILES][APPLET_FILE_NAME];
    const size_t n = env->file_list(SAVE_DIR, ".sav", names, MAX_FILES);
    int top = 0;
    const size_t cl = strlen(s_code);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(names[i], s_code, cl) == 0 && names[i][cl] == '-') {
            const int k = atoi(names[i] + cl + 1);
            if (k > top) top = k;
        }
    }
    snprintf(s_path, sizeof s_path, SAVE_DIR "/%s-%03d.sav", s_code, top + 1);
}

/* the .sav files of exactly this save's size */
static void list_restorable(const applet_env_t *env) {
    char names[MAX_FILES][APPLET_FILE_NAME];
    const size_t n = env->file_list(SAVE_DIR, ".sav", names, MAX_FILES);
    s_nfiles = 0;
    for (size_t i = 0; i < n; i++) {
        char p[sizeof SAVE_DIR + APPLET_FILE_NAME];
        snprintf(p, sizeof p, SAVE_DIR "/%.*s", APPLET_FILE_NAME - 1, names[i]);
        if (env->file_size(p) == (long)s_blk.save_size) {
            memcpy(s_files[s_nfiles++], names[i], APPLET_FILE_NAME);
        }
    }
}

/* ---- the steps -------------------------------------------------------------- */

static void step_load(const applet_env_t *env, mba_client_t *c) {
    static uint8_t blob[BLOB_MAX];
    const long n = env->file_read(BLOB_PATH, 0, blob, sizeof blob);
    if (n < (long)sizeof(svt_block_t) || n >= (long)sizeof blob) {
        fail("savetool.bin unreadable");
        return;
    }
    const size_t words = ((size_t)n + 3) / 4;
    memset(blob + n, 0, words * 4 - (size_t)n);
    if (mba_write(c, SVT_BASE, (const uint32_t *)(const void *)blob, words) != MBA_OK || !block_read(c)) {
        fail("could not load the blob");
        return;
    }
    if (s_blk.magic != SVT_MAGIC || !(s_blk.entry & 1)) {
        fail("savetool.bin is not one");
        return;
    }
    s_entry = s_blk.entry;
    s_span = 0;
    s_state = S_DETECT;
}

static void step_detect(mba_client_t *c) {
    if (!command(c, SVT_DETECT, s_span, SPAN, 900000)) {
        fail("the GBA stopped answering");
        return;
    }
    if (s_blk.save_type != SVT_NONE) {
        s_state = S_ID;
        return;
    }
    s_span += SPAN;
    if (s_span >= ROM_MAX) fail("no save in this game");
}

static void step_id(mba_client_t *c) {
    if (!command(c, SVT_FLASH_ID, 0, 0, 100000)) {
        fail("the GBA stopped answering");
        return;
    }
    s_state = S_HEADER;
}

static void step_header(mba_client_t *c) {
    uint32_t w[4];
    if (mba_read(c, 0x080000A0u, w, 4) != MBA_OK) {
        fail("could not read the game");
        return;
    }
    const char *h = (const char *)w;
    memcpy(s_title, h, 12);
    s_title[12] = '\0';
    memcpy(s_code, h + 12, 4);
    s_code[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (s_code[i] < '0' || s_code[i] > 'Z') s_code[i] = '_';   /* a file name, always */
    }
    if (s_blk.save_type == SVT_EEPROM || s_blk.save_size == 0) {
        fail("EEPROM saves: not yet");
        return;
    }
    s_state = S_READY;
}

static void begin_backup(const applet_env_t *env, bool before_restore) {
    next_backup_path(env);
    s_restore_after = before_restore;
    s_state = S_BACKUP_READ;
    s_msg[0] = '\0';
}

static void step_backup_read(mba_client_t *c) {
    if (!command(c, SVT_READ, 0, 0, 400000) || s_blk.status != SVT_ST_OK) {
        fail("could not read the save");
        return;
    }
    s_chunk = 0;
    s_state = S_BACKUP_COPY;
}

static void step_backup_copy(const applet_env_t *env, mba_client_t *c) {
    const uint32_t off = s_chunk * SVT_SECTOR;
    if (mba_read(c, SVT_BUF + off, s_words, SVT_SECTOR / 4) != MBA_OK) {
        fail("the link dropped");
        return;
    }
    if (!env->file_write(s_path, off, s_words, SVT_SECTOR)) {
        fail("could not write the card");
        return;
    }
    if (++s_chunk * SVT_SECTOR < s_blk.save_size) return;
    if (s_restore_after) {
        s_chunk = 0;
        s_state = S_WRITE;
        return;
    }
    say(true, "saved %s", s_path + sizeof SAVE_DIR);
    s_state = S_READY;
}

static void step_write(const applet_env_t *env, mba_client_t *c) {
    const uint32_t off = s_chunk * SVT_SECTOR;
    if (env->file_read(s_restore, off, s_words, SVT_SECTOR) != (long)SVT_SECTOR) {
        fail("could not read the file");
        return;
    }
    if (mba_write(c, SVT_BUF + off, s_words, SVT_SECTOR / 4) != MBA_OK) {
        fail("the link dropped");
        return;
    }
    if (!command(c, SVT_WRITE, s_chunk, 0, 300000)) {
        fail("the GBA stopped answering");
        return;
    }
    if (s_blk.status != SVT_ST_OK) {
        char why[48];
        snprintf(why, sizeof why, "sector %lu: %s", (unsigned long)s_chunk,
                 s_blk.status == SVT_ST_VERIFY ? "reads back wrong" :
                 s_blk.status == SVT_ST_TIMEOUT ? "flash timed out" : "refused");
        fail(why);
        return;
    }
    if (++s_chunk * SVT_SECTOR < s_blk.save_size) return;
    say(true, "restored %s", s_restore + sizeof SAVE_DIR);
    s_state = S_READY;
}

/* ---- the applet ------------------------------------------------------------- */

static bool has_card(const applet_env_t *env);

static bool st_enter(const applet_env_t *env) {
    s_state = S_IDLE;
    s_cursor = 0;
    s_msg[0] = '\0';
    memset(&s_blk, 0, sizeof s_blk);
    return env->agent_start && env->agent && has_card(env);
}

static void st_exit(void) {
    const applet_env_t *env = applet_env();
    if (env->agent_stop) env->agent_stop();
}

static bool has_card(const applet_env_t *env) {
    return env->file_read && env->file_write && env->file_size && env->file_list;
}

static void st_tick(const applet_env_t *env) {
    if (s_state == S_IDLE || s_state == S_FAILED) return;
    if (!has_card(env) || !env->agent_start) {
        fail(!has_card(env) ? "no microSD card" : "no link port");   /* whatever was asked */
        return;
    }
    if (s_state == S_START) {
        const char *why = env->agent_start();
        if (why) {
            fail(why);
            return;
        }
        s_state = S_LOAD;
        return;
    }
    mba_client_t *c = env->agent ? env->agent() : NULL;
    if (!c) {
        if (s_state != S_IDLE && s_state != S_FAILED) fail("the agent is gone");
        return;
    }
    switch (s_state) {
    case S_LOAD:        step_load(env, c); break;
    case S_DETECT:      step_detect(c); break;
    case S_ID:          step_id(c); break;
    case S_HEADER:      step_header(c); break;
    case S_BACKUP_READ: step_backup_read(c); break;
    case S_BACKUP_COPY: step_backup_copy(env, c); break;
    case S_WRITE:       step_write(env, c); break;
    default: break;
    }
}

static bool st_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    if (in == APPLET_IN_MOVE && value) {
        const int d = value > 0 ? 1 : -1;
        if (s_state == S_READY) s_cursor = (uint8_t)((s_cursor + ACT_COUNT + d) % ACT_COUNT);
        if (s_state == S_PICK && s_nfiles) s_cursor = (uint8_t)((s_cursor + s_nfiles + d) % s_nfiles);
        return true;
    }
    if (in == APPLET_IN_BACK && (s_state == S_PICK || s_state == S_CONFIRM)) {
        s_state = S_READY;
        s_cursor = 0;
        return true;
    }
    if (in != APPLET_IN_CONFIRM) return false;     /* Back leaves */
    switch (s_state) {
    case S_IDLE:
    case S_FAILED:
        s_msg[0] = '\0';
        s_state = S_START;                          /* tick() sends, once "sending" is drawn */
        break;
    case S_READY:
        s_msg[0] = '\0';
        if (s_cursor == ACT_BACKUP) {
            begin_backup(env, false);
        } else {
            list_restorable(env);
            s_cursor = 0;
            s_state = S_PICK;
        }
        break;
    case S_PICK:
        if (s_nfiles) {
            snprintf(s_restore, sizeof s_restore, SAVE_DIR "/%.*s", APPLET_FILE_NAME - 1, s_files[s_cursor]);
            s_state = S_CONFIRM;
        }
        break;
    case S_CONFIRM:
        begin_backup(env, true);                    /* the save it replaces, kept first */
        break;
    default:
        break;
    }
    return true;
}

static void st_page(const applet_env_t *env, ui_page_t *p) {
    const char *key = env->confirm ? env->confirm : "A";
    switch (s_state) {
    case S_IDLE:
        ui_page_add(p, COL_TEXT, "cartridge in; hold");
        ui_page_add(p, COL_TEXT, "START+SELECT while");
        ui_page_add(p, COL_TEXT, "turning the GBA on;");
        ui_page_add(p, COL_TEXT, "link it to the board");
        snprintf(p->footer, sizeof p->footer, "%s start", key);
        break;
    case S_START: case S_LOAD:
        ui_page_add(p, COL_WARN, "sending...");
        break;
    case S_DETECT: case S_ID: case S_HEADER:
        ui_page_add(p, COL_WARN, "looking at the game");
        ui_page_add(p, COL_TEXT_DIM, "ROM %lu MB", (unsigned long)(s_span >> 20));
        break;
    case S_READY:
        ui_page_add(p, COL_GOOD, "%s %s", s_code, s_title);
        ui_page_add(p, COL_TEXT_DIM, "%s", k_types[s_blk.save_type < 5 ? s_blk.save_type : 0]);
        for (int i = 0; i < ACT_COUNT; i++)
            ui_page_add(p, i == s_cursor ? COL_CURSOR : COL_TEXT, "%c %s", i == s_cursor ? '>' : ' ', k_actions[i]);
        if (s_msg[0]) ui_page_add(p, s_msg_good ? COL_GOOD : COL_BAD, "%s", s_msg);
        snprintf(p->footer, sizeof p->footer, "%s do it", key);
        break;
    case S_BACKUP_READ: case S_BACKUP_COPY:
        ui_page_add(p, COL_WARN, s_restore_after ? "keeping the old save" : "backing up");
        ui_page_add(p, COL_TEXT_DIM, "%lu of %lu KB", (unsigned long)(s_chunk * 4),
                    (unsigned long)(s_blk.save_size / 1024));
        break;
    case S_PICK:
        if (!s_nfiles) {
            ui_page_add(p, COL_BAD, "no %lu KB .sav in", (unsigned long)(s_blk.save_size / 1024));
            ui_page_add(p, COL_BAD, "saves/ on the card");
        }
        for (size_t i = 0; i < s_nfiles && i < 6; i++)
            ui_page_add(p, i == s_cursor ? COL_CURSOR : COL_TEXT, "%c %s", i == s_cursor ? '>' : ' ', s_files[i]);
        snprintf(p->footer, sizeof p->footer, "%s pick", key);
        break;
    case S_CONFIRM:
        ui_page_add(p, COL_WARN, "write to the cart:");
        ui_page_add(p, COL_TEXT, "%s", s_restore + sizeof SAVE_DIR);
        ui_page_add(p, COL_TEXT_DIM, "its save now is");
        ui_page_add(p, COL_TEXT_DIM, "backed up first");
        snprintf(p->footer, sizeof p->footer, "%s write", key);
        break;
    case S_WRITE:
        ui_page_add(p, COL_WARN, "writing: keep it on");
        ui_page_add(p, COL_TEXT_DIM, "%lu of %lu KB", (unsigned long)(s_chunk * 4),
                    (unsigned long)(s_blk.save_size / 1024));
        break;
    case S_FAILED:
        ui_page_add(p, COL_BAD, "%s", s_msg);
        snprintf(p->footer, sizeof p->footer, "%s again", key);
        break;
    default:
        break;
    }
}

const applet_t applet_save_tool = {
    .name        = "Save Tool",
    .requires    = HW_CAP_DISPLAY | HW_CAP_LINK_PINS,
    .needs_file  = BLOB_PATH,
    .absent_hint = "put savetool/savetool.bin on the card",
    .enter       = st_enter,
    .exit        = st_exit,
    .tick        = st_tick,
    .input       = st_input,
    .page        = st_page,
};
