/* SPDX-License-Identifier: MIT */
#include "aster_audio_decoder.h"
#include "aster_audio_output.h"
#include "aster_os.h"
#ifdef ASTER_S31_AUDIO_PREFETCH
#include "aster_audio_prefetch.h"
#endif
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stddef.h>
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD
#define DR_MP3_ONLY_MP3
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DECODER_BUDGET (768u * 1024u)

enum { MP3_POINTS = 768, MP3_PREROLL = DRMP3_SEEK_LEADING_MP3_FRAMES };
_Static_assert(MP3_PREROLL > 0 && !(MP3_PREROLL & (MP3_PREROLL - 1)),
               "MP3 index stride must be a power of two");
typedef struct {
    uint64_t offsets[MP3_POINTS];
    uint64_t preceding[MP3_PREROLL];
    uint64_t next_header, end, pcm, frame_number, reader_offset;
    uint32_t count, samples_per_frame, header_bytes;
    uint64_t stride;
    uint8_t header[4];
    bool finished;
    aster_os_file_t reader;
    uint8_t block[16384];
} mp3_index_t;

struct aster_audio_decoder {
    aster_os_file_t file;
    uint64_t length, cursor, decoded;
    int io_error;
    bool (*cancel)(void *);
    void *context;
    aster_audio_decoder_info_t info;
    drwav *wav;
    drflac *flac;
    drmp3 *mp3;
    mp3_index_t *index;
#ifdef ASTER_S31_AUDIO_PREFETCH
    /* FLAC only: decoder and ring producer never share a file handle. */
    aster_audio_prefetch_t *prefetch;
    char prefetch_path[256];
    bool prefetch_disabled;
#endif

    /* Newlib's int32_t can be long while dr_libs uses int. Do not alias the
  * caller's int32_t buffer through an incompatible int pointer. */
    union {
        drflac_int32 flac[1024];
        drwav_int32 wav[1024];
        drmp3_int16 mp3[1024];
    } samples;
};

typedef union {
    max_align_t alignment;
    size_t bytes;
} allocation_header;

static void *decoder_alloc(size_t bytes, void *ctx)
{
    aster_audio_decoder_t *d = ctx;
    if (bytes > DECODER_BUDGET - d->info.allocated ||
        bytes > SIZE_MAX - sizeof(allocation_header)) {
        d->io_error = -ENOMEM;
        return NULL;
    }
    allocation_header *h = aster_os_alloc_external(bytes + sizeof(*h));
    if (!h) {
        d->io_error = -ENOMEM;
        return NULL;
    }
    h->bytes = bytes;
    d->info.allocated += bytes;
    if (d->info.allocated > d->info.peak_allocated)
        d->info.peak_allocated = d->info.allocated;
    return h + 1;
}

static void decoder_free(void *p, void *ctx)
{
    if (!p)
        return;
    aster_audio_decoder_t *d = ctx;
    allocation_header *h = (allocation_header *)p - 1;
    d->info.allocated -= h->bytes;
    aster_os_free_external(h);
}

static void *decoder_realloc(void *p, size_t bytes, void *ctx)
{
    if (!p)
        return decoder_alloc(bytes, ctx);
    if (!bytes) {
        decoder_free(p, ctx);
        return NULL;
    }
    allocation_header *h = (allocation_header *)p - 1;
    void *next = decoder_alloc(bytes, ctx);
    if (!next)
        return NULL;
    memcpy(next, p, bytes < h->bytes ? bytes : h->bytes);
    decoder_free(p, ctx);
    return next;
}

static bool cancelled(aster_audio_decoder_t *d)
{
    if (d->cancel && d->cancel(d->context)) {
        d->io_error = -ECANCELED;
        return true;
    }
    return false;
}

/* Consume only contiguous MPEG Layer III headers. Each offset comes from the
 * actual frame size, including VBR and padding. A gap or unsupported frame
 * leaves the remaining stream to dr_mp3's normal seek path. */
