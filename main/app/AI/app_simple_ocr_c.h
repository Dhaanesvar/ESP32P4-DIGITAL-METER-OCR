#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C bridge for lightweight OCR over RGB565 frames.
bool app_simple_ocr_extract_word_c(const uint16_t *frame,
                                   int width,
                                   int height,
                                   char *out_word,
                                   size_t out_word_size,
                                   float *confidence);

bool app_simple_ocr_extract_word_c_roi(const uint16_t *frame,
                                       int width,
                                       int height,
                                       int roi_x0,
                                       int roi_x1,
                                       int roi_y0,
                                       int roi_y1,
                                       char *out_word,
                                       size_t out_word_size,
                                       float *confidence);

// Refines a coarse ROI to a tighter digit/text bounding box.
// Returns false when no reliable digit-like region is found.
bool app_simple_ocr_refine_digit_roi_c(const uint16_t *frame,
                                       int width,
                                       int height,
                                       int roi_x0,
                                       int roi_x1,
                                       int roi_y0,
                                       int roi_y1,
                                       int *out_x0,
                                       int *out_x1,
                                       int *out_y0,
                                       int *out_y1);

#ifdef __cplusplus
}
#endif
