#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iot_button.h"
#include "iot_knob.h"
#include "linux/v4l2-controls.h"
#include "linux/videodev2.h"
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
#define WEB_STREAM_WIDTH 1280
#define WEB_STREAM_HEIGHT 720
#define WEB_STREAM_QUALITY 92
#define WEB_FRAME_INTERVAL_US 200000
#define WEB_RGB_BUF_SIZE (WEB_STREAM_WIDTH * WEB_STREAM_HEIGHT * 2)
#define WEB_JPEG_BUF_SIZE (WEB_RGB_BUF_SIZE / 3)
static int s_video_fd = -1;
static uint8_t *s_jpeg_buf = NULL;
static size_t s_jpeg_buf_size = 0;
static uint8_t *s_preview_buf = NULL;
static size_t s_preview_buf_size = 0;
static uint8_t *s_web_rgb_buf = NULL;
static uint8_t *s_web_jpeg_buf = NULL;
static size_t s_web_jpeg_buf_size = 0;
static size_t s_web_jpeg_len = 0;
static int64_t s_last_web_frame_us = 0;
static SemaphoreHandle_t s_web_jpeg_mutex = NULL;
static size_t s_cache_line_size = 0;
static lv_obj_t *s_canvas = NULL;
static volatile bool s_upload_in_progress = false;
static uint32_t s_warmup_frames = 0;
static volatile bool s_manual_upload_requested = false;
static int64_t s_manual_retry_after_us = 0;

typedef struct {
    bool supported;
    int32_t min;
    int32_t max;
    int32_t step;
    int32_t cur;
} ctrl_info_t;

static ctrl_info_t s_zoom_info = {0};
static ctrl_info_t s_focus_info = {0};
static bool s_autofocus_supported = false;
static bool s_autofocus_enabled = false;
static knob_handle_t s_knob = NULL;
static button_handle_t s_buttons[BSP_BUTTON_NUM] = {0};

typedef enum {
    KNOB_CTRL_ZOOM = 0,
    KNOB_CTRL_FOCUS = 1,
} knob_ctrl_mode_t;

static knob_ctrl_mode_t s_knob_mode = KNOB_CTRL_ZOOM;
static volatile int s_digital_zoom_level = 1; // 1..4

esp_err_t app_pushlog_camera_set_zoom(int32_t value);
esp_err_t app_pushlog_camera_set_focus(int32_t value);
esp_err_t app_pushlog_camera_set_autofocus(bool enable);

static int32_t clamp_ctrl_value(int32_t value, int32_t min, int32_t max, int32_t step)
{
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    if (step > 1) {
        value = min + ((value - min) / step) * step;
    }
    return value;
}

static void knob_step_apply(int dir)
{
    if (s_knob_mode == KNOB_CTRL_ZOOM) {
        // Always apply digital zoom so zoom knob works even when sensor zoom
        // control is unsupported or not effective.
        int next_level = s_digital_zoom_level + dir;
        if (next_level < 1) {
            next_level = 1;
        }
        if (next_level > 4) {
            next_level = 4;
        }
        if (next_level != s_digital_zoom_level) {
            s_digital_zoom_level = next_level;
            ESP_LOGI(TAG, "digital zoom level=%d", s_digital_zoom_level);
        }

        // Try sensor optical zoom as passthrough when available.
        if (s_zoom_info.supported) {
            int32_t delta = s_zoom_info.step > 0 ? s_zoom_info.step : 1;
            int32_t next = s_zoom_info.cur + (dir * delta);
            next = clamp_ctrl_value(next, s_zoom_info.min, s_zoom_info.max, delta);
            if (app_pushlog_camera_set_zoom(next) == ESP_OK) {
                ESP_LOGI(TAG, "sensor zoom=%ld", (long)next);
            }
        }
    } else {
        if (!s_focus_info.supported) {
            return;
        }
        if (s_autofocus_supported && s_autofocus_enabled) {
            (void)app_pushlog_camera_set_autofocus(false);
        }
        int32_t delta = s_focus_info.step > 0 ? s_focus_info.step : 1;
        int32_t next = s_focus_info.cur + (dir * delta);
        next = clamp_ctrl_value(next, s_focus_info.min, s_focus_info.max, delta);
        if (app_pushlog_camera_set_focus(next) == ESP_OK) {
            ESP_LOGI(TAG, "knob focus=%ld", (long)next);
        }
    }
}

static void knob_left_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;
    knob_step_apply(-1);
}

static void knob_right_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;
    knob_step_apply(1);
}

