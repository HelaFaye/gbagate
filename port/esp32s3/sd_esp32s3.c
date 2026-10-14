/*
 * sd_esp32s3.c -- programs on an SD card.
 *
 * The same catalogue and the same staging as the USB path: rom_catalog.c takes
 * its filesystem as a set of callbacks, so only the backend changes. That was
 * worth designing for and this is the payoff -- a board with no USB storage
 * still browses and sends programs with no change above this file.
 *
 * Not yet run on hardware.
 *
 * The card shares the display's SPI bus
 * ------------------------------------
 * Both are SPI, and on a devkit there are plenty of pins but only so many SPI
 * hosts worth using. Sharing is fine because the two are never busy at once by
 * construction: rom_stage() reads the whole file and closes it before any
 * transfer starts, and the display is only written from the UI task. What is
 * not fine is a card left selected -- so the driver relies on the SPI driver's
 * own chip-select handling rather than holding CS by hand.
 *
 * If the display glitches while browsing, that is the first thing to check.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "rom_catalog.h"

static const char *TAG = "sd";

#define MOUNT_POINT "/sd"

/* Shares the display bus; only CS differs. */


static sdmmc_card_t *s_card;
static bool          s_mounted;

bool sd_mounted(void) { return s_mounted; }

esp_err_t sd_init(int spi_host) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_host;
    /*
     * 20 MHz rather than the default. Cards on a shared bus with long devkit
     * jumpers are unreliable at 40, and the failure is intermittent read
     * errors that look like a corrupt filesystem rather than a signal problem.
     */
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;   /* board.h */
    slot.host_id = (spi_host_device_t)spi_host;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        /*
         * Deliberately not formatting on failure. This is the user's card,
         * with their programs on it -- reformatting because a read went wrong
         * would be the worst possible response. The USB volume is ours and is
         * formatted if empty; a card is not.
         */
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot,
                                            &mount, &s_card);
    if (err != ESP_OK) {
        /* No card is the normal case, not a fault: the mode says so and the
         * device works without one. */
        ESP_LOGI(TAG, "no card mounted (%s)", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "card mounted: %s, %lluMB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

void sd_unmount(void) {
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_mounted = false;
    }
}

/* ---- rom_io_t over the card -------------------------------------------- */

typedef struct { DIR *dir; } sd_scan_t;

static bool sd_next(void *ctx, char *name, size_t cap, uint32_t *size) {
    sd_scan_t *s = ctx;
    if (!s->dir) {
        return false;
    }
    struct dirent *de;
    while ((de = readdir(s->dir)) != NULL) {
        if (!rom_name_is_program(de->d_name)) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof path, MOUNT_POINT "/%s", de->d_name);

        /* stat, not d_type: the FATFS layer is not obliged to fill it in, and
         * the size is needed anyway. */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        strncpy(name, de->d_name, cap - 1);
        name[cap - 1] = '\0';
        *size = (uint32_t)st.st_size;
        return true;
    }
    return false;
}

static bool sd_read_head(void *ctx, const char *name, uint8_t *buf, size_t n) {
    (void)ctx;
    if (!s_mounted) {
        return false;
    }
    char path[300];
    snprintf(path, sizeof path, MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}

static bool sd_read_all(void *ctx, const char *name, uint8_t *buf, size_t cap,
                        uint32_t *out_len) {
    (void)ctx;
    if (!s_mounted) {
        return false;
    }
    char path[300];
    snprintf(path, sizeof path, MOUNT_POINT "/%s", name);

    /* Length from stat, so an exact fit is not mistaken for a file larger than
     * the buffer -- which would refuse a maximum-size 256 KB program. */
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > cap) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t want = (size_t)st.st_size;
    size_t got = fread(buf, 1, want, f);
    fclose(f);

    if (got != want) {
        return false;
    }
    *out_len = (uint32_t)got;
    return true;
}

static const rom_io_t SD_IO = { sd_next, sd_read_head, sd_read_all };

const rom_io_t *sd_io(void) { return &SD_IO; }

bool sd_rescan(rom_catalog_t *cat, uint32_t logo_reference) {
    if (!s_mounted) {
        return false;
    }
    sd_scan_t scan = { opendir(MOUNT_POINT) };
    if (!scan.dir) {
        return false;
    }
    rom_catalog_scan(cat, &SD_IO, &scan, logo_reference);
    closedir(scan.dir);
    ESP_LOGI(TAG, "%u programs, %u playable", cat->count, cat->playable);
    return true;
}
