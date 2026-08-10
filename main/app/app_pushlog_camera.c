#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/jpeg_encode.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#define PREVIEW_CANVAS_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

#include "app_pushlog.h"
#include "app_pushlog_camera.h"
#include "app_video.h"
#include "app_video_stream.h"
#include "app_video_utils.h"

static const char *TAG = "pushlog_cam";

#define CAMERA_WARMUP_FRAMES 30
#define JPEG_QUALITY 85
#define JPEG_BUFFER_SIZE (PHOTO_WIDTH_1080P * PHOTO_HEIGHT_1088P * 2)
#define STREAM_FRAME_WIDTH 640
#define STREAM_FRAME_HEIGHT 480
#define STREAM_JPEG_QUALITY 45
#define STREAM_JPEG_BUFFER_SIZE (STREAM_FRAME_WIDTH * STREAM_FRAME_HEIGHT * 2)
#define STREAM_UPDATE_INTERVAL_US (150000)
static int s_video_fd = -1;
static uint8_t *s_jpeg_buf = NULL;
static size_t s_jpeg_buf_size = 0;
static uint8_t *s_stream_rgb_buf = NULL;
static size_t s_stream_rgb_buf_size = 0;
static uint8_t *s_stream_jpeg_buf = NULL;
static size_t s_stream_jpeg_buf_size = 0;
static size_t s_stream_jpeg_len = 0;
static int64_t s_last_stream_encode_us = 0;
static portMUX_TYPE s_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_preview_buf = NULL;
static size_t s_preview_buf_size = 0;
static size_t s_cache_line_size = 0;
static lv_obj_t *s_canvas = NULL;
static volatile bool s_upload_in_progress = false;
static uint32_t s_warmup_frames = 0;
static volatile bool s_manual_upload_requested = false;
static int64_t s_manual_retry_after_us = 0;

static void pushlog_camera_update_stream_frame(uint8_t *camera_buf,
                                               uint32_t camera_buf_hes,
                                               uint32_t camera_buf_ves,
                                               int64_t now_us)
{
    if (!s_stream_rgb_buf || !s_stream_jpeg_buf) {
        return;
    }

    if ((now_us - s_last_stream_encode_us) < STREAM_UPDATE_INTERVAL_US) {
        return;
    }

    uint32_t crop_width = camera_buf_hes;
    uint32_t crop_height = camera_buf_ves;
    uint32_t crop_width_by_height = (camera_buf_ves * 4U) / 3U;
    if (crop_width_by_height <= camera_buf_hes) {
        crop_width = crop_width_by_height;
    } else {
        crop_height = (camera_buf_hes * 3U) / 4U;
    }

    esp_err_t ret = app_image_process_scale_crop(
        camera_buf,
        camera_buf_hes,
        camera_buf_ves,
        crop_width,
        crop_height,
        s_stream_rgb_buf,
        STREAM_FRAME_WIDTH,
        STREAM_FRAME_HEIGHT,
        s_stream_rgb_buf_size,
        PPA_SRM_ROTATION_ANGLE_0
    );
    if (ret != ESP_OK) {
        return;
    }

    uint32_t stream_jpeg_size = 0;
    ret = app_image_encode_jpeg(
        s_stream_rgb_buf,
        STREAM_FRAME_WIDTH,
        STREAM_FRAME_HEIGHT,
        STREAM_JPEG_QUALITY,
        s_stream_jpeg_buf,
        s_stream_jpeg_buf_size,
        &stream_jpeg_size
    );
    if (ret != ESP_OK || stream_jpeg_size == 0) {
        return;
    }

    portENTER_CRITICAL(&s_stream_lock);
    s_stream_jpeg_len = stream_jpeg_size;
    s_last_stream_encode_us = now_us;
    portEXIT_CRITICAL(&s_stream_lock);
}

