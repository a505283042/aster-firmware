/* SPDX-License-Identifier: MIT */
#ifndef ASTER_AUDIO_PREFETCH_H
#define ASTER_AUDIO_PREFETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S31 FLAC's sequential compressed-byte prefetch. This is an internal
 * firmware service, not an APP ABI. The producer owns its separate SD file;
 * only the decoder thread calls read/stop. Never free a running worker.
 */
typedef struct aster_audio_prefetch aster_audio_prefetch_t;
typedef struct {
    bool active;
    bool eof;
    uint32_t buffered_bytes;
    uint32_t capacity_bytes;
    uint32_t wait_count;
    uint32_t wait_max_ms;
    int32_t io_error;
} aster_audio_prefetch_window_t;

aster_audio_prefetch_t *aster_audio_prefetch_start(const char *path,
                                                   uint64_t start_offset,
                                                   uint64_t file_length);

/* Returns accepted bytes, zero at EOF, or a negative errno on error.
 * A short positive read is valid. The decoder's existing callback loops
 * until its request is satisfied and updates its independent logical cursor.
 */
int aster_audio_prefetch_read(aster_audio_prefetch_t *prefetch, void *data,
                              size_t size, bool (*cancel)(void *), void *ctx);
void aster_audio_prefetch_stop(aster_audio_prefetch_t *prefetch);

/* Snapshot uses atomic scalars: it never exposes worker/decoder pointers.
 * When active=false, callers must not interpret buffered_bytes as safety
 * permission to perform artwork I/O.
 */
bool aster_audio_prefetch_window(aster_audio_prefetch_window_t *out);
#endif
