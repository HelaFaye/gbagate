/*
 * online_screen.c -- draw the board's Link: Online page, with the device's
 * own renderer and font, from a board's live values (a test passes what the
 * simulated board's Celio client reports). Writes a PPM.
 *
 *   online_screen <out.ppm> <celio_state> <session> <link_status> \
 *                 <up> <down> <resent> <lost> <max_rtt_ms> [ip]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_render.h"
#include "ui_screens.h"
#include "ui_text.h"

static uint16_t g_fb[UI_SCREEN_W * UI_SCREEN_H];

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr, "usage: %s out.ppm state session link_status up down resent lost max_rtt_ms [ip]\n", argv[0]);
        return 2;
    }
    ui_online_t o = {
        .net = UI_NET_UP,
        .ssid = "test network",
        .ip = argc > 10 ? argv[10] : "192.168.1.50",
        .celio = atoi(argv[2]),
        .session = argv[3],
        .link_status = (uint16_t)strtoul(argv[4], NULL, 0),
        .has_stats = true,
        .up = (uint32_t)strtoul(argv[5], NULL, 0),
        .down = (uint32_t)strtoul(argv[6], NULL, 0),
        .resent = (uint32_t)strtoul(argv[7], NULL, 0),
        .lost = (uint32_t)strtoul(argv[8], NULL, 0),
        .max_rtt_ms = (uint32_t)strtoul(argv[9], NULL, 0),
    };
    ui_keys_t k = ui_keys_for(0);
    ui_page_t p;
    ui_page_online(&p, "Link: Online", &k, &o);
    ui_canvas_t c = { g_fb, UI_SCREEN_W, UI_SCREEN_H };
    ui_render_page(&c, &p);
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", UI_SCREEN_W, UI_SCREEN_H);
    for (int i = 0; i < UI_SCREEN_W * UI_SCREEN_H; i++) {
        const uint16_t v = g_fb[i];
        const uint8_t rgb[3] = { (uint8_t)(((v >> 11) & 0x1F) * 255 / 31), (uint8_t)(((v >> 5) & 0x3F) * 255 / 63),
                                 (uint8_t)((v & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    for (int i = 0; i < p.count; i++) printf("%s\n", p.line[i]);
    return 0;
}
