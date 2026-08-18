#include "app_simple_ocr.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app_simple_ocr_c.h"

namespace {

struct Glyph {
    char ch;
    std::array<uint8_t, 7> rows;
};

// 5x7 uppercase glyphs. Each row uses lower 5 bits.
static const std::array<Glyph, 36> kGlyphs = {{
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}},
    {'A', {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
}};

static inline uint8_t rgb565_to_luma(uint16_t px)
{
    uint8_t r = static_cast<uint8_t>((px >> 11) & 0x1F);
    uint8_t g = static_cast<uint8_t>((px >> 5) & 0x3F);
    uint8_t b = static_cast<uint8_t>(px & 0x1F);

    uint16_t r8 = static_cast<uint16_t>((r << 3) | (r >> 2));
    uint16_t g8 = static_cast<uint16_t>((g << 2) | (g >> 4));
    uint16_t b8 = static_cast<uint16_t>((b << 3) | (b >> 2));

    return static_cast<uint8_t>((r8 * 76 + g8 * 150 + b8 * 29) >> 8);
}

static uint8_t otsu_threshold(const std::vector<uint8_t> &gray)
{
    std::array<uint32_t, 256> hist = {0};
    for (uint8_t v : gray) {
        hist[v]++;
    }

    const uint32_t total = static_cast<uint32_t>(gray.size());
    if (total == 0) {
        return 127;
    }

    uint64_t sum_all = 0;
    for (int i = 0; i < 256; ++i) {
        sum_all += static_cast<uint64_t>(i) * hist[static_cast<size_t>(i)];
    }

    uint32_t w_b = 0;
    uint64_t sum_b = 0;
    double best_var = -1.0;
    uint8_t best_t = 127;

    for (int t = 0; t < 256; ++t) {
        w_b += hist[static_cast<size_t>(t)];
        if (w_b == 0) {
            continue;
        }
        uint32_t w_f = total - w_b;
        if (w_f == 0) {
            break;
        }

        sum_b += static_cast<uint64_t>(t) * hist[static_cast<size_t>(t)];
        double m_b = static_cast<double>(sum_b) / static_cast<double>(w_b);
        double m_f = static_cast<double>(sum_all - sum_b) / static_cast<double>(w_f);
        double d = m_b - m_f;
        double var_between = static_cast<double>(w_b) * static_cast<double>(w_f) * d * d;
        if (var_between > best_var) {
            best_var = var_between;
            best_t = static_cast<uint8_t>(t);
        }
    }

    return best_t;
}

static char match_glyph(const std::array<uint8_t, 7> &rows, float &score, float &margin)
{
    char best_char = '?';
    int best_dist = 35;
    int second_best_dist = 35;

    for (const auto &g : kGlyphs) {
        int dist = 0;
        for (int y = 0; y < 7; ++y) {
            uint8_t diff = static_cast<uint8_t>((rows[y] ^ g.rows[y]) & 0x1F);
            dist += __builtin_popcount(diff);
        }
        if (dist < best_dist) {
            second_best_dist = best_dist;
            best_dist = dist;
            best_char = g.ch;
        } else if (dist < second_best_dist) {
            second_best_dist = dist;
        }
    }

    score = 1.0f - (static_cast<float>(best_dist) / 35.0f);
    margin = static_cast<float>(second_best_dist - best_dist) / 35.0f;
    return best_char;
}

static bool decode_word_from_bin(const std::vector<uint8_t> &bin,
                                 int w,
                                 int h,
                                 std::string &word,
                                 float *confidence)
{
    word.clear();
    if (confidence) {
        *confidence = 0.0f;
    }

    // Keep only the densest horizontal text band to avoid background clutter.
    std::vector<int> row_sum(h, 0);
    for (int y = 0; y < h; ++y) {
        int r = 0;
        for (int x = 0; x < w; ++x) {
            r += bin[static_cast<size_t>(y) * w + x];
        }
        row_sum[y] = r;
    }
    int best_y = 0;
    int best_r = -1;
    for (int y = 0; y < h; ++y) {
        if (row_sum[y] > best_r) {
            best_r = row_sum[y];
            best_y = y;
        }
    }
    if (best_r < w / 14) {
        return false;
    }

    int y0_band = std::max(0, best_y - h / 4);
    int y1_band = std::min(h - 1, best_y + h / 4);

    const int row_gate = std::max(2, best_r / 8);
    int y0_refine = best_y;
    int y1_refine = best_y;
    while (y0_refine > 0 && row_sum[y0_refine - 1] >= row_gate) {
        y0_refine--;
    }
    while (y1_refine < h - 1 && row_sum[y1_refine + 1] >= row_gate) {
        y1_refine++;
    }
    y0_band = std::min(y0_band, y0_refine);
    y1_band = std::max(y1_band, y1_refine);
    if ((y1_band - y0_band + 1) < 9) {
        return false;
    }

    std::vector<int> col_sum(w, 0);
    for (int x = 0; x < w; ++x) {
        int c = 0;
        for (int y = y0_band; y <= y1_band; ++y) {
            c += bin[static_cast<size_t>(y) * w + x];
        }
        col_sum[x] = c;
    }

    const int band_h = y1_band - y0_band + 1;
    const int min_col = std::max(2, band_h / 10);
    std::vector<std::pair<int, int>> spans;
    int start = -1;
    for (int x = 0; x < w; ++x) {
        if (col_sum[x] >= min_col) {
            if (start < 0) {
                start = x;
            }
        } else if (start >= 0) {
            if (x - start >= 3) {
                spans.emplace_back(start, x - 1);
            }
            start = -1;
        }
    }
    if (start >= 0 && (w - start) >= 3) {
        spans.emplace_back(start, w - 1);
    }

    if (spans.empty() || spans.size() > 14) {
        return false;
    }

    // Split overly wide spans using local minima in column density.
    std::vector<std::pair<int, int>> refined_spans;
    refined_spans.reserve(spans.size() * 2);
    for (const auto &span : spans) {
        const int sx0 = span.first;
        const int sx1 = span.second;
        const int sw = sx1 - sx0 + 1;

        if (sw <= 22) {
            refined_spans.push_back(span);
            continue;
        }

        int last = sx0;
        for (int x = sx0 + 3; x <= sx1 - 3; ++x) {
            int c = col_sum[x];
            int l = col_sum[x - 1];
            int r = col_sum[x + 1];
            bool valley = (c <= (min_col / 2)) && (c <= l) && (c <= r);
            if (valley && (x - last) >= 3) {
                refined_spans.emplace_back(last, x - 1);
                last = x + 1;
            }
        }

        if ((sx1 - last + 1) >= 3) {
            refined_spans.emplace_back(last, sx1);
        }
    }
    spans.swap(refined_spans);

    if (spans.empty() || spans.size() > 14) {
        return false;
    }

    std::string out;
    float score_sum = 0.0f;
    int score_count = 0;

    for (const auto &span : spans) {
        int x0 = span.first;
        int x1 = span.second;
        int cw = x1 - x0 + 1;
        if (cw < 3 || cw > 36) {
            continue;
        }

        int y0 = y1_band;
        int y1 = -1;
        for (int y = y0_band; y <= y1_band; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (bin[static_cast<size_t>(y) * w + x]) {
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
        }
        if (y1 <= y0) {
            continue;
        }

        std::array<uint8_t, 7> rows = {0, 0, 0, 0, 0, 0, 0};
        const int ch = y1 - y0 + 1;
        for (int gy = 0; gy < 7; ++gy) {
            int sy0 = y0 + (gy * ch) / 7;
            int sy1 = y0 + ((gy + 1) * ch) / 7;
            if (sy1 <= sy0) {
                sy1 = sy0 + 1;
            }
            uint8_t row_bits = 0;
            for (int gx = 0; gx < 5; ++gx) {
                int sx0 = x0 + (gx * cw) / 5;
                int sx1 = x0 + ((gx + 1) * cw) / 5;
                if (sx1 <= sx0) {
                    sx1 = sx0 + 1;
                }

                int on = 0;
                int total = 0;
                for (int sy = sy0; sy < sy1 && sy < h; ++sy) {
                    for (int sx = sx0; sx < sx1 && sx <= x1; ++sx) {
                        on += bin[static_cast<size_t>(sy) * w + sx];
                        total++;
                    }
                }

                if (total > 0 && (on * 2) >= total) {
                    row_bits |= static_cast<uint8_t>(1u << (4 - gx));
                }
            }
            rows[gy] = row_bits;
        }

        float score = 0.0f;
        float margin = 0.0f;
        char c = match_glyph(rows, score, margin);
        if (score < 0.50f || margin < 0.01f) {
            c = '?';
        }

        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        score_sum += score;
        score_count++;
    }

    if (out.empty()) {
        return false;
    }

    while (!out.empty() && out.front() == '?') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '?') {
        out.pop_back();
    }

    if (out.size() < 1 || out.size() > 14) {
        return false;
    }

    int unknown = 0;
    int alnum = 0;
    for (char c : out) {
        if (c == '?') {
            unknown++;
        }
        if (std::isalnum(static_cast<unsigned char>(c))) {
            alnum++;
        }
    }
    if (unknown > static_cast<int>(out.size() / 2) || alnum < 1) {
        return false;
    }

    float avg_score = score_count > 0 ? (score_sum / static_cast<float>(score_count)) : 0.0f;
    if (avg_score < 0.45f) {
        return false;
    }

    word = out;
    if (confidence) {
        *confidence = avg_score;
    }
    return true;
}

struct SevenSegDigit;
static void morph_open_vertical(std::vector<uint8_t> &bin, int w, int h);
static void morph_close_square5(std::vector<uint8_t> &bin, int w, int h);
static bool decode_digits_7segment_from_bin(const std::vector<uint8_t> &bin,
                                            int w,
                                            int h,
                                            std::string &word,
                                            float *confidence);

static bool extract_from_roi(const uint16_t *frame,
                             int width,
                             int height,
                             int roi_x0,
                             int roi_x1,
                             int roi_y0,
                             int roi_y1,
                             std::string &word,
                             float *confidence)
{
    const int roi_w = roi_x1 - roi_x0;
    const int roi_h = roi_y1 - roi_y0;
    if (roi_w < 40 || roi_h < 20) {
        return false;
    }

    const int w = 160;
    const int h = 64;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h);

