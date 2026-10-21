#ifndef CAST_TIMELINE_PREVIEW_H
#define CAST_TIMELINE_PREVIEW_H
#include "timeline_render.h"
typedef struct CastTimelinePreview CastTimelinePreview;
typedef struct {
    bool busy, ready;
    uint64_t revision, requested_generation, completed_generation, position_ns;
    char error[1024];
} CastTimelinePreviewSnapshot;
/* Immutable document/configuration; rejects active capture clips. Opening starts
 * a worker but performs no media IO on the caller. No audio device is opened. */
CastTimelinePreview *timeline_preview_open(const CastTimeline *, const Config *, char *, size_t);
/* Latest request wins. Generation is caller-owned and must increase strictly.
 * Superseded frames are never published. No decode or allocation on this path. */
int timeline_preview_request(CastTimelinePreview *, uint64_t position_ns, uint64_t generation);
void timeline_preview_snapshot(CastTimelinePreview *, CastTimelinePreviewSnapshot *);
/* Copies the latest published RGBA frame into caller-owned storage. Returns 1
 * when no frame exists, -1 on allocation failure, otherwise 0. Allocation/copy
 * takes place on the caller: use a UI/transport worker, never a capture callback. */
int timeline_preview_copy(CastTimelinePreview *, Frame *, uint64_t *generation);
/* Cancels pending/active decode and joins the worker. */
void timeline_preview_cancel(CastTimelinePreview *);
bool timeline_preview_finished(CastTimelinePreview *);
void timeline_preview_close(CastTimelinePreview *);
#endif
