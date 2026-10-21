#ifndef CAST_TIMELINE_RENDER_H
#define CAST_TIMELINE_RENDER_H
#include "cast.h"
#include "timeline.h"
#include "timeline_export_status.h"
typedef bool (*CastTimelineCancel)(void *);
typedef struct CastTimelineRenderer CastTimelineRenderer;
/* Thread confined, CPU-only, local regular inputs. Open validates every referenced
 * source; missing/invalid inputs fail with their media ID. No output/capture state.
 * Track order is bottom-to-top; visual clips contain-fit with alpha compositing.
 * Embedded video audio and audio clips mix stereo float at 48 kHz with gain/mute. */
CastTimelineRenderer *timeline_renderer_open(const CastTimeline *, const Config *,
                                             CastTimelineCancel, void *, char *, size_t);
void timeline_renderer_close(CastTimelineRenderer *);
int timeline_renderer_frame(CastTimelineRenderer *, uint64_t time_ns, Frame *, char *, size_t);
/* sample_position is on the document 48 kHz clock. Count must be 1..48000.
 * The caller owns interleaved stereo output (2*count floats). */
int timeline_renderer_audio(CastTimelineRenderer *, uint64_t sample_position, unsigned count,
                            float *, char *, size_t);
typedef struct CastTimelineExport CastTimelineExport;
/* Start copies the timeline/configuration and queues an owned worker. Inputs and
 * destination are opened there. Final output replaces an owned regular destination
 * only after trailer and fsync succeed; cancel/failure leaves it intact. */
CastTimelineExport *timeline_export_start(const CastTimeline *, const Config *, const char *,
                                          char *, size_t);
/* Optional application/private overlay; called on the export worker after the
 * montage frame is composed, before encoding. user must live until close returns.
 * Nonzero fails the export atomically. The callback must honor caller cancellation
 * through its own bounded work; no frame storage is retained by the exporter. */
typedef int (*CastTimelineFrameFilter)(void *user, uint64_t time_ns, Frame *, char *, size_t);
CastTimelineExport *timeline_export_start_filtered(const CastTimeline *, const Config *,
                                                   const char *, CastTimelineFrameFilter, void *,
                                                   char *, size_t);
/* Atomically publishes with link(2); an existing destination is never replaced. */
CastTimelineExport *timeline_export_start_noreplace(const CastTimeline *, const Config *,
                                                    const char *, char *, size_t);
void timeline_export_snapshot(CastTimelineExport *, CastTimelineExportSnapshot *);
void timeline_export_cancel(CastTimelineExport *);
/* Cancels if still running, joins, and frees. Call after snapshot COMPLETE to keep
 * a successful export. No external ffmpeg process or device is used. */
void timeline_export_close(CastTimelineExport *);
#endif
