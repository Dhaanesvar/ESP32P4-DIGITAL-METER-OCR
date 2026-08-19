#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "iot_knob.h"
#include "linux/v4l2-controls.h"
#include "linux/videodev2.h"
#include "lvgl.h"

#define PREVIEW_CANVAS_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

#include "app_pushlog.h"
#include "app_pushlog_camera.h"
#include "AI/app_pushlog_ai_overlay.h"
#include "AI/app_simple_ocr_c.h"
#include "app_video.h"
#include "app_video_stream.h"
#include "app_video_utils.h"

static const char *TAG = "pushlog_cam";

#define CAMERA_WARMUP_FRAMES 30
#define JPEG_QUALITY 80
#define JPEG_QUALITY_FALLBACK 55
#define MAX_MODEM_UPLOAD_JPEG_SIZE (320U * 1024U)
#define JPEG_BUFFER_SIZE (PHOTO_WIDTH_1080P * PHOTO_HEIGHT_1088P * 2 / 5)
#define WEB_STREAM_WIDTH 960
#define WEB_STREAM_HEIGHT 540
#define WEB_STREAM_QUALITY 82
#define WEB_FRAME_INTERVAL_US 100000
#define WEB_RGB_BUF_SIZE (WEB_STREAM_WIDTH * WEB_STREAM_HEIGHT * 2)
#define WEB_JPEG_BUF_SIZE (WEB_RGB_BUF_SIZE / 3)
#define UPLOAD_QUEUE_LEN 4
#define METER_EXPECTED_DIGITS 9
#define PREVIEW_BACKLIGHT_PERCENT 62
#define DEFAULT_MANUAL_EXPOSURE_PERCENT 35
#define OCR_ROI_X0_PCT 32
#define OCR_ROI_X1_PCT 68
#define OCR_ROI_Y0_PCT 36
#define OCR_ROI_Y1_PCT 62
// Optional board button/knob controls can trigger GPIO ISR allocation issues
// on some firmware/component mixes; keep disabled for web-only pushlog flow.
static const bool kEnableHwControls = true;
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
static uint32_t s_warmup_frames = 0;
static volatile bool s_manual_upload_requested = false;
static int64_t s_manual_retry_after_us = 0;
static int64_t s_web_pause_until_us = 0;
static QueueHandle_t s_upload_queue = NULL;
static TaskHandle_t s_upload_task = NULL;
static SemaphoreHandle_t s_ocr_mutex = NULL;
static char s_last_ocr_word[32] = {0};
static char s_last_meter_reading[32] = {0};
static float s_last_meter_score = 0.0f;
static uint32_t s_last_ocr_seq = 0;
static volatile bool s_last_digit_box_valid = false;
static volatile int s_last_digit_box_x0 = 0;
static volatile int s_last_digit_box_x1 = 0;
static volatile int s_last_digit_box_y0 = 0;
static volatile int s_last_digit_box_y1 = 0;

typedef struct {
    uint8_t *jpeg_data;
    size_t jpeg_len;
    bool manual;
    uint8_t retry_count;
    char ocr_word[32];
    char meter_reading[32];
    float ocr_score;
} upload_job_t;

typedef struct {
    bool supported;
    int32_t min;
    int32_t max;
    int32_t step;
    int32_t cur;
} ctrl_info_t;

static ctrl_info_t s_zoom_info = {0};
static ctrl_info_t s_focus_info = {0};
static ctrl_info_t s_exposure_info = {0};
static bool s_autofocus_supported = false;
static bool s_autofocus_enabled = false;
static bool s_auto_exposure_supported = false;
static bool s_auto_exposure_enabled = true;
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
esp_err_t app_pushlog_camera_set_auto_exposure(bool enable);
esp_err_t app_pushlog_camera_set_exposure(int32_t value);
esp_err_t app_pushlog_camera_request_upload_now(void);

static bool map_ocr_char_to_meter_digit(char c, char *digit, float *weight, bool *is_exact)
{
    if (!digit || !weight || !is_exact) {
        return false;
    }

    char u = (char)toupper((unsigned char)c);
    if (u >= '0' && u <= '9') {
        *digit = u;
        *weight = 1.0f;
        *is_exact = true;
        return true;
    }

    *is_exact = false;
    switch (u) {
        case 'O':
        case 'Q':
        case 'D':
            *digit = '0';
            *weight = 0.82f;
            return true;
        case 'I':
        case 'L':
            *digit = '1';
            *weight = 0.78f;
            return true;
        case 'Z':
            *digit = '2';
            *weight = 0.72f;
            return true;
        case 'A':
        case 'H':
            *digit = '4';
            *weight = 0.68f;
            return true;
        case 'S':
            *digit = '5';
            *weight = 0.70f;
            return true;
        case 'G':
            *digit = '6';
            *weight = 0.66f;
            return true;
        case 'T':
            *digit = '7';
            *weight = 0.64f;
            return true;
        case 'B':
            *digit = '8';
            *weight = 0.55f;
            return true;
        default:
            return false;
    }
}

static bool is_ocr_dot_char(char c)
{
    return (c == '.' || c == ',' || c == ':');
}

