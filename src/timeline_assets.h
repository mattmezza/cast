#ifndef CAST_TIMELINE_ASSETS_H
#define CAST_TIMELINE_ASSETS_H
#include "timeline.h"
#define CAST_ASSET_WIDTH 160
#define CAST_ASSET_HEIGHT 90
#define CAST_ASSET_BINS 256
#define CAST_ASSET_CACHE 16
#define CAST_ASSET_QUEUE 8
#define CAST_ASSET_SPAN_MAX_NS UINT64_C(30000000000)
typedef struct CastTimelineAssets CastTimelineAssets;
typedef struct {
    CastTimelineMedia media; /* Immutable finalized source; no capture clips. */
    uint64_t generation, source_start_ns, span_ns;
    bool thumbnail, waveform;
} CastTimelineAssetRequest;
typedef struct {
    uint64_t id, generation, media_id, source_start_ns, span_ns;
    bool thumbnail_ready, waveform_ready;
    unsigned bins; /* Envelope covers precisely source_start_ns..+span_ns. */
    unsigned char rgba[CAST_ASSET_WIDTH * CAST_ASSET_HEIGHT * 4];
    float peak[CAST_ASSET_BINS][2], rms[CAST_ASSET_BINS][2];
    char thumbnail_error[256], waveform_error[256];
} CastTimelineAssetResult;
typedef struct { uint64_t generation; unsigned pending, cached; bool busy; } CastTimelineAssetStatus;
/* Inert worker, bounded RAM only; never writes disk or opens audio/capture devices. */
CastTimelineAssets *timeline_assets_open(void);
/* Bounded copy/queue only, no filesystem/decode. 0 queued/cache hit, 1 queue full,
 * -1 invalid/stale. Returns a stable ID, including duplicate pending/cache hits.
 * Newer generation cancels old work and retires all old results. Caller must
 * advance it on edits/relinks/source replacement; finalized source contents are
 * immutable within a generation. Requests are limited to 30s visible windows,
 * not whole-clip waveforms. Worker work has a five-second cancellation deadline. */
int timeline_assets_request(CastTimelineAssets *, const CastTimelineAssetRequest *, uint64_t *id);
/* 0 copies complete/partial/error result; 1 not cached/pending/evicted. */
int timeline_assets_copy(CastTimelineAssets *, uint64_t generation, uint64_t id,
                         CastTimelineAssetResult *);
void timeline_assets_status(CastTimelineAssets *, CastTimelineAssetStatus *);
/* Caller generation must increase; no waiting for decode. */
void timeline_assets_cancel(CastTimelineAssets *, uint64_t generation);
/* Cancels and joins before freeing; may wait for interrupted decoder shutdown. */
void timeline_assets_close(CastTimelineAssets *);
#endif
