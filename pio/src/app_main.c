/*
 * app_main.c -- bring-up, in the order the hardware needs.
 *
 * Not yet run on hardware.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "board.h"
#include "hw_caps.h"
#include "settings.h"
#include "link_source.h"
#include "variant.h"

static const char *TAG = "main";

extern void tasks_start(uint32_t features);
extern uint16_t *st7735_framebuffer(void);
extern esp_err_t st7735_init(int panel);
#ifndef GBAGATE_QEMU
/* Weak: the link and mini images have no USB drive (usb_msc_esp32s3.c is
 * built for gate and full only), and the call below is a runtime check. */
esp_err_t msc_storage_init(void);
__attribute__((weak)) esp_err_t msc_storage_init(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif

#ifndef VARIANT
#error "platformio.ini must define VARIANT"
#endif

/* What USB became at boot, for the Link: USB page: the setting can change
 * since, but only a restart applies it. */
static bool s_gblink;       /* GB-Link's firmware runs on this image */
static bool s_usb_gblink;   /* ... and USB is its adapter, not the drive */

bool app_gblink_running(void) { return s_gblink; }
bool app_usb_is_gblink(void) { return s_usb_gblink; }

void app_main(void) {
    ESP_LOGI(TAG, "gbagate %s", GBAGATE_VARIANT_NAME);

    /*
     * Resolve the image before touching hardware. A dependency error here is a
     * build mistake that reached a device, and it is far better to say so on
     * the console than to half-start and fail later in something obscure.
     */
    variant_result_t v = variant_resolve(VARIANT);
    if (v.status != VARIANT_VALID) {
        ESP_LOGE(TAG, "incoherent image: %s (%s)",
                 variant_status_name(v.status), feature_name(v.offender));
        return;
    }

    char buf[420];
    variant_describe(v.resolved, buf, sizeof buf);
    ESP_LOGI(TAG, "features: %s", buf);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /*
     * Probe before starting anything. hw_caps decides which menu entries exist,
     * and a mode whose hardware is absent must never appear -- see the
     * diagnostics screen, which is reachable with no capabilities at all.
     */
    /*
     * Log every probe. A black panel has at least four causes -- display init
     * failed, the UI task never started, nothing ever rendered, or QEMU mapped
     * the serial ports differently -- and they are indistinguishable from the
     * outside. The console log has to say which.
     */
    /*
     * Two gates on every subsystem, and they mean different things:
     *
     *   the variant says the firmware was built with the feature;
     *   the board says the part is fitted.
     *
     * Both must hold. Keeping them separate is what lets a bring-up leave the
     * card off with a build flag instead of an edit, and lets one image serve
     * boards that differ in what is populated.
     */
    uint32_t caps = 0;
    if ((v.resolved & FEAT_DISPLAY) && BOARD_HAS_DISPLAY) {
        esp_err_t derr = st7735_init(0);
        if (derr == ESP_OK) {
            caps |= HW_CAP_DISPLAY;
            ESP_LOGI(TAG, "display ready, framebuffer %p", st7735_framebuffer());
        } else {
            ESP_LOGE(TAG, "display init failed: %s -- the panel will stay black",
                     esp_err_to_name(derr));
        }
    }
#ifndef GBAGATE_QEMU
    /*
     * The link PHY. Nothing used to start it -- gba_link_start() had no
     * caller, and the functions link_source binds the gate to did not exist,
     * so no hardware image could have linked. One engine now serves the gate
     * and the GB-Link personality; see gba_link_esp32s3.c.
     */
    if ((v.resolved & (FEAT_LINK_MASTER | FEAT_LINK_SLAVE)) && BOARD_HAS_LINK) {
        extern void gba_link_start(void);
        gba_link_start();
        /* GB-Link's Wireless Adapter mode (0x07) drives the same pins in
         * 32-bit Normal mode; it borrows them from the engine above while it
         * runs, and its SD interrupt uses the ISR service that one installed. */
        extern void rfu_link_start(void);
        rfu_link_start();
        /* The bridge UART to a companion such as GB-Link-Switch-LDN
         * (BOARD_HAS_BRIDGE; a no-op without it). */
        extern void bridge_uart_start(void);
        bridge_uart_start();
    }

    /*
     * One USB controller, so one personality, chosen by the "usb" setting:
     * a GB-Link adapter (the default, for the GB-Link apps, the netplay bridge
     * and Celio) or the drive the ROM catalogue reads. Read here rather than
     * by gate_settings_start() because USB must be up before the host gives
     * up on enumeration, and that is long before the UI.
     */
    settings_t boot;
    extern const settings_io_t *nvs_settings_io(void);
    settings_load(&boot, nvs_settings_io(), NULL);
    bool as_gblink = (v.resolved & FEAT_GBLINK) && boot.usb_mode == USB_MODE_GBLINK;
    /* GB-Link's firmware runs whatever USB is: the wireless link cable and
     * the bridge UART drive it with no USB host at all. */
    if (v.resolved & FEAT_GBLINK) {
        extern void gblink_start(void);
        gblink_start();            /* answer before the host can ask */
        s_gblink = true;
    }
    if (as_gblink) {
        extern esp_err_t usb_gblink_start(void);
        if (usb_gblink_start() == ESP_OK) {
            s_usb_gblink = true;
            ESP_LOGI(TAG, "USB: GB-Link adapter");
        } else {
            ESP_LOGE(TAG, "USB: GB-Link personality failed to start");
        }
    } else if (v.resolved & FEAT_USB_MSC) {
        if (msc_storage_init() == ESP_OK) {
            caps |= HW_CAP_USB;
            ESP_LOGI(TAG, "USB: drive");
        }
    }
#endif
    if (v.resolved & FEAT_ESPNOW) {
        caps |= HW_CAP_WIFI;
    }

    /*
     * Nothing was setting this, so every mode that needs the link port -- the
     * chip gate, both link modes, Send Program and Self Test, five of the eight
     * -- was filtered out of the menu. The device booted to Settings and
     * Diagnostics and looked like it had almost no features.
     *
     * "Detected" cannot mean "a cable is plugged in": the link port has no
     * presence pin, and on real hardware the parent only clocks once a game
     * asks it to. What the capability actually gates is whether this image has
     * somewhere for transfers to come from -- the PHY on hardware, the bridge
     * under emulation -- and link_source_active() answers exactly that.
     */
    /*
     * The chip reader is a board that may or may not be fitted, so this is a
     * real probe rather than a build-time fact -- an MCP23017 either answers on
     * I2C or it does not. Under emulation there is no I2C at all, so the mode
     * stays hidden, which is correct: there is nothing to read a chip with.
     */
    if ((v.resolved & FEAT_CHIP_READER) && BOARD_HAS_READER) {
#if defined(GBAGATE_QEMU)
        ESP_LOGI(TAG, "no chip reader under emulation; Read Chip hidden");
#else
        extern bool chip_reader_probe(void);
        if (chip_reader_probe()) {
            caps |= HW_CAP_CHIP_READER;
            ESP_LOGI(TAG, "chip reader detected");
        } else {
            ESP_LOGI(TAG, "no chip reader fitted; Read Chip hidden");
        }
#endif
    }

#if !defined(GBAGATE_QEMU)
    /*
     * Infrared shares the link connector but not its protocol -- General
     * Purpose mode rather than Multi-Play -- so it is brought up separately.
     * QEMU has no pins, so the mode reports "no IR fitted" there via the weak
     * defaults in gate_app.c.
     */
    if ((v.resolved & FEAT_IR) && BOARD_HAS_IR) {
        extern esp_err_t ir_init(void);
        if (ir_init() == ESP_OK) {
            ESP_LOGI(TAG, "infrared ready");
        } else {
            ESP_LOGW(TAG, "infrared init failed; the mode will say so");
        }
    }
#endif

    if ((v.resolved & (FEAT_LINK_MASTER | FEAT_LINK_SLAVE)) && BOARD_HAS_LINK) {
        const link_source_t *src = link_source_active();
        if (src && src->poll) {
            caps |= HW_CAP_LINK_PORT;
            ESP_LOGI(TAG, "link source: %s", src->name);
        } else {
            ESP_LOGW(TAG, "no link source; link modes will be hidden");
        }
    }

    /*
     * Settings before tasks: the display offsets are calibration, and a UI that
     * starts before they are loaded paints one frame with the wrong ones.
     */
    {
        extern void gate_settings_start(void);
        gate_settings_start();
    }

    tasks_start(v.resolved);

    /*
     * Show the shell. Without this the firmware boots straight into the chip
     * list and all seven modes -- self test included -- are unreachable, which
     * is exactly how it behaved until now.
     */
    if (v.resolved & FEAT_DISPLAY) {
        extern void gate_shell_start(uint32_t shell_caps);
        gate_shell_start(caps);
        ESP_LOGI(TAG, "shell started with caps 0x%08X", (unsigned)caps);
    } else {
        ESP_LOGW(TAG, "image has no display feature; nothing will be drawn");
    }
}