static bool extract_meter_reading(const char *ocr_word, char *out_reading, size_t out_size)
{
    if (!ocr_word || !out_reading || out_size < 2) {
        return false;
    }

    typedef struct {
        char digits[32];
        bool exact[32];
        size_t digits_n;
        size_t dot_positions[8];
        size_t dot_n;
        float weight_sum;
        int exact_n;
    } token_run_t;

    token_run_t best = {0};
    token_run_t cur = {0};
    bool in_run = false;

    for (size_t i = 0; ; ++i) {
        char c = ocr_word[i];
        bool at_end = (c == '\0');
        char mapped = '\0';
        float weight = 0.0f;
        bool is_exact = false;
        bool is_digit = (!at_end && map_ocr_char_to_meter_digit(c, &mapped, &weight, &is_exact));
        bool is_dot = (!at_end && is_ocr_dot_char(c));

        if (is_digit || is_dot) {
            if (!in_run) {
                in_run = true;
                memset(&cur, 0, sizeof(cur));
            }

            if (is_digit && cur.digits_n < (sizeof(cur.digits) - 1)) {
                cur.digits[cur.digits_n] = mapped;
                cur.exact[cur.digits_n] = is_exact;
                cur.digits_n++;
                cur.weight_sum += weight;
                if (is_exact) {
                    cur.exact_n++;
                }
            } else if (is_dot && cur.dot_n < (sizeof(cur.dot_positions) / sizeof(cur.dot_positions[0]))) {
                cur.dot_positions[cur.dot_n++] = cur.digits_n;
            }
        }

        if ((at_end || (!is_digit && !is_dot)) && in_run) {
            bool better = false;
            if (cur.digits_n > best.digits_n) {
                better = true;
            } else if (cur.digits_n == best.digits_n) {
                if (cur.exact_n > best.exact_n) {
                    better = true;
                } else if (cur.exact_n == best.exact_n && cur.weight_sum > best.weight_sum) {
                    better = true;
                }
            }

            if (better || best.digits_n == 0) {
                best = cur;
            }
            in_run = false;
        }

        if (at_end) {
            break;
        }
    }

    if (best.digits_n < METER_EXPECTED_DIGITS || out_size <= METER_EXPECTED_DIGITS) {
        out_reading[0] = '\0';
        return false;
    }

    size_t start = best.digits_n - METER_EXPECTED_DIGITS;
    int exact_in_slice = 0;
    for (size_t i = start; i < best.digits_n; ++i) {
        if (best.exact[i]) {
            exact_in_slice++;
        }
    }

    // Require a minimum exact-digit signal before accepting full 9-digit decode.
    if (exact_in_slice < 4) {
        out_reading[0] = '\0';
        return false;
    }

    int best_dot_local = -1;
    int best_dot_score = -1000;
    for (size_t i = 0; i < best.dot_n; ++i) {
        size_t pos = best.dot_positions[i];
        if (pos <= start || pos >= best.digits_n) {
            continue;
        }

        int local = (int)(pos - start);
        int right_digits = (int)METER_EXPECTED_DIGITS - local;
        if (local <= 0 || local >= METER_EXPECTED_DIGITS) {
            continue;
        }

        int score = 0;
        if (right_digits >= 1 && right_digits <= 3) {
            score += 20;
        }
        score -= (right_digits > 2) ? (right_digits - 2) : (2 - right_digits);
        if (local < 2) {
            score -= 5;
        }

        if (score > best_dot_score) {
            best_dot_score = score;
            best_dot_local = local;
        }
    }

    size_t n = 0;
    for (size_t i = start; i < best.digits_n && n < (out_size - 1); ++i) {
        int local = (int)(i - start);
        if (best_dot_local >= 0 && local == best_dot_local && n < (out_size - 1)) {
            out_reading[n++] = '.';
        }
        out_reading[n++] = best.digits[i];
    }
    out_reading[n] = '\0';
    return true;
}

static bool extract_meter_fallback(const char *ocr_word, char *out_reading, size_t out_size)
{
    if (!ocr_word || !out_reading || out_size < 2) {
        return false;
    }

    size_t n = 0;
    for (size_t i = 0; ocr_word[i] != '\0'; ++i) {
        char d = '\0';
        float w = 0.0f;
        bool ex = false;
        if (map_ocr_char_to_meter_digit(ocr_word[i], &d, &w, &ex)) {
            if (n < (out_size - 1)) {
                out_reading[n++] = d;
            }
        }
    }
    out_reading[n] = '\0';
    if (n < METER_EXPECTED_DIGITS || out_size <= METER_EXPECTED_DIGITS) {
        out_reading[0] = '\0';
        return false;
    }

    if (n > METER_EXPECTED_DIGITS) {
        size_t start = n - METER_EXPECTED_DIGITS;
        memmove(out_reading, &out_reading[start], METER_EXPECTED_DIGITS);
    }
    out_reading[METER_EXPECTED_DIGITS] = '\0';
    return true;
}

static bool extract_any_digits(const char *ocr_word, char *out_reading, size_t out_size)
{
    if (!ocr_word || !out_reading || out_size < 2) {
        return false;
    }

    size_t n = 0;
    for (size_t i = 0; ocr_word[i] != '\0'; ++i) {
        char d = '\0';
        float w = 0.0f;
        bool ex = false;
        if (map_ocr_char_to_meter_digit(ocr_word[i], &d, &w, &ex)) {
            if (n < (out_size - 1)) {
                out_reading[n++] = d;
            }
        }
    }
    out_reading[n] = '\0';
    return n > 0;
}