static void index_headers(aster_audio_decoder_t *d, uint64_t offset, const uint8_t *data, size_t n)
{
    mp3_index_t *idx = d->index;
    while (!idx->finished) {
        uint64_t at = idx->next_header + idx->header_bytes;
        if (at < offset || at - offset >= n)
            break;
        size_t pos = (size_t)(at - offset);
        size_t take = sizeof(idx->header) - idx->header_bytes;
        if (take > n - pos)
            take = n - pos;
        memcpy(idx->header + idx->header_bytes, data + pos, take);
        idx->header_bytes += take;
        if (idx->header_bytes < sizeof(idx->header))
            break;

        const uint8_t *h = idx->header;
        if (!drmp3_hdr_valid(h) || DRMP3_HDR_GET_LAYER(h) != 1 || DRMP3_HDR_IS_FREE_FORMAT(h) ||
            drmp3_hdr_sample_rate_hz(h) != d->info.rate) {
            idx->finished = true;
            break;
        }
        unsigned bytes = drmp3_hdr_frame_bytes(h, 0) + drmp3_hdr_padding(h);
        unsigned samples = drmp3_hdr_frame_samples(h);
        if (bytes > idx->end - idx->next_header ||
            (idx->samples_per_frame && samples != idx->samples_per_frame)) {
            idx->finished = true;
            break;
        }
        uint64_t frame = idx->frame_number;
        unsigned slot = frame % MP3_PREROLL;
        if (frame && !(frame & (idx->stride - 1))) {
            if (idx->count == MP3_POINTS) {
                /* Keep even multiples of the old stride on long recordings. */
                for (unsigned i = 0; i < MP3_POINTS / 2; i++)
                    idx->offsets[i] = idx->offsets[2 * i + 1];
                idx->count /= 2;
                idx->stride *= 2;
            }
            if (!(frame & (idx->stride - 1)))
                idx->offsets[idx->count++] = idx->preceding[slot];
        }
        idx->preceding[slot] = idx->next_header;
        idx->frame_number++;
        idx->samples_per_frame = samples;
        idx->pcm += samples;
        idx->next_header += bytes;
        idx->header_bytes = 0;
        idx->finished = idx->end - idx->next_header < sizeof(idx->header);
    }
}

static size_t read_file(void *ctx, void *buffer, size_t bytes)
{
    aster_audio_decoder_t *d = ctx;
    if (d->io_error || cancelled(d))
        return 0;
    if (bytes > d->length - d->cursor)
        bytes = (size_t)(d->length - d->cursor);
    if (bytes > INT_MAX)
        bytes = INT_MAX;
    size_t done = 0;
    while (done < bytes) {
        if (cancelled(d))
            break;
#ifdef ASTER_S31_AUDIO_PREFETCH
        int n = d->prefetch ?
            aster_audio_prefetch_read(d->prefetch, (uint8_t *)buffer + done,
                                      bytes - done, d->cancel, d->context) :
            aster_os_file_read(&d->file, (uint8_t *)buffer + done, bytes - done);
#else
        int n = aster_os_file_read(&d->file, (uint8_t *)buffer + done, bytes - done);
#endif
        if (n < 0) {
            d->io_error = n;
            break;
        }
        if (!n) {
            d->io_error = -EIO;
            break;
        }
        if ((size_t)n > bytes - done) {
            d->io_error = -EIO;
            break;
        }
        if (d->index && !d->index->finished)
            index_headers(d, d->cursor, (const uint8_t *)buffer + done, (size_t)n);
        done += (size_t)n;
        d->cursor += (unsigned)n;
    }
    return done;
}

static int seek_file(aster_audio_decoder_t *d, int offset, unsigned origin)
{
    if (cancelled(d))
        return 0;
    if (origin > 2)
        return 0;
    uint64_t base = origin == 0 ? 0 : origin == 1 ? d->cursor : d->length;
    /* FLAC binary seeking probes beyond EOF while refining an estimate. This
  * is an unsuccessful probe, not a sticky I/O failure; it may recover by
  * trying an earlier offset. Actual filesystem errors remain terminal. */
    uint64_t at;
    if (offset < 0) {
        uint64_t distance = (uint64_t)(-(int64_t)offset);
        if (distance > base)
            return 0;
        at = base - distance;
    } else {
        if ((uint64_t)offset > d->length - base)
            return 0;
        at = base + (unsigned)offset;
    }
#ifdef ASTER_S31_AUDIO_PREFETCH
    /* dr_flac may seek during a read: stop/join the sequential producer
     * before seeking the decoder's independent random-access file. */
    if (d->prefetch) {
        aster_audio_prefetch_stop(d->prefetch);
        d->prefetch = NULL;
    }
#endif
    int r = aster_os_file_seek(&d->file, (int64_t)at, ASTER_OS_SEEK_SET);
    if (r) {
        d->io_error = r;
        return 0;
    }
    d->cursor = at;
    return 1;
}

static drwav_bool32 wav_seek(void *ctx, int offset, drwav_seek_origin origin)
{
    return seek_file(ctx, offset, (unsigned)origin);
}

static drflac_bool32 flac_seek(void *ctx, int offset, drflac_seek_origin origin)
{
    return seek_file(ctx, offset, (unsigned)origin);
}

