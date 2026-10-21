#ifndef CAST_ENCODER_CAPABILITIES_H
#define CAST_ENCODER_CAPABILITIES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum CastVideoEncoder {
    CAST_VIDEO_AUTO, CAST_VIDEO_OPENH264, CAST_VIDEO_X264, CAST_VIDEO_X265,
    CAST_VIDEO_VP9, CAST_VIDEO_AV1, CAST_VIDEO_ENCODER_COUNT
};
enum CastAudioEncoder {
    CAST_AUDIO_AAC, CAST_AUDIO_OPUS, CAST_AUDIO_PCM, CAST_AUDIO_FLAC,
    CAST_AUDIO_ENCODER_COUNT
};
#define CAST_ENCODER_BIT(index) (UINT32_C(1) << (index))
typedef struct {
    uint32_t video_available, video_allowed, audio_available, audio_allowed;
} CastEncoderCapabilities;
extern const char *const cast_video_encoder_names[CAST_VIDEO_ENCODER_COUNT];
extern const char *const cast_audio_encoder_names[CAST_AUDIO_ENCODER_COUNT];
/* Registered encoders only: not a guarantee that arbitrary dimensions/rate-control
 * settings initialize. No codec contexts, files or devices are opened. */
void encoder_capabilities_snapshot(CastEncoderCapabilities *);
/* Injectable registry for deterministic missing/wrong-media-type/profile tests. */
typedef bool (*CastEncoderLookup)(void *, const char *name, bool video);
void encoder_capabilities_query(CastEncoderCapabilities *, bool lgpl,
                                CastEncoderLookup, void *);
/* Checks named registered encoders, profile policy and actual muxer codec support.
 * Auto means the same reviewed CPU H.264 candidate order as media_video_open.
 * Returns zero when compatible, -1 with a bounded explanation otherwise. */
int encoder_capabilities_container(const char *container, const char *video,
                                   const char *audio, char *error, size_t);
#endif
