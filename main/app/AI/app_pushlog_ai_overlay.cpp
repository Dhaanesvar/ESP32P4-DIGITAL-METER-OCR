#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <list>
#include <vector>
#include <string>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

#include "app_humanface_detect.h"
#include "app_pedestrian_detect.h"
#include "app_drawing_utils.h"
#include "app_pushlog_ai_overlay.h"
#include "app_simple_ocr.hpp"

static const char *TAG = "pushlog_ai";

// Host Roboflow bridge is the active OCR path. Keep local digits model disabled
// because current espdl file uses unsupported quantization/op set on this runtime.
static constexpr bool kEnableLocalDigitsModel = false;

static uint32_t s_face_frame_count = 0;
static uint32_t s_ped_frame_count = 0;
static uint32_t s_ocr_frame_count = 0;
static int64_t s_ocr_last_log_us = 0;
static std::string s_ocr_last_word;
static int64_t s_ocr_last_update_us = 0;
static std::vector<ocr_digit_prediction_t> s_ocr_digit_boxes;

extern const uint8_t digits_ocr_espdl[] asm("_binary_digits_ocr_espdl_start");

static dl::Model *s_digits_model = nullptr;
static bool s_digits_model_ready = false;

struct ModelDet {
    int cls;
    float score;
    float x0;
    float y0;
    float x1;
    float y1;
};

struct OcrCandidate {
    std::string word;
    float score;
    int64_t ts_us;
};

static std::vector<OcrCandidate> s_ocr_recent;

static std::string ocr_sanitize_word(const std::string &word)
{
    std::string out;
    out.reserve(word.size());
    for (char c : word) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::toupper(uc)));
        }
    }
    return out;
}

static bool ocr_word_clean(const std::string &word)
{
    if (word.size() < 2 || word.size() > 12) {
        return false;
    }

    for (char c : word) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    // Reject low-information strings like 00000 / BBBBB that commonly come from texture noise.
    std::array<int, 36> hist = {0};
    int unique = 0;
    for (char c : word) {
        int idx = -1;
        if (c >= '0' && c <= '9') {
            idx = c - '0';
        } else if (c >= 'A' && c <= 'Z') {
            idx = 10 + (c - 'A');
        }
        if (idx >= 0) {
            if (hist[static_cast<size_t>(idx)] == 0) {
                unique++;
            }
            hist[static_cast<size_t>(idx)]++;
        }
    }

    if (word.size() >= 4 && unique < 2) {
        return false;
    }

    int max_count = 0;
    for (int v : hist) {
        max_count = std::max(max_count, v);
    }
    if (word.size() >= 5 && max_count >= static_cast<int>(word.size() - 1)) {
        return false;
    }

    return true;
}

static void ocr_prune_old(int64_t now_us)
{
    const int64_t kWindowUs = 4000000;
    s_ocr_recent.erase(
        std::remove_if(s_ocr_recent.begin(), s_ocr_recent.end(),
                       [now_us, kWindowUs](const OcrCandidate &c) {
                           return (now_us - c.ts_us) > kWindowUs;
                       }),
        s_ocr_recent.end());
}

static bool ocr_pick_stable(std::string &out_word, float &out_score)
{
    if (s_ocr_recent.empty()) {
        return false;
    }

    int best_count = 0;
    float best_score_sum = 0.0f;
    std::string best_word;

    for (size_t i = 0; i < s_ocr_recent.size(); ++i) {
        int count = 1;
        float score_sum = s_ocr_recent[i].score;
        for (size_t j = i + 1; j < s_ocr_recent.size(); ++j) {
            if (s_ocr_recent[j].word == s_ocr_recent[i].word) {
                count++;
                score_sum += s_ocr_recent[j].score;
            }
        }

        if (count > best_count ||
            (count == best_count && score_sum > best_score_sum)) {
            best_count = count;
            best_score_sum = score_sum;
            best_word = s_ocr_recent[i].word;
        }
    }

    if (best_word.empty()) {
        return false;
    }

    float avg_score = best_score_sum / static_cast<float>(best_count);

    // Balanced gate: allow repeated words to pass while still blocking one-off noise.
    if (best_count >= 2 || avg_score >= 0.86f) {
        out_word = best_word;
        out_score = avg_score;
        return true;
    }
    return false;
}