static void mode_button_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;

    if (s_knob_mode == KNOB_CTRL_ZOOM) {
        s_knob_mode = KNOB_CTRL_FOCUS;
        if (s_autofocus_supported) {
            (void)app_pushlog_camera_set_autofocus(false);
        }
        ESP_LOGI(TAG, "Knob mode: FOCUS");
    } else {
        s_knob_mode = KNOB_CTRL_ZOOM;
        if (s_autofocus_supported) {
            (void)app_pushlog_camera_set_autofocus(true);
        }
        ESP_LOGI(TAG, "Knob mode: ZOOM");
    }
}

static void init_hw_controls(void)
{
    esp_err_t ret = bsp_iot_button_create(s_buttons, NULL, BSP_BUTTON_NUM);
    if (ret == ESP_OK) {
        (void)iot_button_register_cb(s_buttons[BSP_BUTTON_1], BUTTON_PRESS_DOWN, NULL, mode_button_cb, NULL);
    } else {
        ESP_LOGW(TAG, "button init failed: %s", esp_err_to_name(ret));
    }

    knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_ENCODER_A,
        .gpio_encoder_b = BSP_ENCODER_B,
        .enable_power_save = false,
    };
    s_knob = iot_knob_create(&knob_cfg);
    if (s_knob) {
        (void)iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL);
        (void)iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
        ESP_LOGI(TAG, "Hardware knob ready: rotate to adjust, press button to toggle ZOOM/FOCUS");
    } else {
        ESP_LOGW(TAG, "knob init failed");
    }
}

