#ifndef CAST_TIMELINE_AUDIO_H
#define CAST_TIMELINE_AUDIO_H
#include "audio_monitor.h"
#include "timeline_render.h"
typedef struct CastTimelineAudio CastTimelineAudio;
typedef struct {
    bool requested, running, finished;
    uint64_t revision, generation;
    CastMonitorStatus output;
    char error[256];
} CastTimelineAudioStatus;
/* Immutable document; monitor ownership transfers on success. Inert until play.
 * Lifecycle, decoding and native output opening run on one owned worker. */
CastTimelineAudio *timeline_audio_open(const CastTimeline *, const Config *, CastAudioMonitor *);
CastTimelineAudio *timeline_audio_native_open(const CastTimeline *, const Config *);
/* Host calls are bounded. Explicit play is the only operation opening output.
 * anchor_ns is the monotonic instant corresponding to media position_ns. */
void timeline_audio_play(CastTimelineAudio *, uint64_t position_ns, uint64_t anchor_ns);
void timeline_audio_pause(CastTimelineAudio *);
void timeline_audio_status(CastTimelineAudio *, CastTimelineAudioStatus *);
/* Cancel clears queued PCM immediately; worker quiesces output asynchronously. */
void timeline_audio_cancel(CastTimelineAudio *);
bool timeline_audio_finished(CastTimelineAudio *);
void timeline_audio_close(CastTimelineAudio *);
#endif
