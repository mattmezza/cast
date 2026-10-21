#ifndef CAST_TIMELINE_H
#define CAST_TIMELINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAST_TIMELINE_MEDIA_MAX 64
#define CAST_TIMELINE_CLIP_MAX 128
#define CAST_TIMELINE_TRACK_MAX 16
#define CAST_TIMELINE_PATH_MAX 4096
#define CAST_TIMELINE_LIMIT_NS UINT64_C(604800000000000)
typedef enum {
    CAST_TIMELINE_VIDEO,
    CAST_TIMELINE_AUDIO,
    CAST_TIMELINE_IMAGE
} CastTimelineKind;
typedef struct {
    uint64_t id;
    char path[CAST_TIMELINE_PATH_MAX];
    uint64_t duration_ns;
    CastTimelineKind kind;
    bool missing;
} CastTimelineMedia;
typedef struct {
    uint64_t id, media_id, start_ns, source_in_ns, duration_ns;
    unsigned track;
    float gain;
    bool muted, capture_locked;
} CastTimelineClip;
typedef struct {
    CastTimelineMedia media[CAST_TIMELINE_MEDIA_MAX];
    CastTimelineClip clips[CAST_TIMELINE_CLIP_MAX];
    unsigned media_count, clip_count;
    uint64_t next_id, revision;
} CastTimeline;
/* History is heap-owned and bounded. The document itself contains no pointers. */
typedef struct CastTimelineHistory CastTimelineHistory;
void timeline_init(CastTimeline *);
int timeline_validate(const CastTimeline *, char *, size_t);
const CastTimelineMedia *timeline_media(const CastTimeline *, uint64_t);
const CastTimelineClip *timeline_clip(const CastTimeline *, uint64_t);
uint64_t timeline_duration(const CastTimeline *);
int timeline_add_media(CastTimeline *, const char *, CastTimelineKind, uint64_t,
                       uint64_t *, char *, size_t);
int timeline_add_clip(CastTimeline *, uint64_t media_id, unsigned track,
                      uint64_t start_ns, uint64_t source_in_ns, uint64_t duration_ns,
                      uint64_t *, char *, size_t);
int timeline_trim(CastTimeline *, uint64_t, uint64_t source_in_ns, uint64_t duration_ns,
                  char *, size_t);
int timeline_move(CastTimeline *, uint64_t, unsigned track, uint64_t start_ns, char *, size_t);
int timeline_split(CastTimeline *, uint64_t, uint64_t position_ns, uint64_t *, char *, size_t);
int timeline_remove(CastTimeline *, uint64_t, char *, size_t);
int timeline_gain(CastTimeline *, uint64_t, float, bool muted, char *, size_t);
/* Captured takes may grow while recording; ordinary editing cannot mutate them. */
int timeline_capture_update(CastTimeline *, uint64_t, uint64_t duration_ns, bool recording,
                            char *, size_t);
CastTimelineHistory *timeline_history_open(unsigned capacity);
void timeline_history_close(CastTimelineHistory *);
int timeline_checkpoint(CastTimelineHistory *, const CastTimeline *, char *, size_t);
int timeline_undo(CastTimelineHistory *, CastTimeline *, char *, size_t);
int timeline_redo(CastTimelineHistory *, CastTimeline *, char *, size_t);
#endif
