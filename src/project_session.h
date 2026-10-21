#ifndef CAST_PROJECT_SESSION_H
#define CAST_PROJECT_SESSION_H
#include "timeline.h"
#include "timeline_export_status.h"
typedef struct CastProjectSession CastProjectSession;
struct App;
typedef struct {
    uint64_t id, duration_ns;
    char label[96];
    CastTimelineKind kind;
    bool missing;
} CastProjectMediaView;
#define CAST_PROJECT_ASSET_MAX 4
#define CAST_PROJECT_ASSET_WIDTH 80
#define CAST_PROJECT_ASSET_HEIGHT 45
#define CAST_PROJECT_ASSET_BINS 256
typedef struct {
    uint64_t clip_id, media_id, revision, generation, source_start_ns, span_ns;
    bool pending, thumbnail_ready, waveform_ready, missing;
    unsigned bins;
    unsigned char rgba[CAST_PROJECT_ASSET_WIDTH * CAST_PROJECT_ASSET_HEIGHT * 4];
    float peak[CAST_PROJECT_ASSET_BINS];
    char thumbnail_error[256], waveform_error[256];
} CastProjectAssetView;
typedef struct {
    bool open, busy, dirty, recording;
    bool exporting, reviewing, preview_busy, playing;
    char preview_error[1024];
    bool playback_audio_requested, playback_audio_running;
    char playback_audio_error[256];
    CastTimelineExportSnapshot export;
    uint64_t revision, duration_ns, playhead_ns;
    unsigned clip_count, media_count, missing_count;
    char path[4096], name[128], status[256], error[256];
    CastProjectMediaView media[CAST_TIMELINE_MEDIA_MAX];
    CastTimelineClip clips[CAST_TIMELINE_CLIP_MAX];
    unsigned asset_count;
    CastProjectAssetView assets[CAST_PROJECT_ASSET_MAX];
} CastProjectSnapshot;
CastProjectSession *project_session_open(void);
void project_session_close(CastProjectSession *);
/* Bounded immediate PCM epoch barrier; resuming requires explicit play. */
void project_session_silence(CastProjectSession *);
/* Memory-only immutable single-source document. duration=0 selects remaining
 * source, or five seconds for still images. Caller owns output storage. */
int project_session_live_source(CastProjectSession *, uint64_t media_id,
                                uint64_t source_in_ns, uint64_t duration_ns,
                                CastTimeline *out, char *error, size_t error_size);
/* Main-thread borrowed view; invalidated by tick/command/close. */
struct CastFrame;
const struct CastFrame *project_session_review_frame(CastProjectSession *);
void project_session_tick(struct App *);
void project_session_snapshot(CastProjectSession *, CastProjectSnapshot *);
/* Read-only assets window START_NS SPAN_NS selects a document-clock viewport.
 * Asset source_start/span describe actual bounded source-clock coverage (max30s
 * per clip), including panned offsets; no revision/history/dirty state changes. */
int project_session_command(struct App *, int, char **, char *, size_t);
#endif