static drwav_bool32 wav_tell(void *ctx, drwav_int64 *cursor)
{
    aster_audio_decoder_t *d = ctx;
    *cursor = (drwav_int64)d->cursor;
    return 1;
}

static drflac_bool32 flac_tell(void *ctx, drflac_int64 *cursor)
{
    aster_audio_decoder_t *d = ctx;
    *cursor = (drflac_int64)d->cursor;
    return 1;
}

static drmp3_bool32 mp3_seek(void *ctx, int offset, drmp3_seek_origin origin)
{
    return seek_file(ctx, offset, (unsigned)origin);
}

static drmp3_bool32 mp3_tell(void *ctx, drmp3_int64 *cursor)
{
    aster_audio_decoder_t *d = ctx;
    *cursor = (drmp3_int64)d->cursor;
    return 1;
}

static bool mp3_path(const char *path)
{
    size_t length = strlen(path);
    if (length < 4)
        return false;
    const char *ext = path + length - 4;
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'p' || ext[2] == 'P') &&
           ext[3] == '3';
}

/* During playback, read at most one block ahead per PCM block. On a cold
 * seek, extend only as far as the requested frame. The second cursor never
 * disturbs dr_mp3's buffered audio or holds a file open after indexing. */
static void extend_index(aster_audio_decoder_t *d, uint64_t target, bool one_block)
{
    mp3_index_t *idx = d->index;
    if (!idx || !idx->reader.handle)
        return;
    while (!idx->finished && idx->pcm <= target && !cancelled(d)) {
        uint64_t at = idx->next_header + idx->header_bytes;
        if (at < idx->reader_offset || at - idx->reader_offset >= sizeof(idx->block)) {
            if (aster_os_file_seek(&idx->reader, (int64_t)at, ASTER_OS_SEEK_SET))
                break;
            idx->reader_offset = at;
        }
        uint64_t left = idx->end - idx->reader_offset;
        size_t n = left < sizeof(idx->block) ? (size_t)left : sizeof(idx->block);
        int got = aster_os_file_read(&idx->reader, idx->block, n);
        if (got <= 0 || (size_t)got > n)
            break;
        index_headers(d, idx->reader_offset, idx->block, (size_t)got);
        idx->reader_offset += (unsigned)got;
        if (one_block && !idx->finished)
            return;
    }
    if (idx->finished || idx->pcm <= target) {
        aster_os_file_close(&idx->reader);
        idx->reader.handle = NULL;
    }
}

static int open_mp3(aster_audio_decoder_t *d, const char *path)
{
    drmp3_allocation_callbacks alloc = {d, decoder_alloc, decoder_realloc, decoder_free};
    drmp3 *mp3 = decoder_alloc(sizeof(*mp3), d);
    if (!mp3)
        return -ENOMEM;
    if (!drmp3_init(mp3, read_file, mp3_seek, mp3_tell, NULL, d, &alloc)) {
        decoder_free(mp3, d);
        return d->io_error ? d->io_error : -EINVAL;
    }
    d->mp3 = mp3;
    d->info.rate = mp3->sampleRate;
    d->info.channels = mp3->channels;
    d->info.bits = 16;
    d->info.codec = ASTER_AUDIO_CODEC_MP3;
    if (!aster_audio_rate_bit(d->info.rate) || !d->info.channels || d->info.channels > 2)
        return -ENOTSUP;
    /* Untagged streams already traverse the file to count samples. Collect
     * the index on those reads. Tagged streams keep their short open path. */
    if (!d->io_error) {
        d->index = decoder_alloc(sizeof(*d->index), d);
        if (d->index) {
            memset(d->index, 0, sizeof(*d->index));
            d->index->stride = MP3_PREROLL;
            d->index->end = mp3->streamLength < d->length ? mp3->streamLength : d->length;
            d->index->next_header = mp3->streamStartOffset;
            d->index->finished = d->index->next_header >= d->index->end;
        } else {
            d->io_error = 0; /* The index is optional under memory pressure. */
        }
    }
    d->info.frames = drmp3_get_pcm_frame_count(mp3);
    if (d->io_error)
        return d->io_error;
    if (d->index && !d->index->finished)
        (void)aster_os_file_open(&d->index->reader, path, ASTER_OS_READ);
    return d->info.frames ? 0 : -EINVAL;
}

