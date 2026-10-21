#ifndef CAST_PROJECT_SESSION_H
#define CAST_PROJECT_SESSION_H
#include "timeline.h"
#include "timeline_export_status.h"
typedef struct CastProjectSession CastProjectSession;
struct App;
typedef struct {
    uint64_t id;
    char label[96];
    CastTimelineKind kind;
    bool missing;
} CastProjectMediaView;
typedef struct {
    bool open, busy, dirty, recording;
    bool exporting, reviewing, preview_busy, playing;
    char preview_error[1024];
    CastTimelineExportSnapshot export;
    uint64_t revision, duration_ns, playhead_ns;
    unsigned clip_count, media_count, missing_count;
    char path[4096], name[128], status[256], error[256];
    CastProjectMediaView media[CAST_TIMELINE_MEDIA_MAX];
    CastTimelineClip clips[CAST_TIMELINE_CLIP_MAX];
} CastProjectSnapshot;
CastProjectSession *project_session_open(void);
void project_session_close(CastProjectSession *);
/* Main-thread borrowed view; invalidated by tick/command/close. */
struct CastFrame;
const struct CastFrame *project_session_review_frame(CastProjectSession *);
void project_session_tick(struct App *);
void project_session_snapshot(CastProjectSession *, CastProjectSnapshot *);
int project_session_command(struct App *, int, char **, char *, size_t);
#endif
