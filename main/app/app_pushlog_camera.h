#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_pushlog_camera_init(i2c_master_bus_handle_t i2c_handle);
esp_err_t app_pushlog_camera_request_upload_now(void);

#ifdef __cplusplus
}
#endif
