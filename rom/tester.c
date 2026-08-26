/*
 * tester.c -- a self-checking Battle Chip Gate tester ROM for the GBA.
 *
 * Runs in mGBA as the game (the Multi-Play parent) opposite the firmware gate
 * running in QEMU. qemu/mgba_tester.lua shuttles words between the two through
 * a mailbox in EWRAM: this ROM stages a word, the script sends it to the gate
 * as a link_proto WORD, and the gate's answer comes back into the mailbox for
 * this ROM to read.
 *
 * Why a mailbox and not the SIO registers
 * ---------------------------------------
 * An earlier version of this ROM read SIOMULTI0 and wrote SIOMLT_SEND, on the
 * grounds that avoiding SIOCNT was enough to sidestep the timing uncertainty
 * in the retail path. It is not: mGBA refuses a script's writes to the
 * multiplayer data registers outright --
 *
 *     GBA Serial I/O: GPIO write: Unhandled SIOMULTI0 <- FF00
 *
 * -- so the gate's answer never reached the ROM, the handshake never advanced
 * past the gate announcing itself, and the run failed after 1200 frames with
 * the gate still in "waiting". EWRAM has no such semantics: a script's write
 * lands, every time. Since the ROM is ours, nothing is lost by defining the
 * interface ourselves rather than borrowing hardware registers that emulators
 * are entitled to arbitrate.
 *
 * The exchange is sequence-numbered rather than frame-paced, so neither side
 * can miss a word or process one twice if a round trip takes longer than a
 * frame -- which it will, through a socket to another process.
 *
 * The verdict is written to a fixed EWRAM address so mgba_tester.lua can read it
 * without parsing anything, and painted to the Mode 3 screen so a screenshot is
 * meaningful on its own: green = pass, red = fail, blue = still running.
 *
 * Two images, one source
 * ----------------------
 * rom/Makefile links this same object file twice:
 *
 *   make      -> tester.gba, a cartridge image running from 0x08000000
 *   make mb   -> tester.mb,  a multiboot image running from 0x02000000
 *
 * Sharing the object rather than the source is deliberate: it makes it
 * impossible for the two images to drift into running different handshakes,
 * so a discrepancy between them is always a delivery problem and never a logic
 * one. Only the startup file and the linker script differ -- see crt0_mb.s and
 * multiboot.ld. The multiboot image is what exercises src/gba_multiboot.c's
 * mb_send() end to end; the cartridge image cannot, since it is never
 * transferred anywhere.
 *
 * Built by rom/Makefile with a bare arm-none-eabi toolchain (no devkitPro
 * required). The same handshake.h is exercised on the host with no emulator at
 * all by qemu/test_rom_selftest.py.
 */

#include <stdint.h>

#include "handshake.h"

/* ---- GBA memory-mapped I/O -------------------------------------------- */

#define REG_DISPCNT   (*(volatile uint16_t *)0x04000000)
#define REG_VCOUNT    (*(volatile uint16_t *)0x04000006)

#define MODE3_BG2     0x0403               /* Mode 3 + BG2 on            */
#define VRAM          ((volatile uint16_t *)0x06000000)
#define SCREEN_W      240
#define SCREEN_H      160

/* BGR555, what the GBA displays. */
#define RGB15(r, g, b) ((uint16_t)(((b) << 10) | ((g) << 5) | (r)))
#define COL_RUNNING   RGB15(4, 6, 16)
#define COL_PASS      RGB15(6, 28, 10)
#define COL_FAIL      RGB15(28, 5, 5)
#define COL_BAR       RGB15(31, 31, 31)

/* ---- result signature, at a fixed EWRAM address ----------------------- */
/*
 * High in EWRAM (256 KB, 0x02000000..0x0203FFFF), clear of .data/.bss which the
 * linker places low. mgba_tester.lua reads it with emu:read32 / emu:read16.
 */
#define RESULT_ADDR   0x0203FF00u
#define RESULT_MAGIC  0x47544247u  /* 'G','B','G','T' little-endian */

#define STATUS_RUNNING 0u
#define STATUS_PASS    1u
#define STATUS_FAIL    2u

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint32_t frames;
    uint32_t linked_frames;
    uint32_t gate_id_seen;
    uint32_t phase;
    uint16_t rotation[9];
    uint16_t _pad;

    /*
     * The mailbox. Sequence counters rather than flags: a counter cannot be
     * missed by a reader that happens to look twice in one frame, and cannot
     * be mistaken for a repeat if the round trip outlasts a frame.
     *
     *   ROM writes to_gate then bumps seq_to;   the script watches seq_to.
     *   script writes from_gate then bumps seq_from; the ROM watches seq_from.
     */
    uint16_t to_gate;     /* ROM -> gate: the word we are staging  */
    uint16_t seq_to;
    uint16_t from_gate;   /* gate -> ROM: the word the gate sent   */
    uint16_t seq_from;

    /*
     * ---- multiboot evidence (appended: no offset above may move) --------
     *
     * qemu/mgba_tester.lua reads this block at hard-coded byte offsets -- 0,
     * 4, 8, 12, 16 and the mailbox at 44..50 -- so anything new goes on the
     * end and the layout above stays exactly as it was.
     *
     * run_base is where this image is really executing from: 0x08000000 for
     * the cartridge build, 0x02000000 for the multiboot one.
     *
     * boot_mode and slave_id are header bytes 0xC4 and 0xC5. crt0_mb.s
     * initialises both to zero and the BIOS overwrites them while it downloads
     * a multiboot image, so a non-zero pair is positive evidence that a real
     * multiboot happened rather than someone having handed the emulator a
     * cartridge. They mean nothing unless run_base is 0x02000000: the
     * cartridge build has ordinary code at those two addresses and will
     * cheerfully report whatever instruction bytes it finds there.
     *
     * The evidence is one-directional. Non-zero means a downloader wrote them,
     * which only a multiboot does. Zero does *not* prove the download failed:
     * an emulator with a high-level BIOS may load the image straight into
     * EWRAM and jump to it without ever emulating the transfer that would set
     * these bytes. Treat a zero pair as "unproven", and read run_base -- which
     * is decided by the linker and so is always trustworthy -- first.
     */
    uint32_t run_base;
    uint32_t boot_mode;
    uint32_t slave_id;
} result_t;

