#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_pushlog_ai_init(void);

// Draw detection overlays directly into an RGB565 frame buffer.
void app_pushlog_ai_overlay(uint16_t *frame, int width, int height, bool face_mode);

#ifdef __cplusplus
}
#endif