static void get_ocr_roi(int width, int height, int *x0, int *x1, int *y0, int *y1)
{
    int rx0 = (width * OCR_ROI_X0_PCT) / 100;
    int rx1 = (width * OCR_ROI_X1_PCT) / 100;
    int ry0 = (height * OCR_ROI_Y0_PCT) / 100;
    int ry1 = (height * OCR_ROI_Y1_PCT) / 100;

    if (rx1 <= rx0 + 40) {
        rx1 = rx0 + 40;
    }
    if (ry1 <= ry0 + 20) {
        ry1 = ry0 + 20;
    }
    if (rx1 > width) {
        rx1 = width;
    }
    if (ry1 > height) {
        ry1 = height;
    }

    *x0 = rx0;
    *x1 = rx1;
    *y0 = ry0;
    *y1 = ry1;
}

static void draw_ocr_roi_box(uint16_t *buf, int width, int height)
{
    if (!buf || width <= 0 || height <= 0) {
        return;
    }

    int x0 = (width * OCR_ROI_X0_PCT) / 100;
    int x1 = (width * OCR_ROI_X1_PCT) / 100;
    int y0 = (height * OCR_ROI_Y0_PCT) / 100;
    int y1 = (height * OCR_ROI_Y1_PCT) / 100;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= width) {
        x1 = width - 1;
    }
    if (y1 >= height) {
        y1 = height - 1;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    const uint16_t green = 0x07E0; // RGB565 pure green
    const int thick = 3;

    for (int t = 0; t < thick; ++t) {
        int yt = y0 + t;
        int yb = y1 - t;
        if (yt >= 0 && yt < height) {
            for (int x = x0; x <= x1; ++x) {
                buf[yt * width + x] = green;
            }
        }
        if (yb >= 0 && yb < height && yb != yt) {
            for (int x = x0; x <= x1; ++x) {
                buf[yb * width + x] = green;
            }
        }
    }

    for (int t = 0; t < thick; ++t) {
        int xl = x0 + t;
        int xr = x1 - t;
        if (xl >= 0 && xl < width) {
            for (int y = y0; y <= y1; ++y) {
                buf[y * width + xl] = green;
            }
        }
        if (xr >= 0 && xr < width && xr != xl) {
            for (int y = y0; y <= y1; ++y) {
                buf[y * width + xr] = green;
            }
        }
    }

    // Draw refined digit region as yellow to match precise detection area.
    if (s_last_digit_box_valid) {
        int dx0 = s_last_digit_box_x0;
        int dx1 = s_last_digit_box_x1;
        int dy0 = s_last_digit_box_y0;
        int dy1 = s_last_digit_box_y1;

        if (dx0 < 0) {
            dx0 = 0;
        }
        if (dy0 < 0) {
            dy0 = 0;
        }
        if (dx1 >= width) {
            dx1 = width - 1;
        }
        if (dy1 >= height) {
            dy1 = height - 1;
        }

        if (dx1 > dx0 && dy1 > dy0) {
            const uint16_t yellow = 0xFFE0; // RGB565 yellow
            const int dthick = 2;

            for (int t = 0; t < dthick; ++t) {
                int yt = dy0 + t;
                int yb = dy1 - t;
                if (yt >= 0 && yt < height) {
                    for (int x = dx0; x <= dx1; ++x) {
                        buf[yt * width + x] = yellow;
                    }
                }
                if (yb >= 0 && yb < height && yb != yt) {
                    for (int x = dx0; x <= dx1; ++x) {
                        buf[yb * width + x] = yellow;
                    }
                }
            }

            for (int t = 0; t < dthick; ++t) {
                int xl = dx0 + t;
                int xr = dx1 - t;
                if (xl >= 0 && xl < width) {
                    for (int y = dy0; y <= dy1; ++y) {
                        buf[y * width + xl] = yellow;
                    }
                }
                if (xr >= 0 && xr < width && xr != xl) {
                    for (int y = dy0; y <= dy1; ++y) {
                        buf[y * width + xr] = yellow;
                    }
                }
            }
        }
    }
}

static void clamp_roi(int width, int height, int *x0, int *x1, int *y0, int *y1)
{
    if (*x0 < 0) {
        *x0 = 0;
    }
    if (*y0 < 0) {
        *y0 = 0;
    }
    if (*x1 > width) {
        *x1 = width;
    }
    if (*y1 > height) {
        *y1 = height;
    }
    if (*x1 <= *x0 + 40) {
        *x1 = (*x0 + 40 <= width) ? (*x0 + 40) : width;
        *x0 = (*x1 >= 40) ? (*x1 - 40) : 0;
    }
    if (*y1 <= *y0 + 20) {
        *y1 = (*y0 + 20 <= height) ? (*y0 + 20) : height;
        *y0 = (*y1 >= 20) ? (*y1 - 20) : 0;
    }
}

