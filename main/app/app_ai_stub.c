#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "AI/app_ai_detect.h"

static const char *TAG = "app_ai_stub";

esp_err_t app_ai_detect_init(void)
{
    ESP_LOGW(TAG, "AI detect disabled for this build");
    return ESP_OK;
}

esp_err_t app_ai_detection_init_buffers(size_t cache_line_size)
{
    (void)cache_line_size;
    return ESP_OK;
}

esp_err_t app_ai_detection_process_frame(uint8_t *detect_buf, uint32_t width, uint32_t height, int ai_detect_mode)
{
    (void)detect_buf;
    (void)width;
    (void)height;
    (void)ai_detect_mode;
    return ESP_OK;
}

esp_err_t app_ai_detection_deinit(void)
{
    return ESP_OK;
}

esp_err_t app_coco_od_detect(uint16_t *data, int width, int height)
{
    (void)data;
    (void)width;
    (void)height;
    return ESP_OK;
}

esp_err_t app_humanface_ai_detect(uint16_t *detect_buf, uint16_t *draw_buf, int width, int height)
{
    (void)detect_buf;
    (void)draw_buf;
    (void)width;
    (void)height;
    return ESP_OK;
}

esp_err_t app_pedestrian_ai_detect(uint16_t *detect_buf, uint16_t *draw_buf, int width, int height)
{
    (void)detect_buf;
    (void)draw_buf;
    (void)width;
    (void)height;
    return ESP_OK;
}
