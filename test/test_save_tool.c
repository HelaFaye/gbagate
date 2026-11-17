/*
 * test_save_tool.c -- the Save Tool applet (applets/save_tool.c) end to end
 * on the host: the real applet, the real agent client (src/mbagent.c) over
 * the agent's real protocol core (rom/agent_proto.h), and the real blob
 * (rom/savetool.c, compiled here against a model of the GBA's memory), with
 * the card a temporary directory.
 *
 * The cartridge is modelled with SRAM: a byte store, which is what the host
 * can model honestly. Flash, with its command sequences and banks, is tested
 * against mGBA's chip models in test/mgba/savetool_emerald.py.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applet.h"
#include "ui_screens.h"

/* ---- the GBA ------------------------------------------------------------------ */

static uint8_t g_ewram[256 * 1024], g_rom[1 << 20], g_sram[0x8000], g_io[0x400], g_dummy[8];
static bool g_rom_has_save = true;

static uint8_t *gba(uint32_t a, uint32_t n) {
    if (a >= 0x02000000u && a - 0x02000000u + n <= sizeof g_ewram) return g_ewram + (a - 0x02000000u);
    if (a >= 0x08000000u && a < 0x0A000000u) {
        /* a ROM smaller than the space it is in: past its end, nothing */
        if (a - 0x08000000u + n <= sizeof g_rom) return g_rom + (a - 0x08000000u);
        memset(g_dummy, 0, sizeof g_dummy);
        return g_dummy;
    }
    if (a >= 0x0E000000u && a - 0x0E000000u + n <= sizeof g_sram) return g_sram + (a - 0x0E000000u);
    if (a >= 0x04000000u && a - 0x04000000u + n <= sizeof g_io) return g_io + (a - 0x04000000u);
    return NULL;
}

/* the blob, compiled for this host against the model */
#define SVT_PTR(a) ((volatile uint8_t *)gba((uint32_t)(a), 1))
#include "../rom/savetool.c"

static uint32_t load32(uint32_t a) { uint32_t v = 0; uint8_t *p = gba(a, 4); if (p) memcpy(&v, p, 4); return v; }
static void store32(uint32_t a, uint32_t v) {
    if (a >= 0x08000000u && a < 0x0A000000u) return;            /* ROM */
    uint8_t *p = gba(a, 4); if (p) memcpy(p, &v, 4);
}
static void store16(uint32_t a, uint16_t v) { uint8_t *p = gba(a, 2); if (p) memcpy(p, &v, 2); }

#define AG_LOAD32(a)     load32(a)
#define AG_STORE32(a, v) store32((a), (v))
#define AG_STORE16(a, v) store16((a), (uint16_t)(v))
#include "../rom/agent_proto.h"
#include "mbagent.h"

static ag_core_t    g_core;
static uint32_t     g_staged;
static mba_client_t g_client;
static bool         g_up;
static int          g_calls, g_stops;

static uint32_t xfer(void *ctx, uint32_t w) {
    (void)ctx;
    uint32_t back = g_staged;
    g_staged = ag_step(&g_core, w);
    if (g_core.call_pending) {                       /* the agent's main loop would run it */
        g_core.call_pending = false;
        g_calls++;
        const svt_block_t *b = (const svt_block_t *)gba(SVT_BASE, sizeof(svt_block_t));
        if (b && g_core.call_addr == b->entry) svt_main();
    }
    return back;
}

static const char *agent_start(void) {
    ag_core_init(&g_core, 0x02000800u);
    ag_protect(&g_core, 0x02000000u, 0x02000800u);
    g_staged = AG_IDLE;
    mba_init(&g_client, xfer, NULL, NULL);
    g_up = mba_ping(&g_client) == MBA_OK;
    return g_up ? NULL : "the agent did not answer";
}
static mba_client_t *agent(void) { return g_up ? &g_client : NULL; }
static void agent_stop(void) { g_stops++; g_up = false; }

/* ---- the card ------------------------------------------------------------------- */

static char g_root[64];

static void full(char *out, size_t cap, const char *path) { snprintf(out, cap, "%s/%s", g_root, path); }

