/* SPDX-License-Identifier: MIT
 * ESP32-S31 hard-JPEG path adapted from FakePod_S31/src/drivers/jpeg.
 * Hardware decodes compact/MCU-padded RGB565; the caller receives exactly
 * the same fit-into-cap layout as the Aster software image renderer.
 *
 * Restrict the accelerated fast path to modest compressed/raster sizes:
 * oversized, progressive or otherwise unsupported JPEG stays on Aster's
 * constant-memory TJpgDec path rather than exhausting PSRAM/audio buffers.
 */
#include "jpeg_hw.h"
#include <aster/error.h>
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

enum { JPEG_MAX_INPUT = 2 * 1024 * 1024, JPEG_MAX_RAW = 4 * 1024 * 1024,
       JPEG_ENGINE_TIMEOUT_MS = 800 };
static const char *TAG = "ASTER_S31_JPEG";
static jpeg_decoder_handle_t engine;
static SemaphoreHandle_t engine_mutex;
static bool engine_failed;
static bool first_success;

static uint32_t aligned(uint32_t n, uint32_t step)
{
    return (n + step - 1u) / step * step;
}

static void mcu_for(jpeg_down_sampling_type_t method, uint32_t *w, uint32_t *h)
{
    switch (method) {
    case JPEG_DOWN_SAMPLING_YUV420:
        *w = 16u; *h = 16u; break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *w = 16u; *h = 8u; break;
    default:
        *w = 8u; *h = 8u; break;
    }
}

static bool ready_engine(void)
{
    if (engine_failed) return false;
    if (!engine_mutex) {
        engine_mutex = xSemaphoreCreateMutex();
        if (!engine_mutex) return false;
    }
    if (engine) return true;
    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = JPEG_ENGINE_TIMEOUT_MS
    };
    esp_err_t error = jpeg_new_decoder_engine(&cfg, &engine);
    if (error != ESP_OK || !engine) {
        engine = NULL;
        engine_failed = true;
        ESP_LOGW(TAG, "JPEG hardware init failed: %d; software fallback", error);
        return false;
    }
    ESP_LOGI(TAG, "ESP32-S31 hardware JPEG engine available");
    return true;
}

