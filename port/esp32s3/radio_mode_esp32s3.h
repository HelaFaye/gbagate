#ifndef RADIO_MODE_ESP32S3_H
#define RADIO_MODE_ESP32S3_H

#include <stdint.h>
#include "radio_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Core-0 control-plane adapter. Init is idempotent; all transitions must be
 * serialized by the app/UI task. SDR_RX remains unavailable until a real
 * ESP-SDR backend supplies its own start/stop operations. */
void radio_mode_esp32s3_init(void);
radio_mode_result_t radio_mode_esp32s3_select(radio_mode_t target);
void radio_mode_esp32s3_set_wireless(uint8_t mode, uint8_t variant);
radio_mode_result_t radio_mode_esp32s3_capture_begin(void);
radio_mode_result_t radio_mode_esp32s3_capture_end(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_MODE_ESP32S3_H */