    for (int y = 0; y < h; ++y) {
        int src_y = roi_y0 + (y * roi_h) / h;
        for (int x = 0; x < w; ++x) {
            int src_x = roi_x0 + (x * roi_w) / w;
            uint8_t v = rgb565_to_luma(frame[src_y * width + src_x]);
            gray[static_cast<size_t>(y) * w + x] = v;
        }
    }

    uint8_t threshold = otsu_threshold(gray);
    int below_t = 0;
    for (int i = 0; i < w * h; ++i) {
        if (gray[i] < threshold) {
            below_t++;
        }
    }
    bool dark_text_guess = below_t < (w * h) / 2;

    auto pick_better_numeric = [](const std::string &cur_word,
                                  float cur_score,
                                  const std::string &cand_word,
                                  float cand_score) -> bool {
        if (cand_word.empty()) {
            return false;
        }
        if (cur_word.empty()) {
            return true;
        }

        if (cand_word.size() >= 4 && cur_word.size() < 4) {
            return true;
        }
        if ((cand_word.size() >= 4) == (cur_word.size() >= 4)) {
            if (cand_word.size() > cur_word.size()) {
                return true;
            }
            if (cand_word.size() == cur_word.size() && cand_score > cur_score) {
                return true;
            }
        }
        return false;
    };