static void pushlog_camera_frame_cb(uint8_t *camera_buf,
                                    uint8_t camera_buf_index,
                                    uint32_t camera_buf_hes,
                                    uint32_t camera_buf_ves,
                                    size_t camera_buf_len)
{
    (void)camera_buf_index;
    (void)camera_buf_len;

    if (camera_buf == NULL || s_jpeg_buf == NULL) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    pushlog_camera_update_stream_frame(camera_buf, camera_buf_hes, camera_buf_ves, now_us);

    if (s_canvas && s_preview_buf) {
        esp_err_t display_ret = app_image_process_video_frame(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            1,
            PPA_SRM_ROTATION_ANGLE_0,
            s_preview_buf,
            s_preview_buf_size
        );

        if (display_ret == ESP_OK) {
            bsp_display_lock(0);
            lv_canvas_set_buffer(s_canvas, s_preview_buf, BSP_LCD_H_RES, BSP_LCD_V_RES, PREVIEW_CANVAS_COLOR_FORMAT);
            lv_refr_now(NULL);
            bsp_display_unlock();
        }
    }

    bool force_capture = false;
    if (s_manual_upload_requested && now_us >= s_manual_retry_after_us) {
        force_capture = true;
        s_manual_upload_requested = false;
    }

    if (!force_capture && s_warmup_frames < CAMERA_WARMUP_FRAMES) {
        s_warmup_frames++;
        return;
    }

    if (!force_capture && !app_pushlog_should_capture_now()) {
        return;
    }

    if (s_upload_in_progress) {
        if (force_capture) {
            s_manual_upload_requested = true;
            s_manual_retry_after_us = now_us + 500000;
        }
        return;
    }

    s_upload_in_progress = true;

    uint32_t jpeg_size = 0;
    esp_err_t ret = app_image_encode_jpeg(camera_buf,
                                          camera_buf_hes,
                                          camera_buf_ves,
                                          JPEG_QUALITY,
                                          s_jpeg_buf,
                                          s_jpeg_buf_size,
                                          &jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        s_upload_in_progress = false;
        return;
    }

    if (force_capture) {
        ESP_LOGI(TAG, "Uploading manual/boot captured image (%lu bytes)", (unsigned long)jpeg_size);
    } else {
        ESP_LOGI(TAG, "Uploading periodic captured image (%lu bytes)", (unsigned long)jpeg_size);
    }
    ret = app_pushlog_upload_jpeg(s_jpeg_buf, jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Pushlog upload failed: %s", esp_err_to_name(ret));
        if (force_capture) {
            // Re-queue manual/boot upload until Wi-Fi becomes ready.
            s_manual_upload_requested = true;
            s_manual_retry_after_us = esp_timer_get_time() + 1000000;
        }
    } else {
        if (force_capture) {
            ESP_LOGI(TAG, "Manual/boot Pushlog upload successful");
        } else {
            ESP_LOGI(TAG, "Periodic Pushlog upload successful");
        }
    }

    s_upload_in_progress = false;
}

esp_err_t app_pushlog_camera_init(i2c_master_bus_handle_t i2c_handle)
{
    (void)i2c_handle;

    esp_err_t ret = app_video_utils_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_utils_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bsp_camera_start(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_line_size);
    if (ret != ESP_OK || s_cache_line_size == 0) {
        s_cache_line_size = 64;
    }

    if (bsp_display_start()) {
        bsp_display_backlight_on();

        s_preview_buf_size = BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
        s_preview_buf = heap_caps_aligned_calloc(s_cache_line_size, 1, s_preview_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (s_preview_buf) {
            if (bsp_display_lock(1000)) {
                s_canvas = lv_canvas_create(lv_scr_act());
                lv_obj_set_size(s_canvas, BSP_LCD_H_RES, BSP_LCD_V_RES);
                lv_obj_center(s_canvas);
                bsp_display_unlock();
            }
        } else {
            ESP_LOGW(TAG, "preview buffer alloc failed, continuing without LCD preview");
        }
    } else {
        ESP_LOGW(TAG, "display init failed, continuing without LCD preview");
    }

    s_video_fd = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "app_video_open failed");
        return ESP_FAIL;
    }

    ret = app_video_set_bufs(s_video_fd, EXAMPLE_CAM_BUF_NUM, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_set_bufs failed: %s", esp_err_to_name(ret));
        return ret;
    }

    jpeg_encode_memory_alloc_cfg_t jpeg_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_BUFFER_SIZE, &jpeg_mem_cfg, &s_jpeg_buf_size);
    if (s_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate JPEG buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "JPEG output buffer allocated: requested=%u actual=%u", (unsigned)JPEG_BUFFER_SIZE, (unsigned)s_jpeg_buf_size);

    s_stream_rgb_buf_size = STREAM_FRAME_WIDTH * STREAM_FRAME_HEIGHT * 2;
    s_stream_rgb_buf = heap_caps_aligned_calloc(s_cache_line_size, 1, s_stream_rgb_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stream_rgb_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate stream RGB buffer");
        return ESP_ERR_NO_MEM;
    }

    s_stream_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(STREAM_JPEG_BUFFER_SIZE, &jpeg_mem_cfg, &s_stream_jpeg_buf_size);
    if (s_stream_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate stream JPEG buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Stream JPEG buffer allocated: requested=%u actual=%u", (unsigned)STREAM_JPEG_BUFFER_SIZE, (unsigned)s_stream_jpeg_buf_size);

    ret = app_video_register_frame_operation_cb(pushlog_camera_frame_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_register_frame_operation_cb failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = app_video_stream_task_start(s_video_fd, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_stream_task_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Force one upload soon after reset/startup.
    s_manual_upload_requested = true;
    s_manual_retry_after_us = 0;

    ESP_LOGI(TAG, "Camera stream started for periodic Pushlog upload");
    return ESP_OK;
}

esp_err_t app_pushlog_camera_request_upload_now(void)
{
    if (s_video_fd < 0 || s_jpeg_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_manual_upload_requested = true;
    s_manual_retry_after_us = 0;
    return ESP_OK;
}

esp_err_t app_pushlog_camera_copy_latest_stream_jpeg(uint8_t *dst, size_t dst_size, size_t *out_size)
{
    if (!dst || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_video_fd < 0 || !s_stream_jpeg_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = 0;
    portENTER_CRITICAL(&s_stream_lock);
    len = s_stream_jpeg_len;
    if (len > 0 && len <= dst_size) {
        memcpy(dst, s_stream_jpeg_buf, len);
    }
    portEXIT_CRITICAL(&s_stream_lock);

    if (len == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_size = len;
    return ESP_OK;
}