static bool valid_box(const std::vector<int> &box)
{
    return box.size() >= 4 && std::any_of(box.begin(), box.end(), [](int v) { return v != 0; });
}

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float sigmoidf(float x)
{
    if (x >= 0.0f) {
        float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    float z = std::exp(x);
    return z / (1.0f + z);
}

static float iou_xyxy(const ModelDet &a, const ModelDet &b)
{
    float inter_x0 = std::max(a.x0, b.x0);
    float inter_y0 = std::max(a.y0, b.y0);
    float inter_x1 = std::min(a.x1, b.x1);
    float inter_y1 = std::min(a.y1, b.y1);
    float iw = std::max(0.0f, inter_x1 - inter_x0);
    float ih = std::max(0.0f, inter_y1 - inter_y0);
    float inter = iw * ih;
    float area_a = std::max(0.0f, a.x1 - a.x0) * std::max(0.0f, a.y1 - a.y0);
    float area_b = std::max(0.0f, b.x1 - b.x0) * std::max(0.0f, b.y1 - b.y0);
    float denom = area_a + area_b - inter;
    if (denom <= 1e-6f) {
        return 0.0f;
    }
    return inter / denom;
}

static bool init_digits_model_once()
{
    if (s_digits_model_ready) {
        return true;
    }
    if (s_digits_model) {
        delete s_digits_model;
        s_digits_model = nullptr;
    }

    s_digits_model = new dl::Model((const char *)digits_ocr_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (!s_digits_model) {
        ESP_LOGE(TAG, "digits model alloc failed");
        return false;
    }

    auto &ins = s_digits_model->get_inputs();
    auto &outs = s_digits_model->get_outputs();
    if (ins.empty() || outs.empty()) {
        ESP_LOGE(TAG, "digits model io missing");
        delete s_digits_model;
        s_digits_model = nullptr;
        return false;
    }

    dl::TensorBase *in = ins.begin()->second;
    if (!in || in->shape.size() != 4 || in->shape[0] != 1 || in->shape[1] != 640 || in->shape[2] != 640 || in->shape[3] != 3) {
        ESP_LOGE(TAG, "digits model input shape unsupported");
        delete s_digits_model;
        s_digits_model = nullptr;
        return false;
    }

    s_digits_model_ready = true;
    ESP_LOGI(TAG, "ESP-DL digits model ready");
    return true;
}

static void decode_scale_nhwc(const dl::TensorBase *box_t,
                              const dl::TensorBase *score_t,
                              int stride,
                              float conf_thres,
                              std::vector<ModelDet> &out)
{
    if (!box_t || !score_t) {
        return;
    }
    if (box_t->shape.size() != 4 || score_t->shape.size() != 4) {
        return;
    }

    const int h = box_t->shape[1];
    const int w = box_t->shape[2];
    const int bc = box_t->shape[3];
    const int sc = score_t->shape[3];
    if (bc != 64 || sc != 10 || score_t->shape[1] != h || score_t->shape[2] != w) {
        return;
    }

    const int8_t *box = (const int8_t *)box_t->data;
    const int8_t *score = (const int8_t *)score_t->data;
    const float box_scale = DL_SCALE(box_t->exponent);
    const float score_scale = DL_SCALE(score_t->exponent);

    for (int yi = 0; yi < h; ++yi) {
        for (int xi = 0; xi < w; ++xi) {
            const int sidx = (yi * w + xi) * sc;
            int best_cls = 0;
            float best_logit = -1e9f;
            for (int c = 0; c < sc; ++c) {
                float v = dl::dequantize(score[sidx + c], score_scale);
                if (v > best_logit) {
                    best_logit = v;
                    best_cls = c;
                }
            }

            float conf = sigmoidf(best_logit);
            if (conf < conf_thres) {
                continue;
            }

            const int bidx = (yi * w + xi) * bc;
            float dist[4] = {0};
            for (int side = 0; side < 4; ++side) {
                float maxv = -1e9f;
                float logits[16];
                for (int k = 0; k < 16; ++k) {
                    logits[k] = dl::dequantize(box[bidx + side * 16 + k], box_scale);
                    if (logits[k] > maxv) {
                        maxv = logits[k];
                    }
                }
                float sum = 0.0f;
                float expv[16];
                for (int k = 0; k < 16; ++k) {
                    expv[k] = std::exp(logits[k] - maxv);
                    sum += expv[k];
                }
                float expected = 0.0f;
                float inv = (sum > 1e-6f) ? (1.0f / sum) : 0.0f;
                for (int k = 0; k < 16; ++k) {
                    expected += (float)k * (expv[k] * inv);
                }
                dist[side] = expected * (float)stride;
            }

            float cx = ((float)xi + 0.5f) * (float)stride;
            float cy = ((float)yi + 0.5f) * (float)stride;
            ModelDet d;
            d.cls = best_cls;
            d.score = conf;
            d.x0 = clampf(cx - dist[0], 0.0f, 639.0f);
            d.y0 = clampf(cy - dist[1], 0.0f, 639.0f);
            d.x1 = clampf(cx + dist[2], 0.0f, 639.0f);
            d.y1 = clampf(cy + dist[3], 0.0f, 639.0f);
            if ((d.x1 - d.x0) >= 3.0f && (d.y1 - d.y0) >= 3.0f) {
                out.push_back(d);
            }
        }
    }
}

static void nms(std::vector<ModelDet> &dets, float iou_thres)
{
    std::sort(dets.begin(), dets.end(), [](const ModelDet &a, const ModelDet &b) { return a.score > b.score; });
    std::vector<ModelDet> kept;
    kept.reserve(dets.size());
    for (const auto &d : dets) {
        bool sup = false;
        for (const auto &k : kept) {
            if (iou_xyxy(d, k) > iou_thres) {
                sup = true;
                break;
            }
        }
        if (!sup) {
            kept.push_back(d);
        }
        if (kept.size() >= 16) {
            break;
        }
    }
    dets.swap(kept);
}

static bool run_digits_model(const uint16_t *frame,
                             int width,
                             int height,
                             int roi_x0,
                             int roi_x1,
                             int roi_y0,
                             int roi_y1,
                             std::vector<ocr_digit_prediction_t> &digits,
                             std::string &word,
                             float &score)
{
    digits.clear();
    word.clear();
    score = 0.0f;

    if (!s_digits_model_ready && !init_digits_model_once()) {
        return false;
    }

    dl::TensorBase *in = s_digits_model->get_input();
    if (!in) {
        return false;
    }

    const int in_h = in->shape[1];
    const int in_w = in->shape[2];
    const int roi_w = std::max(1, roi_x1 - roi_x0);
    const int roi_h = std::max(1, roi_y1 - roi_y0);

    std::vector<float> input_nhwc((size_t)in_h * (size_t)in_w * 3U, 0.0f);
    for (int y = 0; y < in_h; ++y) {
        int sy = roi_y0 + (y * roi_h) / in_h;
        sy = std::max(0, std::min(height - 1, sy));
        for (int x = 0; x < in_w; ++x) {
            int sx = roi_x0 + (x * roi_w) / in_w;
            sx = std::max(0, std::min(width - 1, sx));
            uint16_t px = frame[sy * width + sx];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            float r = ((float)r5 * 255.0f / 31.0f) / 255.0f;
            float g = ((float)g6 * 255.0f / 63.0f) / 255.0f;
            float b = ((float)b5 * 255.0f / 31.0f) / 255.0f;
            size_t idx = ((size_t)y * (size_t)in_w + (size_t)x) * 3U;
            input_nhwc[idx + 0] = r;
            input_nhwc[idx + 1] = g;
            input_nhwc[idx + 2] = b;
        }
    }

    dl::TensorBase in_f({1, in_h, in_w, 3}, input_nhwc.data(), 0, dl::DATA_TYPE_FLOAT, false);
    if (!in->assign(&in_f)) {
        return false;
    }

    s_digits_model->run();
    auto &outs = s_digits_model->get_outputs();
    auto it_box0 = outs.find("box0");
    auto it_box1 = outs.find("box1");
    auto it_box2 = outs.find("box2");
    auto it_score0 = outs.find("score0");
    auto it_score1 = outs.find("score1");
    auto it_score2 = outs.find("score2");
    if (it_box0 == outs.end() || it_box1 == outs.end() || it_box2 == outs.end() ||
        it_score0 == outs.end() || it_score1 == outs.end() || it_score2 == outs.end()) {
        return false;
    }

    std::vector<ModelDet> dets;
    dets.reserve(128);
    decode_scale_nhwc(it_box0->second, it_score0->second, 8, 0.45f, dets);
    decode_scale_nhwc(it_box1->second, it_score1->second, 16, 0.45f, dets);
    decode_scale_nhwc(it_box2->second, it_score2->second, 32, 0.45f, dets);
    if (dets.empty()) {
        return false;
    }

    nms(dets, 0.45f);
    std::sort(dets.begin(), dets.end(), [](const ModelDet &a, const ModelDet &b) { return a.x0 < b.x0; });

    float score_sum = 0.0f;
    for (const auto &d : dets) {
        ocr_digit_prediction_t od;
        od.digit = (char)('0' + std::max(0, std::min(9, d.cls)));
        od.confidence = d.score;

        float fx0 = (d.x0 * (float)roi_w / 640.0f) + (float)roi_x0;
        float fy0 = (d.y0 * (float)roi_h / 640.0f) + (float)roi_y0;
        float fx1 = (d.x1 * (float)roi_w / 640.0f) + (float)roi_x0;
        float fy1 = (d.y1 * (float)roi_h / 640.0f) + (float)roi_y0;

        od.x0 = (int)clampf(fx0, 0.0f, (float)(width - 1));
        od.y0 = (int)clampf(fy0, 0.0f, (float)(height - 1));
        od.x1 = (int)clampf(fx1, 0.0f, (float)(width - 1));
        od.y1 = (int)clampf(fy1, 0.0f, (float)(height - 1));
        if (od.x1 > od.x0 && od.y1 > od.y0) {
            digits.push_back(od);
            word.push_back(od.digit);
            score_sum += od.confidence;
        }
    }

    if (digits.empty()) {
        return false;
    }

    score = score_sum / (float)digits.size();
    return true;
}

esp_err_t app_pushlog_ai_init(void)
{
    if (!get_humanface_detect()) {
        ESP_LOGE(TAG, "human face detector init failed");
        return ESP_FAIL;
    }
    if (!get_pedestrian_detect()) {
        ESP_LOGE(TAG, "pedestrian detector init failed");
        return ESP_FAIL;
    }

    if (kEnableLocalDigitsModel) {
        init_digits_model_once();
    } else {
        ESP_LOGW(TAG, "local digits model disabled; using host OCR bridge for predictions");
    }

    ESP_LOGI(TAG, "AI overlay initialized (face/pedestrian + OCR state)");
    return ESP_OK;
}

bool app_pushlog_ai_get_latest_ocr(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }

    out[0] = '\0';
    if (s_ocr_last_word.empty()) {
        return false;
    }

    int64_t now = esp_timer_get_time();
    if ((now - s_ocr_last_update_us) > 5000000) {
        return false;
    }

    snprintf(out, out_len, "%s", s_ocr_last_word.c_str());
    return true;
}

void app_pushlog_ai_overlay(uint16_t *frame, int width, int height, bool face_mode)
{
    if (!frame || width <= 0 || height <= 0) {
        return;
    }

    uint32_t boxes = 0;

    if (face_mode) {
        s_face_frame_count++;
        std::list<dl::detect::result_t> results = app_humanface_detect(frame, width, height);
        for (const auto &res : results) {
            const auto &box = res.box;
            if (!valid_box(box)) {
                continue;
            }
            boxes++;
            draw_rectangle_rgb(frame, width, height,
                               box[0], box[1], box[2], box[3],
                               0, 0, 0, 255, 0, 3, false);

            if (res.keypoint.size() >= 10 &&
                std::any_of(res.keypoint.begin(), res.keypoint.end(), [](int v) { return v != 0; })) {
                draw_green_points(frame, res.keypoint, false);
            }
        }

        if ((s_face_frame_count % 30) == 0) {
            ESP_LOGI(TAG, "[ai_detect] mode=face boxes=%u", (unsigned)boxes);
        }
    } else {
        s_ped_frame_count++;
        std::list<dl::detect::result_t> results = app_pedestrian_detect(frame, width, height);
        for (const auto &res : results) {
            const auto &box = res.box;
            if (!valid_box(box)) {
                continue;
            }
            boxes++;
            draw_rectangle_rgb(frame, width, height,
                               box[0], box[1], box[2], box[3],
                               0, 0, 255, 255, 0, 3, false);
        }

        if ((s_ped_frame_count % 30) == 0) {
            ESP_LOGI(TAG, "[ai_detect] mode=pedestrian boxes=%u", (unsigned)boxes);
        }
    }

    // Lightweight OCR runs less frequently to keep preview smooth.
    s_ocr_frame_count++;
    if ((s_ocr_frame_count % 20) == 0) {
        std::string raw_word;
        float score = 0.0f;
        int64_t now = esp_timer_get_time();
        ocr_prune_old(now);

        // Refresh per-digit predictions from the central ROI and cache for overlay rendering.
        std::vector<ocr_digit_prediction_t> digits;
        float digits_score = 0.0f;
        int roi_x0 = (width * 28) / 100;
        int roi_x1 = (width * 72) / 100;
        int roi_y0 = (height * 32) / 100;
        int roi_y1 = (height * 68) / 100;
        bool used_model = run_digits_model(frame,
                                           width,
                                           height,
                                           roi_x0,
                                           roi_x1,
                                           roi_y0,
                                           roi_y1,
                                           digits,
                                           raw_word,
                                           score);

        if (used_model) {
            s_ocr_digit_boxes = std::move(digits);
        } else if (app_simple_ocr_extract_digits_roi(frame,
                                                     width,
                                                     height,
                                                     roi_x0,
                                                     roi_x1,
                                                     roi_y0,
                                                     roi_y1,
                                                     digits,
                                                     &digits_score)) {
            s_ocr_digit_boxes = std::move(digits);
        } else {
            s_ocr_digit_boxes.clear();
        }

        if (used_model || app_simple_ocr_extract_word(frame, width, height, raw_word, &score)) {
            std::string word = ocr_sanitize_word(raw_word);
            if (score >= 0.50f && ocr_word_clean(word)) {
                s_ocr_recent.push_back({word, score, now});
                if (s_ocr_recent.size() > 12) {
                    s_ocr_recent.erase(s_ocr_recent.begin());
                }

                std::string stable_word;
                float stable_score = 0.0f;
                if (ocr_pick_stable(stable_word, stable_score)) {
                    bool changed = (stable_word != s_ocr_last_word);
                    s_ocr_last_word = stable_word;
                    s_ocr_last_update_us = now;

                    if (changed || (now - s_ocr_last_log_us) > 5000000) {
                        ESP_LOGI(TAG, "[ocr] word=%s score=%.2f", stable_word.c_str(), stable_score);
                        s_ocr_last_log_us = now;
                    }
                }
            } else if ((s_ocr_frame_count % 120) == 0) {
                ESP_LOGI(TAG, "[ocr_debug] raw=%s score=%.2f", raw_word.c_str(), score);
            }
        }
    }

    for (const auto &d : s_ocr_digit_boxes) {
        draw_rectangle_rgb(frame,
                           width,
                           height,
                           d.x0,
                           d.y0,
                           d.x1,
                           d.y1,
                           0,
                           0,
                           255,
                           180,
                           0,
                           2,
                           false);
    }

}
