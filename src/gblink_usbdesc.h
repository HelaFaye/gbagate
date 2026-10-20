/*
 * gblink_usbdesc.h -- the USB identity GB-Link's host software expects.
 *
 * Every byte here is either fixed by a GB-Link host or copied from the
 * upstream descriptors (GBLink-Firmware layers/usbLayer.cpp at 4aef46a), so
 * that a browser, the netplay bridge and Windows' driver binding all see the
 * same device whichever board is plugged in:
 *
 *   VID 0x2FE3 PID 0x000A          netplay-bridge usb/device.ts; webusb filters
 *   bcdUSB 0x0210                  Chrome reads the WebUSB capability only >= 2.10
 *   bcdDevice = BCD of 2.2.7       launcher.js reads the version from it
 *   interface 0: vendor, 4 bulk    EP1 OUT cmd, EP1 IN status, EP2 OUT/IN data
 *   interfaces 1-2: CDC-ACM + IAD  WebSerial
 *   BOS: WebUSB (vendor code 1, landing page launcher.gblink.io)
 *        MS OS 2.0 (vendor code 2) -- WinUSB on interface 0 only, via a
 *        function-subset header, or usbccgp never binds WinUSB and WebUSB
 *        cannot open the device on Windows.
 *
 * Built at run time rather than as static arrays because two fields depend on
 * settings (the landing-page flag) and on the chip (the serial number), and
 * because a builder can be checked byte by byte on a host, which a table in a
 * port file cannot.
 */
#ifndef GBLINK_USBDESC_H
#define GBLINK_USBDESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBLINK_USB_VID         0x2FE3u
#define GBLINK_USB_PID         0x000Au
/* The GB-Link protocol level this firmware implements. Reported by
 * GetFirmwareInfo and folded into bcdDevice. */
#define GBLINK_PROTO_MAJOR     2u
#define GBLINK_PROTO_MINOR     2u
#define GBLINK_PROTO_PATCH     7u
#define GBLINK_USB_BCD_DEVICE  ((GBLINK_PROTO_MAJOR << 8) | (GBLINK_PROTO_MINOR << 4) | GBLINK_PROTO_PATCH)

#define GBLINK_VENDOR_CODE_WEBUSB 0x01u
#define GBLINK_VENDOR_CODE_MSOS2  0x02u
#define GBLINK_VENDOR_CODE_MSOS1  0x03u

#define GBLINK_IF_VENDOR   0u
#define GBLINK_IF_CDC_COMM 1u
#define GBLINK_IF_CDC_DATA 2u

#define GBLINK_EP_CDC_NOTIF 0x83u
#define GBLINK_EP_CDC_OUT   0x04u
#define GBLINK_EP_CDC_IN    0x84u

#define GBLINK_DESC_DEVICE_LEN 18u
#define GBLINK_DESC_CONFIG_LEN 112u /* 9 + (9+4*7) + 8 + (9+5+5+4+5+7) + (9+2*7) */
#define GBLINK_DESC_BOS_LEN    64u  /* 5 + WebUSB 24 + MS OS 2.0 28 + USB 2.0 ext 7 */
#define GBLINK_DESC_MSOS2_LEN  170u
#define GBLINK_DESC_URL_LEN    21u

/* String indices in the device descriptor. */
#define GBLINK_STR_MANUFACTURER 1u
#define GBLINK_STR_PRODUCT      2u
#define GBLINK_STR_SERIAL       3u

void gblink_desc_device(uint8_t out[GBLINK_DESC_DEVICE_LEN]);
void gblink_desc_config(uint8_t out[GBLINK_DESC_CONFIG_LEN]);
/* landing: whether to advertise the landing page (persisted 0x44 setting). */
void gblink_desc_bos(uint8_t out[GBLINK_DESC_BOS_LEN], bool landing);
void gblink_desc_msos2(uint8_t out[GBLINK_DESC_MSOS2_LEN]);
void gblink_desc_url(uint8_t out[GBLINK_DESC_URL_LEN]);

/*
 * Answer a vendor control request (IN, device or interface recipient).
 * Returns the number of bytes to send from *data, or -1 to stall.
 *
 *   bRequest 0x01 wIndex 0x0001  WebUSB GET_ALLOWED_ORIGINS (legacy, 13 bytes)
 *   bRequest 0x01 wIndex 0x0002  WebUSB GET_URL, wValue low byte = 1
 *   bRequest 0x02 wIndex 0x0007  MS OS 2.0 descriptor set
 *   bRequest 0x03 wIndex 0x0004  MS OS 1.0 extended compat ID
 *
 * *data points at static storage owned by this module.
 */
int gblink_desc_vendor_request(uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                               const uint8_t **data);

/* String descriptor 0xEE, the MS OS 1.0 signature "MSFT100" + vendor code. */
extern const uint8_t gblink_desc_msos1_string[18];

#ifdef __cplusplus
}
#endif

#endif /* GBLINK_USBDESC_H */