static uint16_t lerp565(uint16_t left, uint16_t right, unsigned alpha)
{
    unsigned a = 256u - alpha;
    unsigned r = (((left >> 11) * a + (right >> 11) * alpha) + 128u) >> 8;
    unsigned g = (((((left >> 5) & 63u) * a +
                    ((right >> 5) & 63u) * alpha)) + 128u) >> 8;
    unsigned b = (((left & 31u) * a + (right & 31u) * alpha) + 128u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool resize_into(const uint16_t *src, uint32_t w, uint32_t h, uint32_t stride,
                        uint16_t *dst, uint32_t ow, uint32_t oh,
                        bool nearest, aster_imgdec_progress_fn progress, void *context)
{
    for (uint32_t y = 0; y < oh; ++y) {
        if (progress && (y % 16u) == 0 && !progress(context)) return false;
        uint32_t yy = oh > 1u ? (uint32_t)((uint64_t)y * (h - 1u) * 256u / (oh - 1u)) : 0u;
        uint32_t sy = yy >> 8, fy = yy & 255u;
        uint32_t ny = sy + 1u < h ? sy + 1u : sy;
        for (uint32_t x = 0; x < ow; ++x) {
            uint32_t xx = ow > 1u ? (uint32_t)((uint64_t)x * (w - 1u) * 256u / (ow - 1u)) : 0u;
            uint32_t sx = xx >> 8, fx = xx & 255u;
            uint32_t nx = sx + 1u < w ? sx + 1u : sx;
            if (nearest) {
                uint32_t xxn = fx >= 128u ? nx : sx;
                uint32_t yyn = fy >= 128u ? ny : sy;
                dst[y * ow + x] = src[yyn * stride + xxn];
            } else {
                uint16_t row_a = lerp565(src[sy * stride + sx],
                                         src[sy * stride + nx], fx);
                uint16_t row_b = lerp565(src[ny * stride + sx],
                                         src[ny * stride + nx], fx);
                dst[y * ow + x] = lerp565(row_a, row_b, fy);
            }
        }
    }
    return true;
}

aster_error_t aster_s31_jpeg_render(const char *path, uint8_t *rgb565,
                                   uint32_t cap_w, uint32_t cap_h,
                                   uint32_t *out_w, uint32_t *out_h,
                                   bool nearest,
                                   aster_imgdec_progress_fn progress, void *context)
{
    if (!path || !rgb565 || !out_w || !out_h || !cap_w || !cap_h)
        return ASTER_EINVAL;
    aster_imgdec_info_t probe = {0};
    if (aster_imgdec_probe(path, &probe, cap_w, cap_h) != ASTER_OK ||
        probe.fmt != ASTER_IMG_JPG || probe.unsupported ||
        probe.file_size < 32u || probe.file_size > JPEG_MAX_INPUT ||
        !probe.out_w || !probe.out_h)
        return ASTER_ENOTSUP;
    if (progress && !progress(context)) return ASTER_ECANCELLED;
    FILE *file = fopen(path, "rb");
    if (!file) return ASTER_ENOTSUP;
    uint8_t *compressed = heap_caps_aligned_alloc(
        64u, probe.file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!compressed) {
        fclose(file);
        return ASTER_ENOTSUP;
    }
    size_t read_bytes = 0;
    while (read_bytes < probe.file_size) {
        /* Bound each SD access and report progress to Aster's watchdog;
         * input files can sit on slow cards while music is playing. */
        size_t want = probe.file_size - read_bytes;
        if (want > 8192u) want = 8192u;
        size_t got = fread(compressed + read_bytes, 1, want, file);
        if (!got) break;
        read_bytes += got;
        if (progress && !progress(context)) {
            fclose(file);
            heap_caps_free(compressed);
            return ASTER_ECANCELLED;
        }
    }
    fclose(file);
    if (read_bytes != probe.file_size) {
        heap_caps_free(compressed);
        return ASTER_ENOTSUP;
    }
    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(compressed, probe.file_size, &info) != ESP_OK ||
        !info.width || !info.height ||
        (uint64_t)info.width * info.height % 8u != 0u ||
        info.width != probe.src_w || info.height != probe.src_h) {
        heap_caps_free(compressed);
        return ASTER_ENOTSUP;
    }
    uint32_t mw = 8u, mh = 8u;
    mcu_for(info.sample_method, &mw, &mh);
    uint32_t pw = aligned(info.width, mw), ph = aligned(info.height, mh);
    uint64_t bytes = (uint64_t)pw * ph * sizeof(uint16_t);
    if (bytes > JPEG_MAX_RAW) {
        heap_caps_free(compressed);
        return ASTER_ENOTSUP;
    }
    if (!ready_engine()) {
        heap_caps_free(compressed);
        return ASTER_ENOTSUP;
    }
    jpeg_decode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t allocated = 0;
    uint8_t *decoded = jpeg_alloc_decoder_mem((size_t)bytes, &mem, &allocated);
    if (!decoded) {
        heap_caps_free(compressed);
        return ASTER_ENOTSUP;
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601
    };
    uint32_t actual = 0;
    int64_t began = esp_timer_get_time();
    xSemaphoreTake(engine_mutex, portMAX_DELAY);
    esp_err_t result = jpeg_decoder_process(engine, &cfg, compressed,
                          probe.file_size, decoded, allocated, &actual);
    xSemaphoreGive(engine_mutex);
    aster_error_t rc = ASTER_ENOTSUP;
    if (result == ESP_OK && (uint64_t)actual == bytes) {
        if (resize_into((const uint16_t *)decoded, info.width, info.height, pw,
                        (uint16_t *)rgb565, probe.out_w, probe.out_h,
                        nearest, progress, context)) {
            *out_w = probe.out_w;
            *out_h = probe.out_h;
            rc = ASTER_OK;
            if (!first_success) {
                first_success = true;
                ESP_LOGI(TAG, "first hardware JPEG: %ux%u -> %ux%u, %lldus",
                         info.width, info.height, *out_w, *out_h,
                         (long long)(esp_timer_get_time() - began));
            }
        } else rc = ASTER_ECANCELLED;
    }
    /* Both ESP-IDF allocators return heap-managed buffers. */
    heap_caps_free(decoded);
    heap_caps_free(compressed);
    return rc;
}