int aster_audio_decoder_close(aster_audio_decoder_t *d)
{
    if (!d)
        return 0;
#ifdef ASTER_S31_AUDIO_PREFETCH
    /* Producer must terminate before any related file/memory is released. */
    if (d->prefetch) {
        aster_audio_prefetch_stop(d->prefetch);
        d->prefetch = NULL;
    }
#endif
    if (d->flac)
        drflac_close(d->flac);
    if (d->wav) {
        drwav_uninit(d->wav);
        decoder_free(d->wav, d);
    }
    if (d->mp3) {
        drmp3_uninit(d->mp3);
        decoder_free(d->mp3, d);
    }
    if (d->index) {
        if (d->index->reader.handle)
            aster_os_file_close(&d->index->reader);
        decoder_free(d->index, d);
    }
    int r = d->file.handle ? aster_os_file_close(&d->file) : 0;
    aster_os_free_external(d);
    return r;
}

int aster_audio_decoder_open(const char *path, bool (*cancel)(void *), void *ctx,
                             aster_audio_decoder_t **out)
{
    if (!path || !out)
        return -EINVAL;
    aster_os_entry_t entry;
    int r = aster_os_stat(path, &entry);
    if (r)
        return r;
    if (entry.type != ASTER_OS_FILE || entry.size < 12 || entry.size > INT64_MAX)
        return -EINVAL;
    aster_audio_decoder_t *d = aster_os_alloc_external(sizeof(*d));
    if (!d)
        return -ENOMEM;
    memset(d, 0, sizeof(*d));
    d->length = entry.size;
    d->cancel = cancel;
    d->context = ctx;
    r = aster_os_file_open(&d->file, path, ASTER_OS_READ);
    if (r) {
        aster_audio_decoder_close(d);
        return r;
    }
    uint8_t magic[12];
    if (read_file(d, magic, sizeof(magic)) != sizeof(magic)) {
        r = d->io_error ? d->io_error : -EIO;
        goto fail;
    }
    if (!seek_file(d, 0, 0)) {
        r = d->io_error;
        goto fail;
    }
    if (!memcmp(magic, "fLaC", 4)) {
        drflac_allocation_callbacks alloc = {d, decoder_alloc, decoder_realloc, decoder_free};
        d->flac = drflac_open(read_file, flac_seek, flac_tell, d, &alloc);
        if (!d->flac) {
            r = d->io_error ? d->io_error : -EINVAL;
            goto fail;
        }
        d->info.rate = d->flac->sampleRate;
        d->info.channels = d->flac->channels;
        d->info.bits = d->flac->bitsPerSample;
        d->info.frames = d->flac->totalPCMFrameCount;
        d->info.codec = ASTER_AUDIO_CODEC_FLAC;
    } else if (!memcmp(magic, "RIFF", 4) && !memcmp(magic + 8, "WAVE", 4)) {
        uint64_t riff = (uint32_t)magic[4] | ((uint32_t)magic[5] << 8) |
                        ((uint32_t)magic[6] << 16) | ((uint32_t)magic[7] << 24);
        if (riff + 8 > d->length) {
            r = -EIO;
            goto fail;
        }
        drwav_allocation_callbacks alloc = {d, decoder_alloc, decoder_realloc, decoder_free};
        drwav *wav = decoder_alloc(sizeof(*wav), d);
        if (!wav) {
            r = -ENOMEM;
            goto fail;
        }
        if (!drwav_init(wav, read_file, wav_seek, wav_tell, d, &alloc)) {
            decoder_free(wav, d);
            r = d->io_error ? d->io_error : -EINVAL;
            goto fail;
        }
        d->wav = wav;
        if (wav->translatedFormatTag != DR_WAVE_FORMAT_PCM) {
            r = -ENOTSUP;
            goto fail;
        }
        d->info.rate = wav->sampleRate;
        d->info.channels = wav->channels;
        d->info.bits = wav->bitsPerSample;
        d->info.frames = wav->totalPCMFrameCount;
        d->info.codec = ASTER_AUDIO_CODEC_WAV;
        if (wav->fmt.formatTag == DR_WAVE_FORMAT_EXTENSIBLE && wav->fmt.validBitsPerSample)
            d->info.bits = wav->fmt.validBitsPerSample;
        if (wav->dataChunkDataPos > d->length ||
            wav->dataChunkDataSize > d->length - wav->dataChunkDataPos ||
            wav->fmt.blockAlign != wav->channels * (wav->bitsPerSample / 8) ||
            !wav->fmt.blockAlign || wav->dataChunkDataSize % wav->fmt.blockAlign) {
            r = -EIO;
            goto fail;
        }
    } else if (mp3_path(path)) {
        r = open_mp3(d, path);
        if (r)
            goto fail;
    } else {
        r = -ENOTSUP;
        goto fail;
    }
    if (d->io_error) {
        r = d->io_error;
        goto fail;
    }
    if (!aster_audio_rate_bit(d->info.rate) || !d->info.channels || d->info.channels > 2 ||
        (d->info.bits != 16 && d->info.bits != 24 && d->info.bits != 32)) {
        r = -ENOTSUP;
        goto fail;
    }
#ifdef ASTER_S31_AUDIO_PREFETCH
    /* Stage H1 affects only FLAC. WAV and MP3/VBR indexing retain the
     * established direct-read path until their buffering policy is ready. */
    if (d->flac && strlen(path) < sizeof(d->prefetch_path)) {
        memcpy(d->prefetch_path, path, strlen(path) + 1);
        d->prefetch = aster_audio_prefetch_start(d->prefetch_path, d->cursor, d->length);
        if (!d->prefetch)
            d->prefetch_disabled = true; /* Original direct-read fallback. */
    }
#endif
    *out = d;
    return 0;
fail:
    aster_audio_decoder_close(d);
    return r;
}

