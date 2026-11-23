/* SPDX-License-Identifier: MIT */
/* Board diagnostics execute outside the console/UI task and are deliberately
 * bounded. PCM includes a short envelope to avoid discontinuities at edges. */
#include "aster_audio_output.h"
#include "aster_media.h"
#include "aster_audio_prefetch.h"
#include "aster_platform.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <string.h>
static atomic_bool running;
static uint32_t test_rate, test_format;
static const char *TAG = "audio_test";

static void tone(void *arg)
{
    (void)arg;
    const uint32_t frames_per_block = 256;
    uint8_t *pcm = heap_caps_malloc(frames_per_block * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    aster_audio_format_t f = {sizeof(f), 1,           test_rate,
                              2,         test_format, ASTER_AUDIO_OUTPUT_EXCLUSIVE};
    int r = pcm ? aster_audio_output_open(ASTER_AUDIO_OUTPUT_WIRED, &f) : -ENOMEM;
    bool opened = !r;
    int volume = aster_plat_audio_get_volume();
    if (!r)
        r = aster_plat_audio_set_volume(50);
    uint32_t bytes = test_format + 1;
    int32_t sine[256];
    for (unsigned i = 0; i < 256; i++)
        sine[i] = (int32_t)(sinf(6.28318530718f * i / 256) * 268435455.0f);
    int64_t began = esp_timer_get_time();
    for (unsigned channel = 0; channel < 2 && !r; channel++) {
        ESP_LOGI(TAG, "%s channel, %lu Hz / PCM%lu; low level", channel ? "RIGHT" : "LEFT",
                 (unsigned long)test_rate, (unsigned long)(bytes * 8));
        uint32_t total = test_rate * 2, phase = 0;
        uint32_t step = (uint32_t)(((uint64_t)(channel ? 660 : 440) << 32) / test_rate);
        for (uint32_t pos = 0; pos < total && !r;) {
            uint32_t n = total - pos;
            if (n > frames_per_block)
                n = frames_per_block;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t at = pos + i, edge = test_rate / 100;
                int32_t sample = sine[phase >> 24];
                phase += step;
                if (at < edge)
                    sample = (int32_t)((int64_t)sample * at / edge);
                else if (total - at < edge)
                    sample = (int32_t)((int64_t)sample * (total - at) / edge);
                uint32_t bits;
                memcpy(&bits, &sample, 4);
                for (unsigned ch = 0; ch < 2; ch++)
                    for (unsigned b = 0; b < bytes; b++)
                        pcm[((size_t)i * 2 + ch) * bytes + b] =
                            ch == channel ? (uint8_t)(bits >> (8 * (4 - bytes + b))) : 0;
            }
            uint32_t sent = 0;
            while (sent < n) {
                int accepted = aster_audio_output_write(ASTER_AUDIO_OUTPUT_WIRED,
                                                        pcm + (size_t)sent * 2 * bytes, n - sent);
                if (accepted < 0) {
                    r = accepted;
                    break;
                }
                sent += (uint32_t)accepted;
            }
            pos += n;
        }
    }
    if (opened) {
        int drained = aster_audio_output_drain(ASTER_AUDIO_OUTPUT_WIRED);
        if (!r)
            r = drained;
        aster_audio_output_stats_t stats;
        if (!aster_audio_output_stats(ASTER_AUDIO_OUTPUT_WIRED, &stats))
            ESP_LOGI(TAG, "submitted=%" PRIu64 " frames errors=%lu elapsed_ms=%" PRId64,
                     stats.submitted_frames, (unsigned long)stats.errors,
                     (esp_timer_get_time() - began) / 1000);
        int closed = aster_audio_output_close(ASTER_AUDIO_OUTPUT_WIRED);
        if (!r)
            r = closed;
        aster_plat_audio_set_volume(volume);
    }
    free(pcm);
    ESP_LOGI(TAG, "finished result=%d", r);
    atomic_store(&running, false);
    vTaskDelete(NULL);
}