    auto try_polarity = [&](bool dark_text, std::string &out_word, float &out_score) -> bool {
        std::vector<uint8_t> bin(static_cast<size_t>(w) * h);
        for (int i = 0; i < w * h; ++i) {
            bool bit = dark_text ? (gray[i] < threshold) : (gray[i] > threshold);
            bin[static_cast<size_t>(i)] = bit ? 1 : 0;
        }

        // Remove isolated noise pixels that often create fake characters.
        std::vector<uint8_t> denoise = bin;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int neighbors = 0;
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx) {
                        neighbors += bin[static_cast<size_t>(y + ky) * w + (x + kx)];
                    }
                }
                uint8_t cur = bin[static_cast<size_t>(y) * w + x];
                if (cur && neighbors <= 2) {
                    denoise[static_cast<size_t>(y) * w + x] = 0;
                } else if (!cur && neighbors >= 7) {
                    denoise[static_cast<size_t>(y) * w + x] = 1;
                }
            }
        }
        bin.swap(denoise);

        // Try multiple morphology variants and keep the strongest numeric sequence.
        std::string seg_best_word;
        float seg_best_score = 0.0f;

        auto try_seg_variant = [&](const std::vector<uint8_t> &candidate_bin) {
            std::string seg_word;
            float seg_score = 0.0f;
            if (decode_digits_7segment_from_bin(candidate_bin, w, h, seg_word, &seg_score) && !seg_word.empty()) {
                if (pick_better_numeric(seg_best_word, seg_best_score, seg_word, seg_score)) {
                    seg_best_word = seg_word;
                    seg_best_score = seg_score;
                }
            }
        };

        try_seg_variant(bin);

        std::vector<uint8_t> seg_open_close = bin;
        morph_open_vertical(seg_open_close, w, h);
        morph_close_square5(seg_open_close, w, h);
        try_seg_variant(seg_open_close);

        std::vector<uint8_t> seg_close_only = bin;
        morph_close_square5(seg_close_only, w, h);
        try_seg_variant(seg_close_only);

        if (!seg_best_word.empty()) {
            out_word = seg_best_word;
            out_score = std::max(seg_best_score, 0.55f);
            return true;
        }

        return decode_word_from_bin(bin, w, h, out_word, &out_score);
    };

    std::string best_word;
    float best_score = 0.0f;

    std::string w0;
    float s0 = 0.0f;
    if (try_polarity(dark_text_guess, w0, s0)) {
        best_word = w0;
        best_score = s0;
    }

    std::string w1;
    float s1 = 0.0f;
    if (try_polarity(!dark_text_guess, w1, s1)) {
        if (best_word.empty() || s1 > best_score) {
            best_word = w1;
            best_score = s1;
        }
    }

    if (best_word.empty()) {
        return false;
    }

    word = best_word;
    if (confidence) {
        *confidence = best_score;
    }
    return true;
}

static float digit_bias_score(const std::string &s)
{
    if (s.empty()) {
        return 0.0f;
    }
    int digits = 0;
    int alnum = 0;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isdigit(uc)) {
            digits++;
        }
        if (std::isalnum(uc)) {
            alnum++;
        }
    }
    if (alnum == 0) {
        return 0.0f;
    }
    return static_cast<float>(digits) / static_cast<float>(alnum);
}

struct SevenSegDigit {
    int x0;
    int x1;
    int y0;
    int y1;
};

