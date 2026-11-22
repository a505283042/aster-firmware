/* SPDX-License-Identifier: MIT */
#include "sdkconfig.h"
#include "aster_player_service.h"
#include "aster_media.h"
#include "aster_audio_output.h"
#include "bt_source.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
bool aster_idf_bt_audio_info(aster_player_info_t *info);
const char *aster_idf_bt_device_name(void);
int aster_idf_bt_audio_control(uint32_t command);

/* FAT directory order is the order shown by HiFiPlayer. This task is the
 * only automatic-next owner once the capability is advertised: the APP no
 * longer needs to stay open to maintain a local playback queue. */
static bool auto_next_running;
static const char *music_basename(const char *path)
{
    if (!strncmp(path, "/SD:/music/", 11))
        return path + 11;
    if (!strncmp(path, "/SD/music/", 10))
        return path + 10;
    return NULL;
}

static bool music_filename(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext || !ext[1])
        return false;
    char tail[5] = {0};
    size_t length = strlen(++ext);
    if (length >= sizeof(tail))
        return false;
    for (size_t i = 0; i < length; i++)
        tail[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? (char)(ext[i] + ('a' - 'A')) : ext[i];
    return !strcmp(tail, "flac") || !strcmp(tail, "mp3") || !strcmp(tail, "wav");
}

/* Do not allocate a full firmware playlist: locate the following filename
 * with one bounded directory pass, wrapping at the last entry. */
static bool music_successor(const char *path, char out[ASTER_MEDIA_PATH_MAX])
{
    const char *current = music_basename(path);
    if (!current || !*current)
        return false;
    DIR *directory = opendir("/SD/music");
    if (!directory)
        return false;
    char first[ASTER_MEDIA_PATH_MAX] = {0};
    char chosen[ASTER_MEDIA_PATH_MAX] = {0};
    bool found_current = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
#ifdef DT_DIR
        if (entry->d_type == DT_DIR)
            continue;
#endif
        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (name[0] == '.' || !music_filename(name) ||
            length + sizeof("/SD:/music/") > ASTER_MEDIA_PATH_MAX)
            continue;
        if (!first[0])
            memcpy(first, name, length + 1);
        if (found_current) {
            memcpy(chosen, name, length + 1);
            break;
        }
        if (!strcmp(name, current))
            found_current = true;
    }
    closedir(directory);
    if (!found_current)
        return false;
    if (!chosen[0] && first[0] && strcmp(first, current))
        memcpy(chosen, first, strlen(first) + 1);
    if (!chosen[0])
        return false; /* Only one file, or the current entry disappeared. */
    int n = snprintf(out, ASTER_MEDIA_PATH_MAX, "/SD:/music/%s", chosen);
    return n > 0 && (unsigned)n < ASTER_MEDIA_PATH_MAX;
}

