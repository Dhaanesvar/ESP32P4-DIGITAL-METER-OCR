#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool app_eg91_modem_is_ready(void);
esp_err_t app_eg91_modem_http_post_jpeg(const char *url,
                                        const uint8_t *jpeg_data,
                                        size_t jpeg_len,
                                        const char *ocr_json,
                                        int *http_status);

#ifdef __cplusplus
}
#endif