static void morph_open_vertical(std::vector<uint8_t> &bin, int w, int h)
{
    std::vector<uint8_t> eroded(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t> opened(static_cast<size_t>(w) * h, 0);

    for (int y = 2; y < h - 2; ++y) {
        for (int x = 0; x < w; ++x) {
            bool ok = true;
            for (int ky = -2; ky <= 2; ++ky) {
                if (!bin[static_cast<size_t>(y + ky) * w + x]) {
                    ok = false;
                    break;
                }
            }
            eroded[static_cast<size_t>(y) * w + x] = ok ? 1 : 0;
        }
    }

    for (int y = 2; y < h - 2; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = 0;
            for (int ky = -2; ky <= 2; ++ky) {
                if (eroded[static_cast<size_t>(y + ky) * w + x]) {
                    v = 1;
                    break;
                }
            }
            opened[static_cast<size_t>(y) * w + x] = v;
        }
    }

    bin.swap(opened);
}

static void morph_close_square5(std::vector<uint8_t> &bin, int w, int h)
{
    std::vector<uint8_t> dilated(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t> closed(static_cast<size_t>(w) * h, 0);

    for (int y = 2; y < h - 2; ++y) {
        for (int x = 2; x < w - 2; ++x) {
            uint8_t v = 0;
            for (int ky = -2; ky <= 2 && !v; ++ky) {
                for (int kx = -2; kx <= 2; ++kx) {
                    if (bin[static_cast<size_t>(y + ky) * w + (x + kx)]) {
                        v = 1;
                        break;
                    }
                }
            }
            dilated[static_cast<size_t>(y) * w + x] = v;
        }
    }

    for (int y = 2; y < h - 2; ++y) {
        for (int x = 2; x < w - 2; ++x) {
            bool ok = true;
            for (int ky = -2; ky <= 2 && ok; ++ky) {
                for (int kx = -2; kx <= 2; ++kx) {
                    if (!dilated[static_cast<size_t>(y + ky) * w + (x + kx)]) {
                        ok = false;
                        break;
                    }
                }
            }
            closed[static_cast<size_t>(y) * w + x] = ok ? 1 : 0;
        }
    }

    bin.swap(closed);
}

static bool detect_digit_components(const std::vector<uint8_t> &bin,
                                    int w,
                                    int h,
                                    std::vector<SevenSegDigit> &digits)
{
    digits.clear();

    std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
    std::vector<int> qx;
    std::vector<int> qy;
    qx.reserve(static_cast<size_t>(w) * h / 8);
    qy.reserve(static_cast<size_t>(w) * h / 8);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!bin[idx] || visited[idx]) {
                continue;
            }

            qx.clear();
            qy.clear();
            qx.push_back(x);
            qy.push_back(y);
            visited[idx] = 1;

            int x0 = x;
            int x1 = x;
            int y0 = y;
            int y1 = y;
            int area = 0;

            for (size_t qi = 0; qi < qx.size(); ++qi) {
                int cx = qx[qi];
                int cy = qy[qi];
                area++;
                x0 = std::min(x0, cx);
                x1 = std::max(x1, cx);
                y0 = std::min(y0, cy);
                y1 = std::max(y1, cy);

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
                        continue;
                    }
                    size_t nidx = static_cast<size_t>(ny) * w + nx;
                    if (!bin[nidx] || visited[nidx]) {
                        continue;
                    }
                    visited[nidx] = 1;
                    qx.push_back(nx);
                    qy.push_back(ny);
                }
            }

            int cw = x1 - x0 + 1;
            int ch = y1 - y0 + 1;
            if (cw < 4 || ch < 14 || area < 18) {
                continue;
            }

            float aspect = static_cast<float>(cw) / static_cast<float>(ch);
            if (aspect < 0.08f || aspect > 0.95f) {
                continue;
            }
            if (ch < (h / 2)) {
                continue;
            }

            digits.push_back({x0, x1, y0, y1});
        }
    }

    if (digits.empty()) {
        return false;
    }

    std::sort(digits.begin(), digits.end(), [](const SevenSegDigit &a, const SevenSegDigit &b) {
        return a.x0 < b.x0;
    });

    return !digits.empty();
}