static void local_auto_next_task(void *ctx)
{
    (void)ctx;
    char armed[ASTER_MEDIA_PATH_MAX] = {0};
    TickType_t ended_since = 0;
    for (;;) {
        aster_media_snapshot_t snapshot = {.size = sizeof(snapshot), .version = 1};
        if (!aster_media_snapshot(&snapshot)) {
            if (snapshot.state == ASTER_MEDIA_OPENING ||
                snapshot.state == ASTER_MEDIA_PLAYING || snapshot.state == ASTER_MEDIA_PAUSED) {
                const char *name = music_basename(snapshot.path);
                if (name && *name)
                    snprintf(armed, sizeof(armed), "%s", snapshot.path);
                else
                    armed[0] = 0;
                ended_since = 0;
            } else if (snapshot.state == ASTER_MEDIA_ENDED && armed[0] &&
                       !strcmp(armed, snapshot.path)) {
                TickType_t now = xTaskGetTickCount();
                if (!ended_since)
                    ended_since = now;
                /* Give older APPs time to claim the ENDED state themselves.
                 * A new APP sees CAP_AUTO_NEXT and leaves it to this task. */
                if ((TickType_t)(now - ended_since) >= pdMS_TO_TICKS(500)) {
                    armed[0] = 0; /* Exactly once for this completed song. */
                    ended_since = 0;
                    char next[ASTER_MEDIA_PATH_MAX];
                    if (music_successor(snapshot.path, next)) {
                        int result = aster_media_play(next, ASTER_MEDIA_OUTPUT_SYSTEM);
                        if (result)
                            ESP_LOGW("hifi_autoplay", "next failed: %d", result);
                        else
                            ESP_LOGI("hifi_autoplay", "next: %s", next);
                    }
                }
            } else if (snapshot.state != ASTER_MEDIA_ENDED) {
                armed[0] = 0;
                ended_since = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static int info(aster_player_info_t *out)
{
    out->capabilities = ASTER_PLAYER_CAP_WAV | ASTER_PLAYER_CAP_FLAC;
    if (auto_next_running)
        out->capabilities |= ASTER_PLAYER_CAP_AUTO_NEXT;
#ifdef ASTER_AUDIO_SPECTRUM_ENABLED
    out->capabilities |= ASTER_PLAYER_CAP_SPECTRUM;
#endif
#ifdef ASTER_IDF_BT_AUDIO
    out->capabilities |= ASTER_PLAYER_CAP_BT_SINK | ASTER_PLAYER_CAP_REMOTE |
                         ASTER_PLAYER_CAP_BT_SOURCE | ASTER_PLAYER_CAP_SYSTEM_OUTPUT;
#endif
    if (aster_idf_bt_audio_info(out)) {
#ifdef ASTER_AUDIO_SPECTRUM_ENABLED
        if (out->state == ASTER_PLAYER_PLAYING)
            aster_audio_output_spectrum(ASTER_AUDIO_OUTPUT_WIRED, out->reserved);
#endif
        return 0;
    }
    snprintf(out->device, sizeof(out->device), "%s", aster_idf_bt_device_name());
    aster_media_snapshot_t media;
    int r = aster_media_snapshot(&media);
    if (r)
        return r;
    out->mode = ASTER_PLAYER_LOCAL;
    out->state = media.state;
    out->error = media.error;
    out->rate = media.rate;
    out->bits = media.bits;
    out->channels = media.channels;
    if (media.rate) {
        out->position_ms = (uint32_t)(media.frame * 1000 / media.rate);
        out->duration_ms = (uint32_t)(media.total_frames * 1000 / media.rate);
    }
    memcpy(out->path, media.path, sizeof(out->path));
    if (media.endpoint == ASTER_AUDIO_OUTPUT_BLUETOOTH)
        out->flags |= ASTER_PLAYER_F_BT_OUTPUT;
#ifdef ASTER_AUDIO_SPECTRUM_ENABLED
    if (out->state == ASTER_PLAYER_PLAYING)
        aster_audio_output_spectrum(media.endpoint, out->reserved);
#endif
    return 0;
}

static int control(const aster_player_request_t *request)
{
    if (request->command >= ASTER_PLAYER_SOURCE_ON)
        return aster_idf_bt_source_request(request->command, request->path);
    if (request->command == ASTER_PLAYER_BT_ON || request->command == ASTER_PLAYER_BT_OFF ||
        request->command == ASTER_PLAYER_NEXT || request->command == ASTER_PLAYER_PREVIOUS)
        return aster_idf_bt_audio_control(request->command);
    aster_player_info_t bt = {0};
    if (aster_idf_bt_audio_info(&bt)) {
        if (request->command == ASTER_PLAYER_SEEK)
            return -ENOTSUP;
        if (request->command == ASTER_PLAYER_PAUSE || request->command == ASTER_PLAYER_RESUME)
            return aster_idf_bt_audio_control(request->command);
        return -EBUSY;
    }
    switch (request->command) {
    case ASTER_PLAYER_PLAY_FILE:
        if (request->value == ASTER_PLAYER_OUTPUT_SYSTEM) {
#ifdef ASTER_IDF_BT_AUDIO
            return aster_media_play(request->path, ASTER_MEDIA_OUTPUT_SYSTEM);
#else
            return -ENOTSUP;
#endif
        }
        if (request->value > ASTER_AUDIO_OUTPUT_BLUETOOTH)
            return -EINVAL;
        if (request->value == ASTER_AUDIO_OUTPUT_BLUETOOTH) {
            aster_player_bt_list_t list = {0};
            int r = aster_idf_bt_source_list(&list);
            if (r)
                return r;
            if (list.state != ASTER_PLAYER_BT_CONNECTED || !list.output_rate)
                return -ENOTCONN;
        }
        return aster_media_play(request->path, request->value ? request->value : ASTER_AUDIO_OUTPUT_WIRED);
    case ASTER_PLAYER_PAUSE:
        return aster_media_pause();
    case ASTER_PLAYER_RESUME:
        return aster_media_resume();
    case ASTER_PLAYER_STOP:
        return aster_media_stop();
    case ASTER_PLAYER_SEEK:
        return aster_media_seek(request->value);
    default:
        return -ENOTSUP;
    }
}

int aster_idf_player_register(void)
{
    static const aster_player_ops_t ops = {
        .info = info, .control = control, .bt_list = aster_idf_bt_source_list};
#ifdef ASTER_IDF_BT_AUDIO
    int err = aster_bt_tx_register();
    if (err)
        return err;
#endif
    int media = aster_media_init();
    if (media)
        return media;
    int registered = aster_player_service_register(&ops);
    if (registered)
        return registered;
    if (!auto_next_running &&
        xTaskCreate(local_auto_next_task, "hifi_autoplay", 6144, NULL, 4, NULL) == pdPASS)
        auto_next_running = true;
    else if (!auto_next_running)
        ESP_LOGW("hifi_autoplay", "task unavailable; APP will handle EOF while open");
    return 0;
}
