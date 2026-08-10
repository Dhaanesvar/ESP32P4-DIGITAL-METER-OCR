#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_pushlog_camera_init(i2c_master_bus_handle_t i2c_handle);
esp_err_t app_pushlog_camera_request_upload_now(void);
esp_err_t app_pushlog_camera_copy_latest_stream_jpeg(uint8_t *dst, size_t dst_size, size_t *out_size);

#ifdef __cplusplus
}
#endif