static bool decode_7segment_digit(const std::vector<uint8_t> &bin,
                                  int w,
                                  int h,
                                  const SevenSegDigit &d,
                                  char &out_digit,
                                  float &out_conf)
{
    const int dw = d.x1 - d.x0 + 1;
    const int dh = d.y1 - d.y0 + 1;
    if (dw < 4 || dh < 14) {
        return false;
    }

    // Very thin components are almost always a 1 on seven-segment displays.
    if (dw <= 6) {
        out_digit = '1';
        out_conf = 0.62f;
        return true;
    }

    const int segW = std::max(1, static_cast<int>(dw * 0.25f));
    const int segH = std::max(1, static_cast<int>(dh * 0.15f));
    const int segHC = std::max(1, static_cast<int>(dh * 0.05f));

    struct Seg {
        int xA;
        int yA;
        int xB;
        int yB;
    };

    std::array<Seg, 7> segs = {{
        {d.x0, d.y0, d.x1 + 1, d.y0 + segH},
        {d.x0, d.y0, d.x0 + segW, d.y0 + (dh / 2)},
        {d.x1 - segW + 1, d.y0, d.x1 + 1, d.y0 + (dh / 2)},
        {d.x0, d.y0 + (dh / 2) - segHC, d.x1 + 1, d.y0 + (dh / 2) + segHC},
        {d.x0, d.y0 + (dh / 2), d.x0 + segW, d.y1 + 1},
        {d.x1 - segW + 1, d.y0 + (dh / 2), d.x1 + 1, d.y1 + 1},
        {d.x0, d.y1 - segH + 1, d.x1 + 1, d.y1 + 1},
    }};

    std::array<int, 7> on = {0, 0, 0, 0, 0, 0, 0};
    float conf_sum = 0.0f;

    for (size_t i = 0; i < segs.size(); ++i) {
        int xA = std::max(0, std::min(segs[i].xA, w - 1));
        int xB = std::max(xA + 1, std::min(segs[i].xB, w));
        int yA = std::max(0, std::min(segs[i].yA, h - 1));
        int yB = std::max(yA + 1, std::min(segs[i].yB, h));

        int total = 0;
        int area = (xB - xA) * (yB - yA);
        for (int y = yA; y < yB; ++y) {
            for (int x = xA; x < xB; ++x) {
                total += bin[static_cast<size_t>(y) * w + x];
            }
        }

        float ratio = area > 0 ? (static_cast<float>(total) / static_cast<float>(area)) : 0.0f;
        if (ratio > 0.35f) {
            on[i] = 1;
        }
        conf_sum += std::abs(ratio - 0.35f);
    }

    struct Pattern {
        std::array<int, 7> seg;
        char digit;
    };
    static const std::array<Pattern, 10> lut = {{
        {{{1, 1, 1, 0, 1, 1, 1}}, '0'},
        {{{0, 0, 1, 0, 0, 1, 0}}, '1'},
        {{{1, 0, 1, 1, 1, 0, 1}}, '2'},
        {{{1, 0, 1, 1, 0, 1, 1}}, '3'},
        {{{0, 1, 1, 1, 0, 1, 0}}, '4'},
        {{{1, 1, 0, 1, 0, 1, 1}}, '5'},
        {{{1, 1, 0, 1, 1, 1, 1}}, '6'},
        {{{1, 0, 1, 0, 0, 1, 0}}, '7'},
        {{{1, 1, 1, 1, 1, 1, 1}}, '8'},
        {{{1, 1, 1, 1, 0, 1, 1}}, '9'},
    }};

    int best_dist = 8;
    char best_digit = '?';
    for (const auto &p : lut) {
        int dist = 0;
        for (size_t i = 0; i < on.size(); ++i) {
            if (on[i] != p.seg[i]) {
                dist++;
            }
        }
        if (dist < best_dist) {
            best_dist = dist;
            best_digit = p.digit;
        }
    }

    if (best_dist > 3) {
        return false;
    }

    out_digit = best_digit;
    out_conf = std::max(0.35f, 1.0f - (static_cast<float>(best_dist) / 7.0f) + (conf_sum / 14.0f));
    return true;
}

static bool decode_digits_7segment_from_bin(const std::vector<uint8_t> &bin,
                                            int w,
                                            int h,
                                            std::string &word,
                                            float *confidence)
{
    std::vector<SevenSegDigit> digits;
    if (!detect_digit_components(bin, w, h, digits)) {
        return false;
    }

    std::string out;
    float score_sum = 0.0f;
    int score_count = 0;

    for (const auto &d : digits) {
        char ch = '?';
        float conf = 0.0f;
        if (!decode_7segment_digit(bin, w, h, d, ch, conf)) {
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            out.push_back(ch);
            score_sum += conf;
            score_count++;
        }
    }

    if (out.empty()) {
        return false;
    }

    word = out;
    if (confidence) {
        *confidence = score_count > 0 ? (score_sum / static_cast<float>(score_count)) : 0.0f;
    }
    return true;
}

struct DigitBox {
    int x0;
    int x1;
    int y0;
    int y1;
    int component_count;
    int area_sum;
};

