/*
 * usb_msc_esp32s3.c -- expose a FAT partition over USB, and read it locally.
 *
 * Programs arrive by drag-and-drop rather than by recompiling: the device
 * enumerates as a mass storage class device, the user drops .mb files on, and
 * rom_catalog reads them back through the rom_io_t implemented here.
 *
 * Not yet run on hardware. The FATFS and TinyUSB calls are ESP-IDF v5.x.
 *
 * The ownership rule
 * ------------------
 * A FAT volume has exactly one owner at a time. If the host is mounted and the
 * firmware writes a byte, or vice versa, the host's cached FAT and the real one
 * diverge and the volume is corrupt -- often not visibly, and often not until
 * much later.
 *
 * So the partition is handed back and forth explicitly:
 *
 *   USB attached   -> tinyusb_msc_storage_mount_to_usb(), app must not touch it
 *   USB detached   -> tinyusb_msc_storage_mount_to_app(), then rescan
 *
 * msc_storage_available() is the single place that answers "may I read a file
 * right now", and everything that touches the filesystem goes through it.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "wear_levelling.h"

#include "rom_catalog.h"

static const char *TAG = "msc";

#define MOUNT_POINT     "/roms"
#define PARTITION_LABEL "storage"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static volatile bool s_host_owns;   /* true while the PC has the volume */
static volatile bool s_dirty;       /* host wrote something; rescan needed */

void msc_on_app_mounted(void);

/* ---- ownership -------------------------------------------------------- */

bool msc_storage_available(void) {
    if (s_wl == WL_INVALID_HANDLE) {
        return false;
    }
    if (s_host_owns) {
        return false;
    }
    /*
     * Ask the stack as well as trusting our own flag. The callback can be
     * missed -- a host that enumerates during boot, before the callback is
     * registered, leaves s_host_owns false while the volume is genuinely
     * exposed. Reading a file then races the host's writes, and the failure is
     * a corrupt FAT that shows up much later and looks like a bad drive.
     */
    return !tinyusb_msc_storage_in_use_by_usb_host();
}

bool msc_storage_dirty(void) {
    return s_dirty;
}

void msc_storage_clear_dirty(void) {
    s_dirty = false;
}

/*
 * TinyUSB tells us when the host mounts and unmounts. Handing the volume over
 * here rather than at cable insertion matters: a host can enumerate without
 * ever mounting, and can keep the volume mounted after the user thinks they
 * are finished.
 */
static void msc_on_mount_changed(tinyusb_msc_event_t *event) {
    if (event->type != TINYUSB_MSC_EVENT_MOUNT_CHANGED) {
        return;
    }
    /*
     * is_mounted is from the application's point of view: true when the volume
     * has been handed back to us. The naming invites the opposite reading, and
     * getting it backwards means writing to a volume the host is holding --
     * which is exactly the corruption this whole arrangement exists to avoid,
     * so it is worth the sentence.
     */
    bool to_app = event->mount_changed_data.is_mounted;
    if (to_app) {
        s_host_owns = false;
        s_dirty     = true; /* assume the host changed something */
        ESP_LOGI(TAG, "volume returned to the device");
        msc_on_app_mounted();   /* from TinyUSB's task: the hook only flags it */
    } else {
        s_host_owns = true;
        ESP_LOGI(TAG, "volume handed to the host");
    }
}

/*
 * The volume has just become the firmware's: at boot, or when a PC handed it
 * back. net_esp32s3.c reads wifi.txt then. Weak, for images without a network.
 */
__attribute__((weak)) void msc_on_app_mounted(void) {}

/* A small text file from the volume's root, whole, NUL-terminated. False if
 * the volume is the host's, the file is absent, or it is larger than cap-1. */
bool msc_read_text(const char *name, char *buf, size_t cap, size_t *len) {
    if (cap == 0 || !msc_storage_available()) {
        return false;
    }
    char path[96];
    snprintf(path, sizeof path, MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    bool whole = n < cap - 1 || fgetc(f) == EOF;
    fclose(f);
    if (!whole) {
        return false;
    }
    buf[n] = '\0';
    if (len) *len = n;
    return true;
}

/* ---- setup ------------------------------------------------------------ */

esp_err_t msc_storage_init(void) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PARTITION_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "no '%s' partition; check partitions.csv", PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wl_mount(part, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wear levelling mount failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * format_if_mount_failed is on deliberately: a brand new device has an
     * unformatted partition, and asking the user to format it over USB before
     * it will enumerate is a bad first experience.
     *
     * The mount config lives inside the spiflash config in ESP-IDF 5.x. It has
     * moved between versions; if this does not compile, that is the field to
     * look at rather than the values.
     */
    const tinyusb_msc_spiflash_config_t sf = {
        .wl_handle = s_wl,
        .callback_mount_changed = msc_on_mount_changed,
        .mount_config = {
            .format_if_mount_failed = true,
            .max_files              = 4,
            .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_spiflash(&sf), TAG,
                        "msc storage init failed");
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_mount(MOUNT_POINT), TAG,
                        "mount failed");

    const tinyusb_config_t tusb = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy      = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb), TAG, "tinyusb install");

    s_host_owns = false;
    s_dirty     = true;
    ESP_LOGI(TAG, "storage ready at %s", MOUNT_POINT);
    msc_on_app_mounted();
    return ESP_OK;
}

