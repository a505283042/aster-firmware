/* SPDX-License-Identifier: MIT
 * S31 FLAC sequential SD prefetch. Adapted conceptually from FakePod_S31's
 * buffered audio source: one producer task, PSRAM compressed ring, short
 * 8-KiB SD reads, and an atomic fill watermark for later artwork QoS.
 *
 * Only the producer accesses its own OS file handle. The decoder keeps its
 * separate random-access handle for dr_flac seek, and all cancellations
 * join the producer before freeing buffers. No hard task deletion.
 */
#include "aster_audio_prefetch.h"
#include "aster_os.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

enum {
    AUDIO_PREFETCH_RING_BYTES = 128u * 1024u,
    AUDIO_PREFETCH_READ_BYTES = 8u * 1024u,
    AUDIO_PREFETCH_MIN_FREE = 2u * 1024u,
    AUDIO_PREFETCH_STALL_MS = 2000u
};

struct aster_audio_prefetch {
    aster_os_file_t file;
    aster_os_mutex_t lock;
    aster_os_task_t worker;
    uint8_t *ring;
    uint8_t *scratch;
    uint64_t offset, file_length; /* producer-only */
    size_t head, tail, filled;    /* guarded by lock */
    bool eof;                     /* guarded by lock */
    int error;                    /* guarded by lock */
    atomic_bool stopping;
    uint32_t wait_count, wait_max_ms; /* decoder-only */
};

/* Exactly one local player session is active. Keep exported stats in
 * independent atomics, never a pointer which could outlive stop(). */
static atomic_bool window_active = ATOMIC_VAR_INIT(false);
static atomic_bool window_eof = ATOMIC_VAR_INIT(false);
static atomic_uint window_filled = ATOMIC_VAR_INIT(0);
static atomic_uint window_wait_count = ATOMIC_VAR_INIT(0);
static atomic_uint window_wait_max_ms = ATOMIC_VAR_INIT(0);
static atomic_int window_error = ATOMIC_VAR_INIT(0);

static void publish_empty(void)
{
    atomic_store(&window_active, false);
    atomic_store(&window_filled, 0);
    atomic_store(&window_eof, false);
    atomic_store(&window_error, 0);
    atomic_store(&window_wait_count, 0);
    atomic_store(&window_wait_max_ms, 0);
}

static void producer(void *arg)
{
    aster_audio_prefetch_t *p = arg;
    unsigned consecutive = 0;
    while (!atomic_load(&p->stopping)) {
        if (p->offset >= p->file_length) {
            (void)aster_os_mutex_lock(&p->lock, ASTER_OS_WAIT_FOREVER);
            p->eof = true;
            atomic_store(&window_eof, true);
            (void)aster_os_mutex_unlock(&p->lock);
            return;
        }
        (void)aster_os_mutex_lock(&p->lock, ASTER_OS_WAIT_FOREVER);
        size_t free_bytes = AUDIO_PREFETCH_RING_BYTES - p->filled;
        (void)aster_os_mutex_unlock(&p->lock);
        if (free_bytes < AUDIO_PREFETCH_MIN_FREE) {
            consecutive = 0;
            aster_os_sleep_ms(1);
            continue;
        }
        size_t n = free_bytes < AUDIO_PREFETCH_READ_BYTES ?
                   free_bytes : AUDIO_PREFETCH_READ_BYTES;
        uint64_t left = p->file_length - p->offset;
        if (left < n)
            n = (size_t)left;
        /* No shared mutex is held during SDMMC/FatFS I/O. Audio consuming
         * the ring never waits on an in-flight storage operation. */
        int got = aster_os_file_read(&p->file, p->scratch, n);
        if (atomic_load(&p->stopping))
            return;
        if (got <= 0 || (size_t)got > n) {
            int error = got < 0 ? got : -EIO;
            (void)aster_os_mutex_lock(&p->lock, ASTER_OS_WAIT_FOREVER);
            p->error = error;
            atomic_store(&window_error, error);
            (void)aster_os_mutex_unlock(&p->lock);
            return;
        }

        (void)aster_os_mutex_lock(&p->lock, ASTER_OS_WAIT_FOREVER);
        size_t first = (size_t)got;
        if (first > AUDIO_PREFETCH_RING_BYTES - p->head)
            first = AUDIO_PREFETCH_RING_BYTES - p->head;
        memcpy(p->ring + p->head, p->scratch, first);
        if ((size_t)got > first)
            memcpy(p->ring, p->scratch + first, (size_t)got - first);
        p->head = (p->head + (size_t)got) % AUDIO_PREFETCH_RING_BYTES;
        p->filled += (size_t)got;
        atomic_store(&window_filled, (unsigned)p->filled);
        (void)aster_os_mutex_unlock(&p->lock);
        p->offset += (unsigned)got;
        if (++consecutive >= 4) {
            consecutive = 0;
            aster_os_sleep_ms(1);
        }
    }
}