static esp_err_t set_cam_ctrl_value(uint32_t ctrl_id, int32_t value)
{
    if (s_video_fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_ext_controls controls = {0};
    struct v4l2_ext_control control[1] = {0};

    controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = ctrl_id;
    control[0].value = value;

    if (ioctl(s_video_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "set ctrl 0x%lx failed", (unsigned long)ctrl_id);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool query_cam_ctrl_info(uint32_t ctrl_id, ctrl_info_t *info)
{
    if (!info || s_video_fd < 0) {
        return false;
    }

    struct v4l2_query_ext_ctrl qctrl = {0};
    qctrl.id = ctrl_id;
    if (ioctl(s_video_fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) != 0) {
        info->supported = false;
        return false;
    }

    info->supported = true;
    info->min = (int32_t)qctrl.minimum;
    info->max = (int32_t)qctrl.maximum;
    info->step = (int32_t)(qctrl.step ? qctrl.step : 1);

    struct v4l2_ext_controls controls = {0};
    struct v4l2_ext_control control[1] = {0};
    controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = ctrl_id;
    if (ioctl(s_video_fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
        info->cur = control[0].value;
    } else {
        info->cur = info->min;
    }

    return true;
}

static void init_web_cam_ctrls(void)
{
    (void)query_cam_ctrl_info(V4L2_CID_ZOOM_ABSOLUTE, &s_zoom_info);
    (void)query_cam_ctrl_info(V4L2_CID_FOCUS_ABSOLUTE, &s_focus_info);

    struct v4l2_query_ext_ctrl qctrl = {0};
    qctrl.id = V4L2_CID_FOCUS_AUTO;
    s_autofocus_supported = (ioctl(s_video_fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0);
    if (s_autofocus_supported) {
        struct v4l2_ext_controls controls = {0};
        struct v4l2_ext_control control[1] = {0};
        controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
        controls.count = 1;
        controls.controls = control;
        control[0].id = V4L2_CID_FOCUS_AUTO;
        if (ioctl(s_video_fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
            s_autofocus_enabled = (control[0].value != 0);
        }
    }

    ESP_LOGI(TAG,
             "controls: zoom=%s focus=%s autofocus=%s",
             s_zoom_info.supported ? "yes" : "no",
             s_focus_info.supported ? "yes" : "no",
             s_autofocus_supported ? "yes" : "no");
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

    if (s_canvas && s_preview_buf) {
        esp_err_t display_ret = app_image_process_video_frame(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            s_digital_zoom_level,
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

    if (s_web_rgb_buf && s_web_jpeg_buf && s_web_jpeg_mutex) {
        if ((now_us - s_last_web_frame_us) >= WEB_FRAME_INTERVAL_US) {
            uint32_t crop_w = 960 / s_digital_zoom_level;
            uint32_t crop_h = 540 / s_digital_zoom_level;

            if (crop_w < 240) {
                crop_w = 240;
            }
            if (crop_h < 135) {
                crop_h = 135;
            }

            esp_err_t web_ret = app_image_process_scale_crop(
                camera_buf,
                camera_buf_hes,
                camera_buf_ves,
                crop_w,
                crop_h,
                s_web_rgb_buf,
                WEB_STREAM_WIDTH,
                WEB_STREAM_HEIGHT,
                WEB_RGB_BUF_SIZE,
                PPA_SRM_ROTATION_ANGLE_0
            );

            if (web_ret == ESP_OK) {
                uint32_t web_jpeg_size = 0;
                web_ret = app_image_encode_jpeg(
                    s_web_rgb_buf,
                    WEB_STREAM_WIDTH,
                    WEB_STREAM_HEIGHT,
                    WEB_STREAM_QUALITY,
                    s_web_jpeg_buf,
                    s_web_jpeg_buf_size,
                    &web_jpeg_size
                );

                if (web_ret == ESP_OK) {
                    if (xSemaphoreTake(s_web_jpeg_mutex, 0) == pdTRUE) {
                        s_web_jpeg_len = web_jpeg_size;
                        xSemaphoreGive(s_web_jpeg_mutex);
                    }
                    s_last_web_frame_us = now_us;
                }
            }
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

    s_web_jpeg_mutex = xSemaphoreCreateMutex();
    if (s_web_jpeg_mutex) {
        size_t align = s_cache_line_size ? s_cache_line_size : 32;
        s_web_rgb_buf = heap_caps_aligned_calloc(align, 1, WEB_RGB_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        jpeg_encode_memory_alloc_cfg_t web_jpeg_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t web_jpeg_size = 0;
        s_web_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(WEB_JPEG_BUF_SIZE, &web_jpeg_mem_cfg, &web_jpeg_size);
        s_web_jpeg_buf_size = web_jpeg_size;

        if (!s_web_rgb_buf || !s_web_jpeg_buf) {
            ESP_LOGW(TAG, "web stream buffers alloc failed, continuing without web live frame endpoint");
            s_web_jpeg_len = 0;
        }
    } else {
        ESP_LOGW(TAG, "web stream mutex alloc failed, continuing without web live frame endpoint");
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

    init_web_cam_ctrls();
    init_hw_controls();

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

esp_err_t app_pushlog_camera_get_web_jpeg(uint8_t *out_buf, size_t out_buf_size, size_t *out_size)
{
    if (!out_buf || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_web_jpeg_buf || !s_web_jpeg_mutex) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (xSemaphoreTake(s_web_jpeg_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t len = s_web_jpeg_len;
    if (len == 0 || len > out_buf_size) {
        xSemaphoreGive(s_web_jpeg_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_buf, s_web_jpeg_buf, len);
    xSemaphoreGive(s_web_jpeg_mutex);

    *out_size = len;
    return ESP_OK;
}

esp_err_t app_pushlog_camera_set_zoom(int32_t value)
{
    if (!s_zoom_info.supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (value < s_zoom_info.min) {
        value = s_zoom_info.min;
    }
    if (value > s_zoom_info.max) {
        value = s_zoom_info.max;
    }

    esp_err_t ret = set_cam_ctrl_value(V4L2_CID_ZOOM_ABSOLUTE, value);
    if (ret == ESP_OK) {
        s_zoom_info.cur = value;
    }
    return ret;
}

esp_err_t app_pushlog_camera_set_focus(int32_t value)
{
    if (!s_focus_info.supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (value < s_focus_info.min) {
        value = s_focus_info.min;
    }
    if (value > s_focus_info.max) {
        value = s_focus_info.max;
    }

    esp_err_t ret = set_cam_ctrl_value(V4L2_CID_FOCUS_ABSOLUTE, value);
    if (ret == ESP_OK) {
        s_focus_info.cur = value;
    }
    return ret;
}

esp_err_t app_pushlog_camera_set_autofocus(bool enable)
{
    if (!s_autofocus_supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = set_cam_ctrl_value(V4L2_CID_FOCUS_AUTO, enable ? 1 : 0);
    if (ret == ESP_OK) {
        s_autofocus_enabled = enable;
    }
    return ret;
}

bool app_pushlog_camera_get_zoom_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur)
{
    if (!s_zoom_info.supported) {
        return false;
    }
    if (min) {
        *min = s_zoom_info.min;
    }
    if (max) {
        *max = s_zoom_info.max;
    }
    if (step) {
        *step = s_zoom_info.step;
    }
    if (cur) {
        *cur = s_zoom_info.cur;
    }
    return true;
}

bool app_pushlog_camera_get_focus_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur)
{
    if (!s_focus_info.supported) {
        return false;
    }
    if (min) {
        *min = s_focus_info.min;
    }
    if (max) {
        *max = s_focus_info.max;
    }
    if (step) {
        *step = s_focus_info.step;
    }
    if (cur) {
        *cur = s_focus_info.cur;
    }
    return true;
}

bool app_pushlog_camera_get_autofocus_state(bool *enabled)
{
    if (!s_autofocus_supported) {
        return false;
    }
    if (enabled) {
        *enabled = s_autofocus_enabled;
    }
    return true;
}
