#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_pushlog_init(void);
bool app_pushlog_should_capture_now(void);
esp_err_t app_pushlog_upload_jpeg(const uint8_t *jpeg_data, size_t jpeg_len);

#ifdef __cplusplus
}
#endif
