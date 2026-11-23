/* SPDX-License-Identifier: MIT */
#ifndef ASTER_AUDIO_DECLICK_H
#define ASTER_AUDIO_DECLICK_H
#include <stdint.h>

/* Scale only the first fade_frames of a new native s32 stereo stream.
 * first_frame counts output-accepted PCM frames (not decoded frames).
 * Every sample at or after fade_frames remains bit-exact. This is a
 * bounded, allocation-free operation performed before DMA submission. */
static inline void aster_audio_fade_in_s32_stereo(int32_t *pcm, uint32_t frames,
                                                  uint64_t first_frame,
                                                  uint32_t fade_frames)
{
    if (!pcm || !frames || !fade_frames || first_frame >= fade_frames)
        return;
    uint32_t count = frames;
    if (count > fade_frames - (uint32_t)first_frame)
        count = fade_frames - (uint32_t)first_frame;
    for (uint32_t i = 0; i < count; ++i) {
        int64_t position = (int64_t)(first_frame + i);
        pcm[i * 2] = (int32_t)(((int64_t)pcm[i * 2] * position) / fade_frames);
        pcm[i * 2 + 1] = (int32_t)(((int64_t)pcm[i * 2 + 1] * position) / fade_frames);
    }
}

#endif