static int media_command(int argc, char **argv)
{
    int r = -EINVAL;
    if (argc == 3 && (!strcmp(argv[1], "play") || !strcmp(argv[1], "play-bt"))) {
        if (atomic_load(&running))
            r = -EBUSY;
        else if (strncmp(argv[2], "/SD/", 4) && strncmp(argv[2], "/SD:/", 5))
            r = -EINVAL;
        else
            r = aster_media_play(argv[2], !strcmp(argv[1], "play-bt") ? ASTER_AUDIO_OUTPUT_BLUETOOTH
                                                                      : ASTER_AUDIO_OUTPUT_WIRED);
    } else if (argc == 2 && !strcmp(argv[1], "pause"))
        r = aster_media_pause();
    else if (argc == 2 && !strcmp(argv[1], "resume"))
        r = aster_media_resume();
    else if (argc == 2 && !strcmp(argv[1], "stop"))
        r = aster_media_stop();
    else if (argc == 3 && !strcmp(argv[1], "seek")) {
        char *end;
        unsigned long ms = strtoul(argv[2], &end, 10);
        if (!*end && ms <= UINT32_MAX)
            r = aster_media_seek((uint32_t)ms);
    } else if (argc == 2 && !strcmp(argv[1], "prefetch")) {
        aster_audio_prefetch_window_t w;
        r = aster_audio_prefetch_window(&w) ? 0 : -EIO;
        if (!r)
            printf("MEDIA PREFETCH active=%u buffered=%lu/%lu (%lu%%) eof=%u "
                   "waits=%lu wait_max_ms=%lu error=%ld\n",
                   w.active ? 1u : 0u,
                   (unsigned long)w.buffered_bytes,
                   (unsigned long)w.capacity_bytes,
                   w.capacity_bytes ? (unsigned long)(100u * (uint64_t)w.buffered_bytes /
                                                      w.capacity_bytes) : 0ul,
                   w.eof ? 1u : 0u, (unsigned long)w.wait_count,
                   (unsigned long)w.wait_max_ms, (long)w.io_error);
    } else if (argc == 2 && !strcmp(argv[1], "status")) {
        aster_media_snapshot_t s;
        r = aster_media_snapshot(&s);
        if (!r)
            printf(
                "MEDIA state=%lu error=%ld rate=%lu bits=%lu channels=%lu codec=%lu frame=%" PRIu64
                "/%" PRIu64 " decoder=%u peak=%u output=%s path=%s\n",
                (unsigned long)s.state, (long)s.error, (unsigned long)s.rate, (unsigned long)s.bits,
                (unsigned long)s.channels, (unsigned long)s.codec, s.frame, s.total_frames,
                (unsigned)s.decoder_bytes, (unsigned)s.decoder_peak,
                s.endpoint == ASTER_AUDIO_OUTPUT_BLUETOOTH ? "bluetooth" :
                s.endpoint == ASTER_AUDIO_OUTPUT_WIRED ? "wired" : "none", s.path);
#ifdef ASTER_AUDIO_SPECTRUM_ENABLED
        if (!r) {
            uint32_t levels[2];
            aster_audio_output_spectrum(s.endpoint, levels);
            printf("SPECTRUM left=%08" PRIx32 " right=%08" PRIx32 "\n", levels[0], levels[1]);
        }
#endif
        printf("HEAP internal=%u external=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    printf("media command result=%d\n", r);
    return r ? 1 : 0;
}

static int command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "status")) {
        aster_audio_output_caps_t caps;
        int r = aster_audio_output_caps(ASTER_AUDIO_OUTPUT_WIRED, &caps);
        if (!r)
            printf("CS43131 PCM formats=0x%lx rates=0x%lx max_frames=%lu test_running=%d\n",
                   (unsigned long)caps.formats, (unsigned long)caps.rates,
                   (unsigned long)caps.max_write_frames, atomic_load(&running));
        printf("audio status result=%d\n", r);
        return r ? 1 : 0;
    }
    if (argc != 4 || strcmp(argv[1], "tone")) {
        puts("aster-audio status | tone RATE 16|24|32 (2s left then 2s right, low volume)");
        return 1;
    }
    char *end;
    unsigned long rate = strtoul(argv[2], &end, 10);
    if (*end || !aster_audio_rate_bit(rate))
        return 1;
    unsigned long bits = strtoul(argv[3], &end, 10);
    if (*end || (bits != 16 && bits != 24 && bits != 32))
        return 1;
    aster_media_snapshot_t media;
    if (!aster_media_snapshot(&media) &&
        (media.state == ASTER_MEDIA_OPENING || media.state == ASTER_MEDIA_PLAYING ||
         media.state == ASTER_MEDIA_PAUSED || media.state == ASTER_MEDIA_STOPPING)) {
        puts("media session busy");
        return 1;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&running, &expected, true)) {
        puts("audio test busy");
        return 1;
    }
    test_rate = rate;
    test_format = bits == 16   ? ASTER_AUDIO_PCM_S16LE
                  : bits == 24 ? ASTER_AUDIO_PCM_S24LE
                               : ASTER_AUDIO_PCM_S32LE;
    if (xTaskCreate(tone, "audio_test", 4096, NULL, 5, NULL) != pdPASS) {
        atomic_store(&running, false);
        return 1;
    }
    puts("audio test queued");
    return 0;
}

void fakepod_audio_diagnostic_register(void)
{
    const esp_console_cmd_t media = {
        .command = "aster-media",
        .help =
            "Native background WAV/FLAC: play /SD/... | pause | resume | seek MS | stop | status | prefetch",
        .func = media_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&media));
    const esp_console_cmd_t cmd = {.command = "aster-audio",
                                   .help = "Bounded low-volume CS43131 diagnostics",
                                   .func = command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