/* ---- rom_io_t over FATFS ---------------------------------------------- */

typedef struct {
    DIR *dir;
} msc_scan_t;

static bool msc_next(void *ctx, char *name, size_t cap, uint32_t *size) {
    msc_scan_t *s = ctx;
    if (!s->dir) {
        return false;
    }

    struct dirent *de;
    while ((de = readdir(s->dir)) != NULL) {
        char path[300];
        snprintf(path, sizeof path, MOUNT_POINT "/%s", de->d_name);

        /*
         * stat, not d_type: DT_DIR is a BSD extension that strict C11 hides
         * and that ESP-IDF's FATFS layer is not obliged to populate. stat is
         * authoritative, and it is needed for the size anyway.
         */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        /* Skip anything that cannot be a program before bothering the caller;
         * a FAT root written by a desktop is full of metadata files. */
        if (!rom_name_is_program(de->d_name)) {
            continue;
        }
        strncpy(name, de->d_name, cap - 1);
        name[cap - 1] = '\0';
        *size = (uint32_t)st.st_size;
        return true;
    }
    return false;
}

static bool msc_read_head(void *ctx, const char *name, uint8_t *buf, size_t n) {
    (void)ctx;
    if (!msc_storage_available()) {
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

static bool msc_read_all(void *ctx, const char *name, uint8_t *buf, size_t cap,
                         uint32_t *out_len) {
    (void)ctx;
    if (!msc_storage_available()) {
        return false;
    }
    char path[300];
    snprintf(path, sizeof path, MOUNT_POINT "/%s", name);

    /*
     * Take the length from stat rather than from the read. Reading cap bytes
     * and testing `got == cap` cannot tell an exact fit from a file that is
     * longer than the buffer -- which would have refused a maximum-size
     * 256 KB program, the one case that matters most.
     */
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > cap) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    /* One read of the whole file, then close. See the staging rule in
     * rom_catalog.h -- nothing below this line may touch the filesystem. */
    size_t want = (size_t)st.st_size;
    size_t got = fread(buf, 1, want, f);
    fclose(f);

    if (got != want) {
        return false;
    }
    *out_len = (uint32_t)got;
    return true;
}

static const rom_io_t MSC_IO = { msc_next, msc_read_head, msc_read_all };

/*
 * Rescan the drive. Returns false if the host currently owns the volume, which
 * is a normal state rather than an error: the UI should show "connected to PC"
 * instead of an empty list.
 */
bool msc_rescan(rom_catalog_t *cat, uint32_t logo_reference) {
    if (!msc_storage_available()) {
        return false;
    }
    msc_scan_t scan = { opendir(MOUNT_POINT) };
    if (!scan.dir) {
        ESP_LOGW(TAG, "cannot open " MOUNT_POINT);
        return false;
    }
    rom_catalog_scan(cat, &MSC_IO, &scan, logo_reference);
    closedir(scan.dir);
    s_dirty = false;

    ESP_LOGI(TAG, "%u programs, %u playable", cat->count, cat->playable);
    return true;
}

const rom_io_t *msc_io(void) {
    return &MSC_IO;
}

/*
 * Hand the volume back to the host deliberately, for a menu action that says
 * "connect to PC". Without this the only way to expose the drive is to guess
 * at what the host is doing, and a device that cannot be told to release its
 * storage is one you end up power-cycling.
 */
esp_err_t msc_release_to_host(void) {
    if (s_wl == WL_INVALID_HANDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = tinyusb_msc_storage_unmount();
    if (err == ESP_OK) {
        s_host_owns = true;
        ESP_LOGI(TAG, "volume released to the host");
    }
    return err;
}

/* Take it back, and rescan: the host has almost certainly changed something. */
esp_err_t msc_claim_from_host(void) {
    if (s_wl == WL_INVALID_HANDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = tinyusb_msc_storage_mount(MOUNT_POINT);
    if (err == ESP_OK) {
        s_host_owns = false;
        s_dirty     = true;
        ESP_LOGI(TAG, "volume reclaimed");
        msc_on_app_mounted();
    }
    return err;
}