void aster_audio_decoder_info(aster_audio_decoder_t *d, aster_audio_decoder_info_t *out)
{
    if (d && out)
        *out = d->info;
}

int aster_audio_decoder_read(aster_audio_decoder_t *d, int32_t *pcm, uint32_t frames)
{
    if (!d || !pcm || !frames || frames > 512)
        return -EINVAL;
    if (cancelled(d))
        return -ECANCELED;
    if (d->io_error)
        return d->io_error;
#ifdef ASTER_S31_AUDIO_PREFETCH
    if (d->flac && !d->prefetch && !d->prefetch_disabled && d->prefetch_path[0]) {
        /* Restart at the decoder cursor after any FLAC seek probe. */
        d->prefetch = aster_audio_prefetch_start(d->prefetch_path, d->cursor, d->length);
        if (!d->prefetch)
            d->prefetch_disabled = true;
    }
#endif
    uint64_t n;
    if (d->mp3) {
        if (d->decoded >= 2048)
            extend_index(d, UINT64_MAX, true);
        n = drmp3_read_pcm_frames_s16(d->mp3, frames, d->samples.mp3);
    } else if (d->flac)
        n = drflac_read_pcm_frames_s32(d->flac, frames, d->samples.flac);
    else
        n = drwav_read_pcm_frames_s32(d->wav, frames, d->samples.wav);
    if (d->io_error)
        return d->io_error;
    for (size_t i = 0; i < (size_t)n * d->info.channels; i++) {
        if (d->mp3)
            pcm[i] = (int32_t)d->samples.mp3[i] * 65536;
        else if (d->flac)
            pcm[i] = d->samples.flac[i];
        else
            pcm[i] = d->samples.wav[i];
    }
    d->decoded += n;
    if (!n && d->info.frames && d->decoded < d->info.frames)
        return -EIO;
    return (int)n;
}

int aster_audio_decoder_seek(aster_audio_decoder_t *d, uint64_t frame)
{
    if (!d || (d->info.frames && frame > d->info.frames))
        return -EINVAL;
    if (cancelled(d))
        return -ECANCELED;
    d->io_error = 0;
    /* All indexed headers have the same samples per frame. Store just the
     * byte offset and reconstruct the nearest dr_mp3 point for this seek. */
    drmp3_seek_point point;
    if (d->index && frame) {
        mp3_index_t *idx = d->index;
        uint64_t raw = frame + d->mp3->delayInPCMFrames;
        extend_index(d, raw, false);
        if (idx->count) {
            uint64_t interval = idx->stride * idx->samples_per_frame;
            uint64_t i = raw / interval;
            if (i > idx->count)
                i = idx->count;
            if (i) {
                point = (drmp3_seek_point){.seekPosInBytes = idx->offsets[i - 1],
                                           .pcmFrameIndex = i * interval,
                                           .mp3FramesToDiscard = MP3_PREROLL};
                drmp3_bind_seek_table(d->mp3, 1, &point);
            }
        }
    }
    bool ok = d->mp3    ? drmp3_seek_to_pcm_frame(d->mp3, frame)
              : d->flac ? drflac_seek_to_pcm_frame(d->flac, frame)
                        : drwav_seek_to_pcm_frame(d->wav, frame);
    if (d->mp3)
        drmp3_bind_seek_table(d->mp3, 0, NULL);
    if (d->io_error)
        return d->io_error;
    if (!ok)
        return -EIO;
    d->decoded = frame;
    return 0;
}
