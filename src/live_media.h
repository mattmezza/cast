#ifndef CAST_LIVE_MEDIA_H
#define CAST_LIVE_MEDIA_H
#include "cast.h"
#include "timeline.h"
#define CAST_LIVE_AUDIO_FRAMES 96000u
#define CAST_LIVE_AUDIO_RATE 48000u
typedef struct CastLiveMedia CastLiveMedia;
#include "live_media_status.h"
/* Inert persistent worker. No capture/native audio output is ever opened. */
CastLiveMedia *live_media_open(void);
/* Injectable monotonic clock for deterministic, device-free integration tests. */
CastLiveMedia *live_media_open_with_clock(uint64_t (*now_ns)(void *), void *);
/* Explicit finalized local source. Owns copies; IO/decode/validation on worker.
 * Times are nanoseconds. Generation must strictly increase for every lifecycle
 * request. Source image duration may be zero; requested duration must be bounded.
 * STARTING prepares first video/100ms PCM before PLAYING; effective anchor is
 * max(requested anchor, readiness clock), preserving the first frame/syllables.
 * Returns 0 queued, -1 invalid/stale. No automatic retry or playback restart. */
int live_media_play(CastLiveMedia *, const CastTimelineMedia *, const Config *,
                    uint64_t source_in_ns, uint64_t duration_ns, uint64_t anchor_ns,
                    uint64_t generation);
int live_media_pause(CastLiveMedia *, uint64_t now_ns, uint64_t generation);
int live_media_resume(CastLiveMedia *, uint64_t anchor_ns, uint64_t generation);
int live_media_stop(CastLiveMedia *, uint64_t generation);
void live_media_status(CastLiveMedia *, uint64_t now_ns, CastLiveMediaStatus *);
/* Nonblocking trylock, no allocation or IO. Full-canvas contain-fit RGBA replaces
 * already allocated, matching-dimension destination. Destination output-clock
 * ts_ns is preserved; source timestamps are in status. 0 copied, 1 unavailable.
 * Paused holds its visual; playing frames older than 250ms are unavailable.
 * ENDED/stop restore the caller's base stage by returning 1. */
int live_media_video(CastLiveMedia *, uint64_t now_ns, Frame *destination);
/* Nonblocking additive stereo 48k float mix into caller's existing device mix.
 * 0 all frames covered, 1 partial/unavailable/contention. Missing frames remain
 * unchanged. Caller applies output privacy gates after mixing. ENDED retains
 * at most two seconds of PCM for timestamped recorder backlog; pause/stop/seek
 * retire it immediately. Never routes desktop/mic or opens a playback device. */
int live_media_audio_mix(CastLiveMedia *, uint64_t start_ns, float *, unsigned frames);
/* Cancels and joins. Must precede freeing any host dependencies. */
void live_media_close(CastLiveMedia *);
#endif