static void update_best_ocr_candidate(const char *word,
                                      float score,
                                      int stage,
                                      int x0,
                                      int x1,
                                      int y0,
                                      int y1,
                                      char *best_word,
                                      size_t best_word_size,
                                      float *best_score,
                                      int *best_stage,
                                      int *best_x0,
                                      int *best_x1,
                                      int *best_y0,
                                      int *best_y1,
                                      int *best_digits,
                                      int *best_exact_digits)
{
    int digits = 0;
    int exact_digits = 0;
    for (size_t i = 0; word[i] != '\0'; ++i) {
        char d = '\0';
        float w = 0.0f;
        bool ex = false;
        if (map_ocr_char_to_meter_digit(word[i], &d, &w, &ex)) {
            digits++;
            if (ex) {
                exact_digits++;
            }
        }
    }

    bool better = false;
    if (digits > *best_digits) {
        better = true;
    } else if (digits == *best_digits) {
        if (exact_digits > *best_exact_digits) {
            better = true;
        } else if (exact_digits == *best_exact_digits && score > *best_score) {
            better = true;
        }
    }

    if (better) {
        strncpy(best_word, word, best_word_size - 1);
        best_word[best_word_size - 1] = '\0';
        *best_score = score;
        *best_stage = stage;
        *best_x0 = x0;
        *best_x1 = x1;
        *best_y0 = y0;
        *best_y1 = y1;
        *best_digits = digits;
        *best_exact_digits = exact_digits;
    }
}

static bool run_ocr_with_fallbacks(const uint16_t *frame,
                                   int width,
                                   int height,
                                   char *ocr_word,
                                   size_t ocr_word_size,
                                   float *ocr_score,
                                   int *used_stage,
                                   int *used_x0,
                                   int *used_x1,
                                   int *used_y0,
                                   int *used_y1)
{
    if (!frame || !ocr_word || ocr_word_size < 2 || !ocr_score) {
        return false;
    }

    int bx0 = 0;
    int bx1 = 0;
    int by0 = 0;
    int by1 = 0;
    get_ocr_roi(width, height, &bx0, &bx1, &by0, &by1);

    typedef struct {
        int x0;
        int x1;
        int y0;
        int y1;
    } roi_t;

    int pad_w1 = width / 16;
    int pad_h1 = height / 20;
    int pad_w2 = width / 10;
    int pad_h2 = height / 12;
    int y_shift = height / 12;

    roi_t rois[6] = {
        {bx0, bx1, by0, by1},
        {bx0 - pad_w1, bx1 + pad_w1, by0 - pad_h1, by1 + pad_h1},
        {bx0 - pad_w2, bx1 + pad_w2, by0 - pad_h2, by1 + pad_h2},
        {bx0, bx1, by0 - y_shift, by1 - y_shift},
        {bx0, bx1, by0 + y_shift, by1 + y_shift},
        {width / 6, (width * 5) / 6, height / 4, (height * 3) / 4},
    };

    char best_word[32] = {0};
    float best_score = 0.0f;
    int best_stage = -1;
    int best_x0 = 0;
    int best_x1 = 0;
    int best_y0 = 0;
    int best_y1 = 0;
    int best_digits = -1;
    int best_exact_digits = -1;

    for (int i = 0; i < 6; ++i) {
        int x0 = rois[i].x0;
        int x1 = rois[i].x1;
        int y0 = rois[i].y0;
        int y1 = rois[i].y1;
        clamp_roi(width, height, &x0, &x1, &y0, &y1);

        bool ok = app_simple_ocr_extract_word_c_roi(frame,
                                                    width,
                                                    height,
                                                    x0,
                                                    x1,
                                                    y0,
                                                    y1,
                                                    ocr_word,
                                                    ocr_word_size,
                                                    ocr_score);
        if (ok) {
            update_best_ocr_candidate(ocr_word,
                                      *ocr_score,
                                      i,
                                      x0,
                                      x1,
                                      y0,
                                      y1,
                                      best_word,
                                      sizeof(best_word),
                                      &best_score,
                                      &best_stage,
                                      &best_x0,
                                      &best_x1,
                                      &best_y0,
                                      &best_y1,
                                      &best_digits,
                                      &best_exact_digits);
        }
    }

    bool full_ok = app_simple_ocr_extract_word_c(frame, width, height, ocr_word, ocr_word_size, ocr_score);
    if (full_ok) {
        update_best_ocr_candidate(ocr_word,
                                  *ocr_score,
                                  99,
                                  0,
                                  width,
                                  0,
                                  height,
                                  best_word,
                                  sizeof(best_word),
                                  &best_score,
                                  &best_stage,
                                  &best_x0,
                                  &best_x1,
                                  &best_y0,
                                  &best_y1,
                                  &best_digits,
                                  &best_exact_digits);
    }

    if (best_stage < 0 || best_digits <= 0) {
        return false;
    }

    int refined_x0 = 0;
    int refined_x1 = 0;
    int refined_y0 = 0;
    int refined_y1 = 0;
    if (app_simple_ocr_refine_digit_roi_c(frame,
                                          width,
                                          height,
                                          best_x0,
                                          best_x1,
                                          best_y0,
                                          best_y1,
                                          &refined_x0,
                                          &refined_x1,
                                          &refined_y0,
                                          &refined_y1)) {
        best_x0 = refined_x0;
        best_x1 = refined_x1;
        best_y0 = refined_y0;
        best_y1 = refined_y1;
    }

    strncpy(ocr_word, best_word, ocr_word_size - 1);
    ocr_word[ocr_word_size - 1] = '\0';
    *ocr_score = best_score;
    if (used_stage) {
        *used_stage = best_stage;
    }
    if (used_x0) {
        *used_x0 = best_x0;
    }
    if (used_x1) {
        *used_x1 = best_x1;
    }
    if (used_y0) {
        *used_y0 = best_y0;
    }
    if (used_y1) {
        *used_y1 = best_y1;
    }
    return true;
}

