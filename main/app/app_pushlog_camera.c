#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
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
#define JPEG_BUFFER_SIZE (PHOTO_WIDTH_1080P * PHOTO_HEIGHT_1088P * 2 / 5)
static int s_video_fd = -1;
static uint8_t *s_jpeg_buf = NULL;
static size_t s_jpeg_buf_size = 0;
static uint8_t *s_preview_buf = NULL;
static size_t s_preview_buf_size = 0;
static size_t s_cache_line_size = 0;
static lv_obj_t *s_canvas = NULL;
static volatile bool s_upload_in_progress = false;
static uint32_t s_warmup_frames = 0;
static volatile bool s_manual_upload_requested = false;
static int64_t s_manual_retry_after_us = 0;

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

    if (bsp_display_start()) {
        bsp_display_backlight_on();

        ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_line_size);
        if (ret == ESP_OK) {
            s_preview_buf_size = BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
            s_preview_buf = heap_caps_aligned_calloc(s_cache_line_size, 1, s_preview_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

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
    size_t jpeg_buf_size = 0;
    s_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_BUFFER_SIZE, &jpeg_mem_cfg, &jpeg_buf_size);
    s_jpeg_buf_size = jpeg_buf_size;
    if (s_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate JPEG buffer");
        return ESP_ERR_NO_MEM;
    }

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
