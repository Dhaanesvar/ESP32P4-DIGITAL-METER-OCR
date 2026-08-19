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
esp_err_t app_pushlog_camera_get_web_jpeg(uint8_t *out_buf, size_t out_buf_size, size_t *out_size);
bool app_pushlog_camera_get_last_meter_reading(char *out_reading, size_t out_size, float *score);
bool app_pushlog_camera_get_last_meter_snapshot(char *out_reading, size_t out_size, float *score, uint32_t *seq);
bool app_pushlog_camera_get_last_ocr_snapshot(char *out_word,
											  size_t out_word_size,
											  char *out_reading,
											  size_t out_reading_size,
											  float *score,
											  uint32_t *seq);
esp_err_t app_pushlog_camera_set_zoom(int32_t value);
esp_err_t app_pushlog_camera_set_focus(int32_t value);
esp_err_t app_pushlog_camera_set_autofocus(bool enable);
esp_err_t app_pushlog_camera_set_auto_exposure(bool enable);
esp_err_t app_pushlog_camera_set_exposure(int32_t value);
bool app_pushlog_camera_get_zoom_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur);
bool app_pushlog_camera_get_focus_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur);
bool app_pushlog_camera_get_autofocus_state(bool *enabled);
bool app_pushlog_camera_get_exposure_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur);
bool app_pushlog_camera_get_auto_exposure_state(bool *enabled);

#ifdef __cplusplus
}
#endif
