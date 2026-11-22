/* SPDX-License-Identifier: MIT
 * FakePod_S31 hardware JPEG priority path for Aster's ESP-IDF image service.
 * Returns ASTER_ENOTSUP to request the existing streaming software fallback.
 */
#ifndef ASTER_IDF_S31_JPEG_HW_H
#define ASTER_IDF_S31_JPEG_HW_H
#include "imgdec.h"
#include <stdint.h>
#include <stdbool.h>

aster_error_t aster_s31_jpeg_render(const char *path, uint8_t *rgb565,
                                   uint32_t cap_w, uint32_t cap_h,
                                   uint32_t *out_w, uint32_t *out_h,
                                   bool nearest,
                                   aster_imgdec_progress_fn progress,
                                   void *context);
#endif