static bool find_digit_box_from_bin(const std::vector<uint8_t> &bin,
                                    int w,
                                    int h,
                                    DigitBox &out_box)
{
    std::vector<int> row_sum(h, 0);
    for (int y = 0; y < h; ++y) {
        int r = 0;
        for (int x = 0; x < w; ++x) {
            r += bin[static_cast<size_t>(y) * w + x];
        }
        row_sum[y] = r;
    }

    int best_y = 0;
    int best_r = -1;
    for (int y = 0; y < h; ++y) {
        if (row_sum[y] > best_r) {
            best_r = row_sum[y];
            best_y = y;
        }
    }
    if (best_r < w / 14) {
        return false;
    }

    int y0_band = std::max(0, best_y - h / 4);
    int y1_band = std::min(h - 1, best_y + h / 4);
    const int row_gate = std::max(2, best_r / 8);
    while (y0_band > 0 && row_sum[y0_band - 1] >= row_gate) {
        y0_band--;
    }
    while (y1_band < h - 1 && row_sum[y1_band + 1] >= row_gate) {
        y1_band++;
    }
    if ((y1_band - y0_band + 1) < 9) {
        return false;
    }

    struct Comp {
        int x0;
        int x1;
        int y0;
        int y1;
        int area;
    };
    std::vector<Comp> comps;

    std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
    std::vector<int> qx;
    std::vector<int> qy;
    qx.reserve(static_cast<size_t>(w) * h / 8);
    qy.reserve(static_cast<size_t>(w) * h / 8);

    for (int y = y0_band; y <= y1_band; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!bin[idx] || visited[idx]) {
                continue;
            }

            qx.clear();
            qy.clear();
            qx.push_back(x);
            qy.push_back(y);
            visited[idx] = 1;

            int x0 = x;
            int x1 = x;
            int yy0 = y;
            int yy1 = y;
            int area = 0;

            for (size_t qi = 0; qi < qx.size(); ++qi) {
                int cx = qx[qi];
                int cy = qy[qi];
                area++;
                x0 = std::min(x0, cx);
                x1 = std::max(x1, cx);
                yy0 = std::min(yy0, cy);
                yy1 = std::max(yy1, cy);

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];
                    if (nx < 0 || nx >= w || ny < y0_band || ny > y1_band) {
                        continue;
                    }
                    size_t nidx = static_cast<size_t>(ny) * w + nx;
                    if (!bin[nidx] || visited[nidx]) {
                        continue;
                    }
                    visited[nidx] = 1;
                    qx.push_back(nx);
                    qy.push_back(ny);
                }
            }

            int cw = x1 - x0 + 1;
            int ch = yy1 - yy0 + 1;
            if (area < 10 || cw < 2 || ch < 6) {
                continue;
            }

            float aspect = static_cast<float>(cw) / static_cast<float>(ch);
            if (aspect < 0.08f || aspect > 1.6f) {
                continue;
            }

            if (ch < (y1_band - y0_band + 1) / 3) {
                continue;
            }

            comps.push_back({x0, x1, yy0, yy1, area});
        }
    }

    if (comps.empty()) {
        return false;
    }

    std::sort(comps.begin(), comps.end(), [](const Comp &a, const Comp &b) {
        return a.x0 < b.x0;
    });

    bool found = false;
    DigitBox best = {0, 0, 0, 0, 0, 0};

    for (size_t i = 0; i < comps.size(); ++i) {
        int cx0 = comps[i].x0;
        int cx1 = comps[i].x1;
        int cy0 = comps[i].y0;
        int cy1 = comps[i].y1;
        int count = 1;
        int area = comps[i].area;

        for (size_t j = i + 1; j < comps.size(); ++j) {
            int gap = comps[j].x0 - cx1;
            if (gap > (w / 12)) {
                break;
            }

            cx0 = std::min(cx0, comps[j].x0);
            cx1 = std::max(cx1, comps[j].x1);
            cy0 = std::min(cy0, comps[j].y0);
            cy1 = std::max(cy1, comps[j].y1);
            count++;
            area += comps[j].area;
        }

        int bw = cx1 - cx0 + 1;
        int bh = cy1 - cy0 + 1;
        if (bw < 12 || bh < 8) {
            continue;
        }

        int score = area + (count * 18);
        int best_score = best.area_sum + (best.component_count * 18);
        if (!found || score > best_score) {
            best.x0 = cx0;
            best.x1 = cx1;
            best.y0 = cy0;
            best.y1 = cy1;
            best.component_count = count;
            best.area_sum = area;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    int pad_x = std::max(1, (best.x1 - best.x0 + 1) / 20);
    int pad_y = std::max(1, (best.y1 - best.y0 + 1) / 10);
    best.x0 = std::max(0, best.x0 - pad_x);
    best.x1 = std::min(w - 1, best.x1 + pad_x);
    best.y0 = std::max(0, best.y0 - pad_y);
    best.y1 = std::min(h - 1, best.y1 + pad_y);

    out_box = best;
    return true;
}

static bool refine_digit_box_from_roi(const uint16_t *frame,
                                      int width,
                                      int height,
                                      int roi_x0,
                                      int roi_x1,
                                      int roi_y0,
                                      int roi_y1,
                                      int *out_x0,
                                      int *out_x1,
                                      int *out_y0,
                                      int *out_y1)
{
    if (!frame || width < 80 || height < 40) {
        return false;
    }

    roi_x0 = std::max(0, std::min(roi_x0, width - 1));
    roi_x1 = std::max(0, std::min(roi_x1, width));
    roi_y0 = std::max(0, std::min(roi_y0, height - 1));
    roi_y1 = std::max(0, std::min(roi_y1, height));

    int roi_w = roi_x1 - roi_x0;
    int roi_h = roi_y1 - roi_y0;
    if (roi_w < 40 || roi_h < 20) {
        return false;
    }

    const int w = 160;
    const int h = 64;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        int src_y = roi_y0 + (y * roi_h) / h;
        for (int x = 0; x < w; ++x) {
            int src_x = roi_x0 + (x * roi_w) / w;
            gray[static_cast<size_t>(y) * w + x] = rgb565_to_luma(frame[src_y * width + src_x]);
        }
    }

    uint8_t threshold = otsu_threshold(gray);
    int below_t = 0;
    for (int i = 0; i < w * h; ++i) {
        if (gray[static_cast<size_t>(i)] < threshold) {
            below_t++;
        }
    }
    bool dark_text_guess = below_t < (w * h) / 2;

    auto build_bin = [&](bool dark_text) {
        std::vector<uint8_t> bin(static_cast<size_t>(w) * h);
        for (int i = 0; i < w * h; ++i) {
            bool bit = dark_text ? (gray[static_cast<size_t>(i)] < threshold)
                                 : (gray[static_cast<size_t>(i)] > threshold);
            bin[static_cast<size_t>(i)] = bit ? 1 : 0;
        }

        std::vector<uint8_t> denoise = bin;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int neighbors = 0;
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx) {
                        neighbors += bin[static_cast<size_t>(y + ky) * w + (x + kx)];
                    }
                }
                uint8_t cur = bin[static_cast<size_t>(y) * w + x];
                if (cur && neighbors <= 2) {
                    denoise[static_cast<size_t>(y) * w + x] = 0;
                } else if (!cur && neighbors >= 7) {
                    denoise[static_cast<size_t>(y) * w + x] = 1;
                }
            }
        }
        return denoise;
    };

    DigitBox best_box = {0, 0, 0, 0, 0, 0};
    bool have_box = false;

    std::vector<uint8_t> bin0 = build_bin(dark_text_guess);
    DigitBox box0 = {0, 0, 0, 0, 0, 0};
    if (find_digit_box_from_bin(bin0, w, h, box0)) {
        best_box = box0;
        have_box = true;
    }

    std::vector<uint8_t> bin1 = build_bin(!dark_text_guess);
    DigitBox box1 = {0, 0, 0, 0, 0, 0};
    if (find_digit_box_from_bin(bin1, w, h, box1)) {
        int score1 = box1.area_sum + (box1.component_count * 18);
        int score0 = best_box.area_sum + (best_box.component_count * 18);
        if (!have_box || score1 > score0) {
            best_box = box1;
            have_box = true;
        }
    }

    if (!have_box) {
        return false;
    }

    int rx0 = roi_x0 + (best_box.x0 * roi_w) / w;
    int rx1 = roi_x0 + ((best_box.x1 + 1) * roi_w) / w;
    int ry0 = roi_y0 + (best_box.y0 * roi_h) / h;
    int ry1 = roi_y0 + ((best_box.y1 + 1) * roi_h) / h;

    rx0 = std::max(0, std::min(rx0, width - 1));
    rx1 = std::max(rx0 + 1, std::min(rx1, width));
    ry0 = std::max(0, std::min(ry0, height - 1));
    ry1 = std::max(ry0 + 1, std::min(ry1, height));

    if ((rx1 - rx0) < 16 || (ry1 - ry0) < 10) {
        return false;
    }

    if (out_x0) {
        *out_x0 = rx0;
    }
    if (out_x1) {
        *out_x1 = rx1;
    }
    if (out_y0) {
        *out_y0 = ry0;
    }
    if (out_y1) {
        *out_y1 = ry1;
    }
    return true;
}

} // namespace

