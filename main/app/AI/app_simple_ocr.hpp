#pragma once

#include <cstdint>
#include <string>

// Lightweight uppercase OCR intended for high-contrast printed text.
// Returns true when a likely word is extracted from the frame.
bool app_simple_ocr_extract_word(const uint16_t *frame,
                                 int width,
                                 int height,
                                 std::string &word,
                                 float *confidence = nullptr);

// OCR constrained to a caller-provided ROI in source frame coordinates.
bool app_simple_ocr_extract_word_roi(const uint16_t *frame,
                                     int width,
                                     int height,
                                     int roi_x0,
                                     int roi_x1,
                                     int roi_y0,
                                     int roi_y1,
                                     std::string &word,
                                     float *confidence = nullptr);
