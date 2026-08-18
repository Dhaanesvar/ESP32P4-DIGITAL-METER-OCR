#include <algorithm>
#include <array>
#include <cctype>
#include <list>
#include <vector>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"

#include "app_humanface_detect.h"
#include "app_pedestrian_detect.h"
#include "app_drawing_utils.h"
#include "app_pushlog_ai_overlay.h"
#include "app_simple_ocr.hpp"

static const char *TAG = "pushlog_ai";

static uint32_t s_face_frame_count = 0;
static uint32_t s_ped_frame_count = 0;
static uint32_t s_ocr_frame_count = 0;
static int64_t s_ocr_last_log_us = 0;
static std::string s_ocr_last_word;

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

    ESP_LOGI(TAG, "AI overlay initialized (face/pedestrian)");
    return ESP_OK;
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

        if (app_simple_ocr_extract_word(frame, width, height, raw_word, &score)) {
            std::string word = ocr_sanitize_word(raw_word);
            if (score >= 0.50f && ocr_word_clean(word)) {
                s_ocr_recent.push_back({word, score, now});
                if (s_ocr_recent.size() > 12) {
                    s_ocr_recent.erase(s_ocr_recent.begin());
                }

                std::string stable_word;
                float stable_score = 0.0f;
                if (ocr_pick_stable(stable_word, stable_score)) {
                    if (stable_word != s_ocr_last_word || (now - s_ocr_last_log_us) > 5000000) {
                        ESP_LOGI(TAG, "[ocr] word=%s score=%.2f", stable_word.c_str(), stable_score);
                        s_ocr_last_word = stable_word;
                        s_ocr_last_log_us = now;
                    }
                }
            } else if ((s_ocr_frame_count % 120) == 0) {
                ESP_LOGI(TAG, "[ocr_debug] raw=%s score=%.2f", raw_word.c_str(), score);
            }
        }
    }
}
