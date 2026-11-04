/*
 * print_store_esp32s3.c -- where the GB Printer mode keeps its prints
 * (src/printer_mode.h): PRINTS/PRNTnnnn.PNG on the SD card when one is
 * mounted, else on the board's own drive (the USB storage partition) while
 * the board, not a PC, has it. Each print takes the number after the highest
 * already there, so nothing is overwritten and a PC sees them in order.
 */
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "gbp_png.h"

static const char *TAG = "prints";

bool sd_mounted(void);
bool msc_storage_available(void);   /* weak false in gate_app.c without a drive */

bool print_store_save(const uint8_t *png, size_t n, char *name, size_t cap) {
    const char *root = sd_mounted() ? "/sd" : msc_storage_available() ? "/roms" : NULL;
    if (!root) {
        return false;
    }
    char dir[24], path[48];
    snprintf(dir, sizeof dir, "%s/PRINTS", root);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %d", dir, errno);
        return false;
    }

    unsigned highest = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const unsigned k = gbp_png_number(de->d_name);
            if (k > highest) highest = k;
        }
        closedir(d);
    }
    if (highest >= 9999) {
        ESP_LOGW(TAG, "%s is full", dir);
        return false;
    }

    char file[GBP_NAME_MAX];
    gbp_png_name(file, highest + 1);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s: %d", path, errno);
        return false;
    }
    const bool ok = fwrite(png, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) {
        ESP_LOGW(TAG, "write %s failed", path);
        remove(path);
        return false;
    }
    if (name && cap) {
        snprintf(name, cap, "%s", file);
    }
    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)n);
    return true;
}