bool app_simple_ocr_extract_word(const uint16_t *frame,
                                 int width,
                                 int height,
                                 std::string &word,
                                 float *confidence)
{
    word.clear();
    if (confidence) {
        *confidence = 0.0f;
    }
    if (!frame || width < 80 || height < 40) {
        return false;
    }

    struct Roi {
        int x0;
        int x1;
        int y0;
        int y1;
    };

    std::array<Roi, 6> rois = {{
        {width / 8, (width * 7) / 8, height / 4, (height * 3) / 4},
        {width / 10, (width * 9) / 10, height / 8, (height * 5) / 8},
        {width / 10, (width * 9) / 10, (height * 3) / 8, (height * 7) / 8},
        {width / 16, (width * 15) / 16, height / 10, (height * 9) / 10},
        {(width * 3) / 16, (width * 13) / 16, (height * 6) / 16, (height * 11) / 16},
        {(width * 2) / 10, (width * 8) / 10, (height * 7) / 16, (height * 12) / 16},
    }};

    std::string best_word;
    float best_score = 0.0f;
    float best_digit_bias = 0.0f;
    for (const auto &roi : rois) {
        std::string candidate;
        float candidate_score = 0.0f;
        if (!extract_from_roi(frame,
                              width,
                              height,
                              roi.x0,
                              roi.x1,
                              roi.y0,
                              roi.y1,
                              candidate,
                              &candidate_score)) {
            continue;
        }

        float candidate_digit_bias = digit_bias_score(candidate);
        if (best_word.empty() ||
            candidate_digit_bias > (best_digit_bias + 0.15f) ||
            (candidate_digit_bias >= (best_digit_bias - 0.05f) &&
             (candidate_score > best_score ||
              (candidate_score == best_score && candidate.size() > best_word.size())))) {
            best_word = std::move(candidate);
            best_score = candidate_score;
            best_digit_bias = candidate_digit_bias;
        }
    }

    if (best_word.empty()) {
        return false;
    }

    word = best_word;
    if (confidence) {
        *confidence = best_score;
    }
    return true;
}

