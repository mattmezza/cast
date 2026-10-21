#ifndef CAST_LIVE_MEDIA_STATUS_H
#define CAST_LIVE_MEDIA_STATUS_H
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    CAST_LIVE_IDLE, CAST_LIVE_STARTING, CAST_LIVE_PLAYING, CAST_LIVE_PAUSED,
    CAST_LIVE_ENDED, CAST_LIVE_FAILED
} CastLiveMediaState;
typedef struct {
    CastLiveMediaState state;
    uint64_t generation, audio_epoch, media_id;
    uint64_t source_in_ns, duration_ns, position_ns;
    uint64_t video_source_ns, video_monotonic_ns, dropped_audio_frames;
    bool has_video, has_audio;
    char error[256];
} CastLiveMediaStatus;
#endif