aster_audio_prefetch_t *aster_audio_prefetch_start(const char *path,
                                                   uint64_t start_offset,
                                                   uint64_t file_length)
{
    if (!path || !*path || start_offset > file_length || file_length > INT64_MAX ||
        atomic_load(&window_active))
        return NULL;
    aster_audio_prefetch_t *p = aster_os_alloc_external(sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->ring = aster_os_alloc_external(AUDIO_PREFETCH_RING_BYTES);
    p->scratch = aster_os_alloc_external(AUDIO_PREFETCH_READ_BYTES);
    p->offset = start_offset;
    p->file_length = file_length;
    if (!p->ring || !p->scratch || aster_os_mutex_create(&p->lock))
        goto fail;
    if (aster_os_file_open(&p->file, path, ASTER_OS_READ))
        goto fail;
    if (aster_os_file_seek(&p->file, (int64_t)start_offset, ASTER_OS_SEEK_SET))
        goto fail;
    atomic_init(&p->stopping, false);
    publish_empty();
    atomic_store(&window_active, true);
    const aster_os_task_config_t config = {
        .name = "hifi_preload", .stack_bytes = 4096, .priority = 19, .core = -1};
    if (aster_os_task_start(&p->worker, &config, producer, p))
        goto fail;
    return p;

fail:
    publish_empty();
    if (p->file.handle)
        (void)aster_os_file_close(&p->file);
    if (p->lock.handle)
        (void)aster_os_mutex_destroy(&p->lock);
    aster_os_free_external(p->scratch);
    aster_os_free_external(p->ring);
    aster_os_free_external(p);
    return NULL;
}

int aster_audio_prefetch_read(aster_audio_prefetch_t *p, void *data,
                              size_t size, bool (*cancel)(void *), void *ctx)
{
    if (!p || (!data && size))
        return -EINVAL;
    if (!size)
        return 0;
    uint32_t waiting_since = aster_os_uptime32();
    bool waited = false;
    for (;;) {
        if (cancel && cancel(ctx))
            return -ECANCELED;
        (void)aster_os_mutex_lock(&p->lock, ASTER_OS_WAIT_FOREVER);
        size_t n = p->filled < size ? p->filled : size;
        if (n) {
            size_t first = n;
            if (first > AUDIO_PREFETCH_RING_BYTES - p->tail)
                first = AUDIO_PREFETCH_RING_BYTES - p->tail;
            memcpy(data, p->ring + p->tail, first);
            if (n > first)
                memcpy((uint8_t *)data + first, p->ring, n - first);
            p->tail = (p->tail + n) % AUDIO_PREFETCH_RING_BYTES;
            p->filled -= n;
            atomic_store(&window_filled, (unsigned)p->filled);
        }
        bool eof = p->eof;
        int error = p->error;
        (void)aster_os_mutex_unlock(&p->lock);
        if (n) {
            if (waited) {
                uint32_t duration = (uint32_t)(aster_os_uptime32() - waiting_since);
                if (duration > p->wait_max_ms) {
                    p->wait_max_ms = duration;
                    atomic_store(&window_wait_max_ms, duration);
                }
            }
            return (int)n;
        }
        if (error)
            return error;
        if (eof)
            return 0;
        if (!waited) {
            waited = true;
            p->wait_count++;
            atomic_store(&window_wait_count, p->wait_count);
        }
        if ((uint32_t)(aster_os_uptime32() - waiting_since) >= AUDIO_PREFETCH_STALL_MS)
            return -ETIMEDOUT;
        aster_os_sleep_ms(1);
    }
}

void aster_audio_prefetch_stop(aster_audio_prefetch_t *p)
{
    if (!p)
        return;
    atomic_store(&p->stopping, true);
    if (p->worker.handle) {
        /* Never delete a task while it might own an SDMMC/FatFs handle.
         * If the OS unexpectedly cannot join, retain the buffers/handle
         * rather than freeing memory the producer might still reference. */
        int joined = aster_os_task_join(&p->worker, ASTER_OS_WAIT_FOREVER);
        if (joined) {
            atomic_store(&window_active, false);
            return; /* Fail closed: intentionally retain resources. */
        }
    }
    publish_empty();
    if (p->file.handle)
        (void)aster_os_file_close(&p->file);
    (void)aster_os_mutex_destroy(&p->lock);
    aster_os_free_external(p->scratch);
    aster_os_free_external(p->ring);
    aster_os_free_external(p);
}

bool aster_audio_prefetch_window(aster_audio_prefetch_window_t *out)
{
    if (!out)
        return false;
    out->active = atomic_load(&window_active);
    out->buffered_bytes = out->active ? atomic_load(&window_filled) : 0;
    out->capacity_bytes = out->active ? AUDIO_PREFETCH_RING_BYTES : 0;
    out->eof = out->active && atomic_load(&window_eof);
    out->io_error = out->active ? atomic_load(&window_error) : 0;
    out->wait_count = out->active ? atomic_load(&window_wait_count) : 0;
    out->wait_max_ms = out->active ? atomic_load(&window_wait_max_ms) : 0;
    return true;
}