static bool enqueue_upload_job(uint8_t *jpeg_data,
                               size_t jpeg_len,
                               bool manual,
                               const char *ocr_word,
                               const char *meter_reading,
                               float ocr_score)
{
    if (!s_upload_queue || !jpeg_data || jpeg_len == 0) {
        return false;
    }

    upload_job_t job = {
        .jpeg_data = jpeg_data,
        .jpeg_len = jpeg_len,
        .manual = manual,
        .retry_count = 0,
        .ocr_score = ocr_score,
    };
    if (ocr_word && ocr_word[0] != '\0') {
        strncpy(job.ocr_word, ocr_word, sizeof(job.ocr_word) - 1);
        job.ocr_word[sizeof(job.ocr_word) - 1] = '\0';
    } else {
        job.ocr_word[0] = '\0';
    }

    if (meter_reading && meter_reading[0] != '\0') {
        strncpy(job.meter_reading, meter_reading, sizeof(job.meter_reading) - 1);
        job.meter_reading[sizeof(job.meter_reading) - 1] = '\0';
    } else {
        job.meter_reading[0] = '\0';
    }

    BaseType_t ok = manual
        ? xQueueSendToFront(s_upload_queue, &job, 0)
        : xQueueSendToBack(s_upload_queue, &job, 0);

    if (ok == pdTRUE) {
        return true;
    }

    if (manual) {
        upload_job_t dropped;
        if (xQueueReceive(s_upload_queue, &dropped, 0) == pdTRUE) {
            if (dropped.jpeg_data) {
                heap_caps_free(dropped.jpeg_data);
            }
            ok = xQueueSendToFront(s_upload_queue, &job, 0);
            if (ok == pdTRUE) {
                ESP_LOGW(TAG, "upload queue full; dropped oldest job to prioritize manual snap");
                return true;
            }
        }
    }

    return false;
}

static void pushlog_upload_task(void *arg)
{
    (void)arg;
    upload_job_t job;

    while (1) {
        if (xQueueReceive(s_upload_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t ret = app_pushlog_upload_jpeg(job.jpeg_data,
                            job.jpeg_len,
                            job.ocr_word,
                            job.meter_reading,
                            job.ocr_score);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Pushlog upload failed: %s", esp_err_to_name(ret));

            if (job.manual && job.retry_count < 30) {
                job.retry_count++;
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (xQueueSendToFront(s_upload_queue, &job, 0) == pdTRUE) {
                    ESP_LOGW(TAG, "Manual/boot upload retry %d/30", job.retry_count);
                    continue;
                }
            }
        } else {
            if (job.manual) {
                ESP_LOGI(TAG, "Manual/boot Pushlog upload successful");
            } else {
                ESP_LOGI(TAG, "Periodic Pushlog upload successful");
            }
        }

        if (job.jpeg_data) {
            heap_caps_free(job.jpeg_data);
        }
    }
}

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
        ESP_LOGI(TAG, "Knob mode: FOCUS (AI mode: face)");
    } else {
        s_knob_mode = KNOB_CTRL_ZOOM;
        if (s_autofocus_supported) {
            (void)app_pushlog_camera_set_autofocus(true);
        }
        ESP_LOGI(TAG, "Knob mode: ZOOM (AI mode: pedestrian)");
    }
}

static void snap_button_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;

    esp_err_t ret = app_pushlog_camera_request_upload_now();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "User button snap requested");
    } else {
        ESP_LOGW(TAG, "User button snap request failed: %s", esp_err_to_name(ret));
    }
}