bool app_simple_ocr_extract_word_roi(const uint16_t *frame,
                                     int width,
                                     int height,
                                     int roi_x0,
                                     int roi_x1,
                                     int roi_y0,
                                     int roi_y1,
                                     std::string &word,
                                     float *confidence)
{
    word.clear();
    if (confidence) {
        *confidence = 0.0f;
    }
    if (!frame || width < 80 || height < 40) {
        return false;
    }

    roi_x0 = std::max(0, std::min(roi_x0, width - 1));
    roi_x1 = std::max(0, std::min(roi_x1, width));
    roi_y0 = std::max(0, std::min(roi_y0, height - 1));
    roi_y1 = std::max(0, std::min(roi_y1, height));

    if (roi_x1 - roi_x0 < 40 || roi_y1 - roi_y0 < 20) {
        return false;
    }

    std::string candidate;
    float candidate_score = 0.0f;
    if (!extract_from_roi(frame,
                          width,
                          height,
                          roi_x0,
                          roi_x1,
                          roi_y0,
                          roi_y1,
                          candidate,
                          &candidate_score)) {
        return false;
    }

    word = std::move(candidate);
    if (confidence) {
        *confidence = candidate_score;
    }
    return true;
}

extern "C" bool app_simple_ocr_extract_word_c(const uint16_t *frame,
                                                int width,
                                                int height,
                                                char *out_word,
                                                size_t out_word_size,
                                                float *confidence)
{
    if (!out_word || out_word_size < 2) {
        return false;
    }

    std::string word;
    float score = 0.0f;
    bool ok = app_simple_ocr_extract_word(frame, width, height, word, &score);
    if (!ok || word.empty()) {
        out_word[0] = '\0';
        if (confidence) {
            *confidence = 0.0f;
        }
        return false;
    }

    std::string cleaned;
    cleaned.reserve(word.size());
    for (char c : word) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            cleaned.push_back(static_cast<char>(std::toupper(uc)));
        }
    }

    if (cleaned.size() < 1) {
        out_word[0] = '\0';
        if (confidence) {
            *confidence = 0.0f;
        }
        return false;
    }

    size_t copy_len = std::min(cleaned.size(), out_word_size - 1);
    std::memcpy(out_word, cleaned.data(), copy_len);
    out_word[copy_len] = '\0';

    if (confidence) {
        *confidence = score;
    }
    return true;
}

extern "C" bool app_simple_ocr_extract_word_c_roi(const uint16_t *frame,
                                                    int width,
                                                    int height,
                                                    int roi_x0,
                                                    int roi_x1,
                                                    int roi_y0,
                                                    int roi_y1,
                                                    char *out_word,
                                                    size_t out_word_size,
                                                    float *confidence)
{
    if (!out_word || out_word_size < 2) {
        return false;
    }

    std::string word;
    float score = 0.0f;
    bool ok = app_simple_ocr_extract_word_roi(frame,
                                              width,
                                              height,
                                              roi_x0,
                                              roi_x1,
                                              roi_y0,
                                              roi_y1,
                                              word,
                                              &score);
    if (!ok || word.empty()) {
        out_word[0] = '\0';
        if (confidence) {
            *confidence = 0.0f;
        }
        return false;
    }

    std::string cleaned;
    cleaned.reserve(word.size());
    for (char c : word) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            cleaned.push_back(static_cast<char>(std::toupper(uc)));
        }
    }

    if (cleaned.size() < 1) {
        out_word[0] = '\0';
        if (confidence) {
            *confidence = 0.0f;
        }
        return false;
    }

    size_t copy_len = std::min(cleaned.size(), out_word_size - 1);
    std::memcpy(out_word, cleaned.data(), copy_len);
    out_word[copy_len] = '\0';

    if (confidence) {
        *confidence = score;
    }
    return true;
}

extern "C" bool app_simple_ocr_refine_digit_roi_c(const uint16_t *frame,
                                                    int width,
                                                    int height,
                                                    int roi_x0,
                                                    int roi_x1,
                                                    int roi_y0,
                                                    int roi_y1,
                                                    int *out_x0,
                                                    int *out_x1,
                                                    int *out_y0,
                                                    int *out_y1)
{
    return refine_digit_box_from_roi(frame,
                                     width,
                                     height,
                                     roi_x0,
                                     roi_x1,
                                     roi_y0,
                                     roi_y1,
                                     out_x0,
                                     out_x1,
                                     out_y0,
                                     out_y1);
}