#define RESULT (*(volatile result_t *)RESULT_ADDR)

/*
 * The image base, defined by crt0.s (cartridge) or crt0_mb.s (multiboot). Used
 * only to read the two header bytes the BIOS rewrites during a multiboot
 * download; see the run_base/boot_mode/slave_id fields above.
 */
extern char _start;

/* Give the round trip plenty of frames before declaring failure: at 60 Hz this
 * is 20 s, far longer than the handshake needs even with network latency. */
#define TEST_FRAMES 1200u

/* ---- freestanding helpers the compiler may still want ----------------- */

void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* ---- display ---------------------------------------------------------- */

static void wait_vblank(void) {
    while (REG_VCOUNT >= SCREEN_H) { }   /* wait out any current vblank */
    while (REG_VCOUNT < SCREEN_H)  { }   /* then wait for the next      */
}

static void fill(uint16_t colour) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        VRAM[i] = colour;
    }
}

/* A short progress bar near the top, one lit cell per link-evidence frame, so
 * even the "running" screen shows that something is happening. */
static void progress_bar(uint32_t linked) {
    uint32_t cells = linked;
    if (cells > 40) {
        cells = 40;
    }
    for (uint32_t c = 0; c < cells; c++) {
        int x0 = 20 + (int)c * 5;
        for (int y = 10; y < 20; y++) {
            for (int x = x0; x < x0 + 4; x++) {
                VRAM[y * SCREEN_W + x] = COL_BAR;
            }
        }
    }
}

/* ---- result ----------------------------------------------------------- */

static void publish(const hs_t *h, uint32_t status) {
    RESULT.magic         = RESULT_MAGIC;
    RESULT.status        = status;
    RESULT.frames        = h->frames;
    RESULT.linked_frames = h->linked_frames;
    RESULT.gate_id_seen  = h->gate_id_seen;
    RESULT.phase         = (uint32_t)h->phase;
    for (int i = 0; i < 9; i++) {
        RESULT.rotation[i] = h->rotation[i];
    }
}

/* ---- entry ------------------------------------------------------------ */

int main(void) {
    REG_DISPCNT = MODE3_BG2;
    fill(COL_RUNNING);

    hs_t h;
    hs_init(&h, HS_GATE_ID_BN6US);

    RESULT.to_gate   = 0;
    RESULT.seq_to    = 0;
    RESULT.from_gate = 0;
    RESULT.seq_from  = 0;

    /* Written once: the BIOS has already finished with these bytes by the time
     * any of our code runs, and nothing rewrites them afterwards. */
    const volatile uint8_t *base = (const volatile uint8_t *)&_start;
    RESULT.run_base  = (uint32_t)(uintptr_t)base;
    RESULT.boot_mode = base[0xC4];
    RESULT.slave_id  = base[0xC5];

    publish(&h, STATUS_RUNNING);

    /*
     * Prime the exchange. Nothing arrives until we have staged something, so
     * without this both sides wait for the other for ever.
     */
    RESULT.to_gate = HS_WORD_IDLE;
    RESULT.seq_to  = 1;

    uint32_t status  = STATUS_RUNNING;
    uint16_t last_in = 0;

    for (;;) {
        wait_vblank();

        /* Step only on a genuinely new answer. Stepping every frame would
         * advance the handshake on stale data whenever the round trip took
         * longer than 16 ms, which through a socket it usually does. */
        uint16_t seq_from = RESULT.seq_from;
        if (seq_from != last_in) {
            last_in = seq_from;
            uint16_t answer = RESULT.from_gate;
            uint16_t stage  = hs_step(&h, answer);
            RESULT.to_gate  = stage;
            RESULT.seq_to   = (uint16_t)(RESULT.seq_to + 1);
        }

        if (status == STATUS_RUNNING) {
            if (hs_passed(&h)) {
                status = STATUS_PASS;
                fill(COL_PASS);
            } else if (h.frames >= TEST_FRAMES) {
                status = STATUS_FAIL;
                fill(COL_FAIL);
            }
        }

        publish(&h, status);
        progress_bar(h.linked_frames);
    }

    return 0;
}