static long file_size(const char *path) {
    char p[256]; struct stat st;
    full(p, sizeof p, path);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static long file_read(const char *path, uint32_t off, void *buf, size_t n) {
    char p[256]; full(p, sizeof p, path);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, off, SEEK_SET);
    const size_t got = fread(buf, 1, n, f);
    fclose(f);
    return (long)got;
}
static bool file_write(const char *path, uint32_t off, const void *buf, size_t n) {
    char p[256]; full(p, sizeof p, path);
    char *slash = strrchr(p, '/');
    *slash = '\0'; mkdir(p, 0755); *slash = '/';
    FILE *f = fopen(p, off ? "r+b" : "wb");
    if (!f) return false;
    fseek(f, off, SEEK_SET);
    const bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}
static int by_name(const void *a, const void *b) { return strcmp(a, b); }
static size_t file_list(const char *dir, const char *suffix, char names[][APPLET_FILE_NAME], size_t max) {
    char p[256]; full(p, sizeof p, dir);
    DIR *d = opendir(p);
    size_t n = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        const size_t l = strlen(e->d_name), sl = strlen(suffix);
        if (l >= sl && strcmp(e->d_name + l - sl, suffix) == 0 && l < APPLET_FILE_NAME)
            memcpy(names[n++], e->d_name, l + 1);
    }
    closedir(d);
    qsort(names, n, APPLET_FILE_NAME, by_name);
    return n;
}

static void put_file(const char *path, const uint8_t *data, size_t n) {
    char p[256]; full(p, sizeof p, path);
    char dir[256]; snprintf(dir, sizeof dir, "%s", p); *strrchr(dir, '/') = '\0'; mkdir(dir, 0755);
    FILE *f = fopen(p, "wb"); fwrite(data, 1, n, f); fclose(f);
}

static bool file_is(const char *path, const uint8_t *want, size_t n) {
    static uint8_t got[0x8000 + 16];
    return file_size(path) == (long)n && file_read(path, 0, got, sizeof got) == (long)n && memcmp(got, want, n) == 0;
}

/* ---- the test -------------------------------------------------------------------- */