static void init_hw_controls(void)
{
    esp_err_t ret = bsp_iot_button_create(s_buttons, NULL, BSP_BUTTON_NUM);
    if (ret == ESP_OK) {
        // Single click: capture/upload now. Long press: toggle knob control mode.
        (void)iot_button_register_cb(s_buttons[BSP_BUTTON_1], BUTTON_SINGLE_CLICK, NULL, snap_button_cb, NULL);
        (void)iot_button_register_cb(s_buttons[BSP_BUTTON_1], BUTTON_LONG_PRESS_START, NULL, mode_button_cb, NULL);
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
    (void)query_cam_ctrl_info(V4L2_CID_EXPOSURE_ABSOLUTE, &s_exposure_info);

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

    struct v4l2_query_ext_ctrl qexp = {0};
    qexp.id = V4L2_CID_EXPOSURE_AUTO;
    s_auto_exposure_supported = (ioctl(s_video_fd, VIDIOC_QUERY_EXT_CTRL, &qexp) == 0);
    if (s_auto_exposure_supported) {
        struct v4l2_ext_controls controls = {0};
        struct v4l2_ext_control control[1] = {0};
        controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
        controls.count = 1;
        controls.controls = control;
        control[0].id = V4L2_CID_EXPOSURE_AUTO;
        if (ioctl(s_video_fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
            s_auto_exposure_enabled = (control[0].value == V4L2_EXPOSURE_AUTO);
        }
    }

    ESP_LOGI(TAG,
             "controls: zoom=%s focus=%s autofocus=%s exposure=%s auto_exposure=%s",
             s_zoom_info.supported ? "yes" : "no",
             s_focus_info.supported ? "yes" : "no",
             s_autofocus_supported ? "yes" : "no",
             s_exposure_info.supported ? "yes" : "no",
             s_auto_exposure_supported ? "yes" : "no");

    if (s_autofocus_supported && !s_autofocus_enabled) {
        if (app_pushlog_camera_set_autofocus(true) == ESP_OK) {
            ESP_LOGI(TAG, "autofocus enabled at startup");
        }
    }

    // Reduce white blowout/flicker on live preview by preferring a stable
    // manual exposure baseline when exposure controls are available.
    if (s_auto_exposure_supported && s_exposure_info.supported) {
        if (app_pushlog_camera_set_auto_exposure(false) == ESP_OK) {
            int32_t span = s_exposure_info.max - s_exposure_info.min;
            int32_t target = s_exposure_info.min + (span * DEFAULT_MANUAL_EXPOSURE_PERCENT) / 100;
            if (s_exposure_info.step > 1) {
                target = s_exposure_info.min + ((target - s_exposure_info.min) / s_exposure_info.step) * s_exposure_info.step;
            }
            if (target < s_exposure_info.min) {
                target = s_exposure_info.min;
            }
            if (target > s_exposure_info.max) {
                target = s_exposure_info.max;
            }
            if (app_pushlog_camera_set_exposure(target) == ESP_OK) {
                ESP_LOGI(TAG, "startup manual exposure=%ld", (long)target);
            }
        }
    }
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

    // Update precise digit ROI from live frame for yellow overlay box.
    {
        int rx0 = 0;
        int rx1 = 0;
        int ry0 = 0;
        int ry1 = 0;
        get_ocr_roi((int)camera_buf_hes, (int)camera_buf_ves, &rx0, &rx1, &ry0, &ry1);

        int dx0 = 0;
        int dx1 = 0;
        int dy0 = 0;
        int dy1 = 0;
        bool refined_ok = app_simple_ocr_refine_digit_roi_c((const uint16_t *)camera_buf,
                                                             (int)camera_buf_hes,
                                                             (int)camera_buf_ves,
                                                             rx0,
                                                             rx1,
                                                             ry0,
                                                             ry1,
                                                             &dx0,
                                                             &dx1,
                                                             &dy0,
                                                             &dy1);

        if (refined_ok) {
            s_last_digit_box_x0 = (dx0 * BSP_LCD_H_RES) / (int)camera_buf_hes;
            s_last_digit_box_x1 = (dx1 * BSP_LCD_H_RES) / (int)camera_buf_hes;
            s_last_digit_box_y0 = (dy0 * BSP_LCD_V_RES) / (int)camera_buf_ves;
            s_last_digit_box_y1 = (dy1 * BSP_LCD_V_RES) / (int)camera_buf_ves;
            s_last_digit_box_valid = true;
        } else {
            s_last_digit_box_valid = false;
        }
    }

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
            bool face_mode = (s_knob_mode == KNOB_CTRL_FOCUS);
            app_pushlog_ai_overlay((uint16_t *)s_preview_buf, BSP_LCD_H_RES, BSP_LCD_V_RES, face_mode);
            draw_ocr_roi_box((uint16_t *)s_preview_buf, BSP_LCD_H_RES, BSP_LCD_V_RES);

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
        // No Pushlog work due now; allow web frame generation below.
    } else {
        int roi_x0 = 0;
        int roi_x1 = 0;
        int roi_y0 = 0;
        int roi_y1 = 0;
        int ocr_stage = -1;

        char ocr_word[32] = {0};
        char meter_reading[32] = {0};
        float ocr_score = 0.0f;
        bool ocr_ok = run_ocr_with_fallbacks((const uint16_t *)camera_buf,
                             (int)camera_buf_hes,
                             (int)camera_buf_ves,
                             ocr_word,
                             sizeof(ocr_word),
                             &ocr_score,
                             &ocr_stage,
                             &roi_x0,
                             &roi_x1,
                             &roi_y0,
                             &roi_y1);
        bool meter_ok = ocr_ok && extract_meter_reading(ocr_word, meter_reading, sizeof(meter_reading));
        bool meter_fallback_ok = false;
        bool meter_partial_ok = false;
        if (!meter_ok && ocr_ok) {
            meter_fallback_ok = extract_meter_fallback(ocr_word, meter_reading, sizeof(meter_reading));
            if (!meter_fallback_ok) {
                meter_partial_ok = extract_any_digits(ocr_word, meter_reading, sizeof(meter_reading));
            }
        }

        if (s_ocr_mutex && xSemaphoreTake(s_ocr_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (ocr_ok && ocr_word[0] != '\0') {
                strncpy(s_last_ocr_word, ocr_word, sizeof(s_last_ocr_word) - 1);
                s_last_ocr_word[sizeof(s_last_ocr_word) - 1] = '\0';
                s_last_ocr_seq++;
            }

            if (meter_ok || meter_fallback_ok || meter_partial_ok) {
                strncpy(s_last_meter_reading, meter_reading, sizeof(s_last_meter_reading) - 1);
                s_last_meter_reading[sizeof(s_last_meter_reading) - 1] = '\0';
                if (meter_ok) {
                    s_last_meter_score = ocr_score;
                } else if (meter_fallback_ok) {
                    s_last_meter_score = ocr_score * 0.70f;
                } else {
                    s_last_meter_score = ocr_score * 0.55f;
                }
            }

            if (ocr_ok) {
                if (meter_ok) {
                    s_last_meter_score = ocr_score;
                } else if (meter_fallback_ok) {
                    s_last_meter_score = ocr_score * 0.70f;
                } else if (meter_partial_ok) {
                    s_last_meter_score = ocr_score * 0.55f;
                } else {
                    s_last_meter_score = ocr_score;
                }
            }
            xSemaphoreGive(s_ocr_mutex);
        }

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
            return;
        }

        if (jpeg_size > MAX_MODEM_UPLOAD_JPEG_SIZE) {
            uint32_t smaller_jpeg_size = 0;
            esp_err_t smaller_ret = app_image_encode_jpeg(camera_buf,
                                                          camera_buf_hes,
                                                          camera_buf_ves,
                                                          JPEG_QUALITY_FALLBACK,
                                                          s_jpeg_buf,
                                                          s_jpeg_buf_size,
                                                          &smaller_jpeg_size);
            if (smaller_ret == ESP_OK && smaller_jpeg_size > 0 && smaller_jpeg_size < jpeg_size) {
                ESP_LOGW(TAG,
                         "JPEG too large for EG91 (%lu bytes), re-encoded at quality %d -> %lu bytes",
                         (unsigned long)jpeg_size,
                         JPEG_QUALITY_FALLBACK,
                         (unsigned long)smaller_jpeg_size);
                jpeg_size = smaller_jpeg_size;
            }
        }

        uint8_t *job_buf = heap_caps_malloc(jpeg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!job_buf) {
            ESP_LOGE(TAG, "failed to allocate upload job buffer (%lu bytes)", (unsigned long)jpeg_size);
            if (force_capture) {
                s_manual_upload_requested = true;
                s_manual_retry_after_us = now_us + 200000;
            }
            return;
        }
        memcpy(job_buf, s_jpeg_buf, jpeg_size);

        if (force_capture) {
            ESP_LOGI(TAG, "Queue manual/boot captured image (%lu bytes)", (unsigned long)jpeg_size);
            if (meter_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] reading=%s", meter_reading);
            } else if (meter_fallback_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] fallback reading=%s", meter_reading);
            } else if (meter_partial_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] partial reading=%s", meter_reading);
            } else if (ocr_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] no reliable meter digits");
            } else {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d no text detected", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1);
            }
            s_web_pause_until_us = now_us + 3000000; // prioritize immediate manual upload path
        } else {
            ESP_LOGI(TAG, "Queue periodic captured image (%lu bytes)", (unsigned long)jpeg_size);
            if (meter_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] reading=%s", meter_reading);
            } else if (meter_fallback_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] fallback reading=%s", meter_reading);
            } else if (meter_partial_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
                ESP_LOGI(TAG, "[ocr_meter] partial reading=%s", meter_reading);
            } else if (ocr_ok) {
                ESP_LOGI(TAG, "[ocr_snap] stage=%d roi=%d,%d,%d,%d word=%s score=%.2f", ocr_stage, roi_x0, roi_y0, roi_x1, roi_y1, ocr_word, ocr_score);
            }
        }

        const char *job_ocr_word = ocr_ok ? ocr_word : "";
        const char *job_meter = (meter_ok || meter_fallback_ok || meter_partial_ok) ? meter_reading : "";
        float job_score = meter_ok ? ocr_score : (meter_fallback_ok ? (ocr_score * 0.70f) : (meter_partial_ok ? (ocr_score * 0.55f) : (ocr_ok ? ocr_score : 0.0f)));
        if (!enqueue_upload_job(job_buf, jpeg_size, force_capture, job_ocr_word, job_meter, job_score)) {
            heap_caps_free(job_buf);
            if (force_capture) {
                ESP_LOGW(TAG, "upload queue busy; retrying manual snap shortly");
                s_manual_upload_requested = true;
                s_manual_retry_after_us = now_us + 200000;
            }
        }
    }

    if (!force_capture && s_web_rgb_buf && s_web_jpeg_buf && s_web_jpeg_mutex) {
        if (now_us < s_web_pause_until_us) {
            return;
        }
        if (s_upload_queue && uxQueueMessagesWaiting(s_upload_queue) > 0) {
            return;
        }
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
        (void)bsp_display_brightness_set(PREVIEW_BACKLIGHT_PERCENT);

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
    s_ocr_mutex = xSemaphoreCreateMutex();
    if (!s_ocr_mutex) {
        ESP_LOGW(TAG, "failed to create OCR mutex; latest meter reading endpoint will be unstable");
    }

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
    ret = app_pushlog_ai_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AI overlay init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (kEnableHwControls) {
        init_hw_controls();
    } else {
        ESP_LOGW(TAG, "hardware button/knob controls disabled for stability");
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

    s_upload_queue = xQueueCreate(UPLOAD_QUEUE_LEN, sizeof(upload_job_t));
    if (!s_upload_queue) {
        ESP_LOGE(TAG, "failed to create upload queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        pushlog_upload_task,
        "pushlog_upload",
        8192,
        NULL,
        8,
        &s_upload_task,
        tskNO_AFFINITY
    );
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create upload task");
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
    s_web_pause_until_us = esp_timer_get_time() + 3000000;
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

bool app_pushlog_camera_get_last_meter_reading(char *out_reading, size_t out_size, float *score)
{
    if (!out_reading || out_size < 2) {
        return false;
    }

    out_reading[0] = '\0';
    if (score) {
        *score = 0.0f;
    }

    if (!s_ocr_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_ocr_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool ok = (s_last_meter_reading[0] != '\0');
    if (ok) {
        strncpy(out_reading, s_last_meter_reading, out_size - 1);
        out_reading[out_size - 1] = '\0';
        if (score) {
            *score = s_last_meter_score;
        }
    }

    xSemaphoreGive(s_ocr_mutex);
    return ok;
}

bool app_pushlog_camera_get_last_meter_snapshot(char *out_reading, size_t out_size, float *score, uint32_t *seq)
{
    if (!out_reading || out_size < 2) {
        return false;
    }

    out_reading[0] = '\0';
    if (score) {
        *score = 0.0f;
    }
    if (seq) {
        *seq = 0;
    }

    if (!s_ocr_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_ocr_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool ok = (s_last_meter_reading[0] != '\0');
    if (ok) {
        strncpy(out_reading, s_last_meter_reading, out_size - 1);
        out_reading[out_size - 1] = '\0';
        if (score) {
            *score = s_last_meter_score;
        }
    }
    if (seq) {
        *seq = s_last_ocr_seq;
    }

    xSemaphoreGive(s_ocr_mutex);
    return ok;
}

bool app_pushlog_camera_get_last_ocr_snapshot(char *out_word,
                                              size_t out_word_size,
                                              char *out_reading,
                                              size_t out_reading_size,
                                              float *score,
                                              uint32_t *seq)
{
    if (!out_word || out_word_size < 2 || !out_reading || out_reading_size < 2) {
        return false;
    }

    out_word[0] = '\0';
    out_reading[0] = '\0';
    if (score) {
        *score = 0.0f;
    }
    if (seq) {
        *seq = 0;
    }

    if (!s_ocr_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_ocr_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool has_word = (s_last_ocr_word[0] != '\0');
    bool has_reading = (s_last_meter_reading[0] != '\0');

    if (has_word) {
        strncpy(out_word, s_last_ocr_word, out_word_size - 1);
        out_word[out_word_size - 1] = '\0';
    }
    if (has_reading) {
        strncpy(out_reading, s_last_meter_reading, out_reading_size - 1);
        out_reading[out_reading_size - 1] = '\0';
    }
    if (score) {
        *score = s_last_meter_score;
    }
    if (seq) {
        *seq = s_last_ocr_seq;
    }

    xSemaphoreGive(s_ocr_mutex);
    return has_word || has_reading;
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

esp_err_t app_pushlog_camera_set_auto_exposure(bool enable)
{
    if (!s_auto_exposure_supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int32_t mode = enable ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
    esp_err_t ret = set_cam_ctrl_value(V4L2_CID_EXPOSURE_AUTO, mode);
    if (ret == ESP_OK) {
        s_auto_exposure_enabled = enable;
    }
    return ret;
}

esp_err_t app_pushlog_camera_set_exposure(int32_t value)
{
    if (!s_exposure_info.supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (value < s_exposure_info.min) {
        value = s_exposure_info.min;
    }
    if (value > s_exposure_info.max) {
        value = s_exposure_info.max;
    }

    if (s_auto_exposure_supported && s_auto_exposure_enabled) {
        esp_err_t mode_ret = app_pushlog_camera_set_auto_exposure(false);
        if (mode_ret != ESP_OK) {
            return mode_ret;
        }
    }

    esp_err_t ret = set_cam_ctrl_value(V4L2_CID_EXPOSURE_ABSOLUTE, value);
    if (ret == ESP_OK) {
        s_exposure_info.cur = value;
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

bool app_pushlog_camera_get_exposure_range(int32_t *min, int32_t *max, int32_t *step, int32_t *cur)
{
    if (!s_exposure_info.supported) {
        return false;
    }
    if (min) {
        *min = s_exposure_info.min;
    }
    if (max) {
        *max = s_exposure_info.max;
    }
    if (step) {
        *step = s_exposure_info.step;
    }
    if (cur) {
        *cur = s_exposure_info.cur;
    }
    return true;
}

bool app_pushlog_camera_get_auto_exposure_state(bool *enabled)
{
    if (!s_auto_exposure_supported) {
        return false;
    }
    if (enabled) {
        *enabled = s_auto_exposure_enabled;
    }
    return true;
}