static int g_fail;
static void check(const char *what, bool ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

extern const applet_t applet_save_tool;
static applet_env_t g_env = {
    .hint = "hold back", .confirm = "A",
    .agent_start = agent_start, .agent = agent, .agent_stop = agent_stop,
    .file_size = file_size, .file_read = file_read, .file_write = file_write, .file_list = file_list,
};
static ui_page_t g_page;

static const char *page(void) {
    static char all[UI_PAGE_LINES * (UI_PAGE_TEXT + 1)];
    memset(&g_page, 0, sizeof g_page);
    applet_save_tool.page(&g_env, &g_page);
    all[0] = '\0';
    for (int i = 0; i < g_page.count; i++) { strcat(all, g_page.line[i]); strcat(all, "|"); }
    return all;
}

static bool lines_fit(void) {
    for (int i = 0; i < g_page.count; i++) if (strlen(g_page.line[i]) > 21) return false;
    return true;
}

static void press(applet_input_t in, int32_t v) { applet_save_tool.input(&g_env, in, v); }

/* tick until the page shows `want` (or a failure), at most n ticks */
static bool run_until(const char *want, int n) {
    for (int i = 0; i < n; i++) {
        applet_save_tool.tick(&g_env);
        const char *p = page();
        if (strstr(p, want)) return true;
    }
    printf("    the page: %s\n", page());
    return false;
}

static void make_cart(void) {
    memset(g_rom, 0xFF, sizeof g_rom);
    memcpy(g_rom + 0xA0, "POKEMON EMERBPEE", 16);
    if (g_rom_has_save) memcpy(g_rom + 0x9A3C0, "SRAM_V113", 9);
    for (size_t k = 0; k < sizeof g_sram; k++) g_sram[k] = (uint8_t)(k * 7 + (k >> 8));
}

int main(void) {
    snprintf(g_root, sizeof g_root, "/tmp/savetool-XXXXXX");
    if (!mkdtemp(g_root)) return 2;
    applet_bind_env(&g_env);
    make_cart();

    printf("save tool: listed only with its blob on the card\n");
    ui_shell_t shell;
    shell_init(&shell, HW_CAP_DISPLAY | HW_CAP_LINK_PINS);
    applet_add_to_shell(&shell);
    bool listed = false;
    for (uint8_t i = 0; i < shell_item_count(&shell); i++) listed |= strcmp(shell_label_at(&shell, i), "Save Tool") == 0;
    check("no savetool.bin: not in the menu", !listed);

    /* the blob as the board loads it: its header (the code is compiled here) */
    static uint8_t blob[1024];
    svt_block_t head = { .magic = SVT_MAGIC, .version = 1, .entry = 0x02001041u, .buf = SVT_BUF };
    memcpy(blob, &head, sizeof head);
    put_file("savetool/savetool.bin", blob, sizeof blob);
    shell_init(&shell, HW_CAP_DISPLAY | HW_CAP_LINK_PINS);
    applet_add_to_shell(&shell);
    listed = false;
    for (uint8_t i = 0; i < shell_item_count(&shell); i++) listed |= strcmp(shell_label_at(&shell, i), "Save Tool") == 0;
    check("savetool/savetool.bin on the card: in the menu", listed);

    printf("save tool: start, find the save\n");
    check("enters", applet_save_tool.enter(&g_env));
    check("asks for START+SELECT with the cartridge in", strstr(page(), "START+SELECT") != NULL && lines_fit());
    press(APPLET_IN_CONFIRM, 0);
    check("finds the game and its save: BPEE, SRAM", run_until("SRAM 32 KB", 40));
    check("... named on the page", strstr(page(), "BPEE POKEMON EMER") != NULL && lines_fit());

    printf("save tool: back up\n");
    const uint8_t *first = g_sram;
    static uint8_t keep[0x8000];
    memcpy(keep, first, sizeof keep);
    press(APPLET_IN_CONFIRM, 0);
    check("backs up", run_until("saved", 40));
    check("saves/BPEE-001.sav is the cartridge's save, byte for byte", file_is("saves/BPEE-001.sav", keep, sizeof keep));
    press(APPLET_IN_CONFIRM, 0);
    check("a second backup is the next number", run_until("saved BPEE-002", 40) && file_size("saves/BPEE-002.sav") == 0x8000);

    printf("save tool: restore an edited save\n");
    static uint8_t edited[0x8000];
    for (size_t k = 0; k < sizeof edited; k++) edited[k] = (uint8_t)(k ^ 0x5A);
    put_file("saves/edited.sav", edited, sizeof edited);
    put_file("saves/other.sav", edited, 1024);                  /* the wrong size */
    press(APPLET_IN_MOVE, 1);                                    /* Restore */
    press(APPLET_IN_CONFIRM, 0);
    const char *p = page();
    check("lists the .sav files of this save's size, not others",
          strstr(p, "BPEE-001.sav") && strstr(p, "BPEE-002.sav") && strstr(p, "edited.sav") && !strstr(p, "other.sav"));
    press(APPLET_IN_MOVE, 1);
    press(APPLET_IN_MOVE, 1);                                    /* edited.sav */
    press(APPLET_IN_CONFIRM, 0);
    check("asks first, and says the old save is kept", strstr(page(), "edited.sav") && strstr(page(), "backed up first"));
    press(APPLET_IN_CONFIRM, 0);
    check("restores", run_until("restored edited.sav", 80));
    check("the cartridge now holds the edited save", memcmp(g_sram, edited, sizeof edited) == 0);
    check("and the save it replaced is saves/BPEE-003.sav", file_is("saves/BPEE-003.sav", keep, sizeof keep));

    printf("save tool: refusals\n");
    applet_save_tool.exit();
    check("leaving gives the link port back", g_stops == 1);
    blob[0] ^= 0xFF;
    put_file("savetool/savetool.bin", blob, sizeof blob);
    applet_save_tool.enter(&g_env);
    press(APPLET_IN_CONFIRM, 0);
    check("a file that is not the blob: said so", run_until("is not one", 10));
    applet_save_tool.exit();
    blob[0] ^= 0xFF;
    put_file("savetool/savetool.bin", blob, sizeof blob);
    g_rom_has_save = false;
    make_cart();
    applet_save_tool.enter(&g_env);
    press(APPLET_IN_CONFIRM, 0);
    check("a game with no save library: said so", run_until("no save in this game", 40));
    applet_save_tool.exit();

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) return 2;
    printf("%s\n", g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
