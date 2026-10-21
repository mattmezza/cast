#include "timeline.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *e, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (n) vsnprintf(e, n, fmt, ap);
    va_end(ap);
    return -1;
}
static bool span(uint64_t start, uint64_t duration)
{
    return duration && start < CAST_TIMELINE_LIMIT_NS &&
           duration <= CAST_TIMELINE_LIMIT_NS - start;
}
void timeline_init(CastTimeline *t)
{
    memset(t, 0, sizeof *t);
    t->next_id = 1;
}
const CastTimelineMedia *timeline_media(const CastTimeline *t, uint64_t id)
{
    for (unsigned i = 0; i < t->media_count; ++i)
        if (t->media[i].id == id) return &t->media[i];
    return NULL;
}
const CastTimelineClip *timeline_clip(const CastTimeline *t, uint64_t id)
{
    for (unsigned i = 0; i < t->clip_count; ++i)
        if (t->clips[i].id == id) return &t->clips[i];
    return NULL;
}
static bool collision(const CastTimeline *t, const CastTimelineClip *c, uint64_t ignore)
{
    for (unsigned i = 0; i < t->clip_count; ++i) {
        const CastTimelineClip *other = &t->clips[i];
        if (other->id != ignore && other->track == c->track &&
            c->start_ns < other->start_ns + other->duration_ns &&
            other->start_ns < c->start_ns + c->duration_ns) return true;
    }
    return false;
}
static int valid_clip(const CastTimeline *t, const CastTimelineClip *c, uint64_t ignore,
                      char *e, size_t n)
{
    const CastTimelineMedia *m = timeline_media(t, c->media_id);
    if (!m || c->track >= CAST_TIMELINE_TRACK_MAX || !span(c->start_ns, c->duration_ns) ||
        !span(c->source_in_ns, c->duration_ns) || !isfinite(c->gain) ||
        c->gain < 0 || c->gain > 4)
        return fail(e, n, "invalid clip source, track, time or gain");
    if (m->kind != CAST_TIMELINE_IMAGE && m->duration_ns &&
        c->source_in_ns + c->duration_ns > m->duration_ns)
        return fail(e, n, "clip exceeds source duration");
    if (collision(t, c, ignore)) return fail(e, n, "clips on the same track cannot overlap");
    return 0;
}
int timeline_validate(const CastTimeline *t, char *e, size_t n)
{
    if (!t || t->media_count > CAST_TIMELINE_MEDIA_MAX ||
        t->clip_count > CAST_TIMELINE_CLIP_MAX || !t->next_id || t->next_id == UINT64_MAX ||
        t->revision == UINT64_MAX)
        return fail(e, n, "invalid timeline bounds or identity");
    for (unsigned i = 0; i < t->media_count; ++i) {
        const CastTimelineMedia *m = &t->media[i];
        if (!m->id || m->id >= t->next_id || !m->path[0] ||
            !memchr(m->path, 0, sizeof m->path) || m->kind > CAST_TIMELINE_IMAGE ||
            m->kind < CAST_TIMELINE_VIDEO || m->duration_ns > CAST_TIMELINE_LIMIT_NS)
            return fail(e, n, "invalid timeline media reference");
        for (unsigned j = 0; j < i; ++j)
            if (t->media[j].id == m->id) return fail(e, n, "duplicate media identifier");
    }
    for (unsigned i = 0; i < t->clip_count; ++i) {
        const CastTimelineClip *c = &t->clips[i];
        if (!c->id || c->id >= t->next_id || timeline_media(t, c->id))
            return fail(e, n, "invalid clip identifier");
        for (unsigned j = 0; j < i; ++j)
            if (t->clips[j].id == c->id) return fail(e, n, "duplicate clip identifier");
        if (valid_clip(t, c, c->id, e, n)) return -1;
    }
    return 0;
}
uint64_t timeline_duration(const CastTimeline *t)
{
    uint64_t end = 0;
    for (unsigned i = 0; i < t->clip_count; ++i) {
        uint64_t next = t->clips[i].start_ns + t->clips[i].duration_ns;
        if (next > end) end = next;
    }
    return end;
}
static int editable(CastTimeline *t, uint64_t id, CastTimelineClip **clip, char *e, size_t n)
{
    if (timeline_validate(t, e, n)) return -1;
    if (t->revision >= UINT64_MAX - 1) return fail(e,n,"timeline revision capacity reached");
    for (unsigned i = 0; i < t->clip_count; ++i)
        if (t->clips[i].id == id) {
            if (t->clips[i].capture_locked)
                return fail(e, n, "the active recording clip cannot be edited");
            *clip = &t->clips[i];
            return 0;
        }
    return fail(e, n, "clip not found");
}
int timeline_add_media(CastTimeline *t, const char *path, CastTimelineKind kind,
                       uint64_t duration, uint64_t *id, char *e, size_t n)
{
    if (timeline_validate(t, e, n)) return -1;
    if (t->revision >= UINT64_MAX - 1) return fail(e,n,"timeline revision capacity reached");
    if (!path || !path[0] || strlen(path) >= CAST_TIMELINE_PATH_MAX ||
        kind < CAST_TIMELINE_VIDEO || kind > CAST_TIMELINE_IMAGE ||
        duration > CAST_TIMELINE_LIMIT_NS || t->media_count == CAST_TIMELINE_MEDIA_MAX ||
        t->next_id >= UINT64_MAX - 1 || t->revision >= UINT64_MAX - 1)
        return fail(e, n, "invalid media or timeline capacity reached");
    CastTimelineMedia *m = &t->media[t->media_count++];
    *m = (CastTimelineMedia){.id = t->next_id++, .kind = kind, .duration_ns = duration};
    snprintf(m->path, sizeof m->path, "%s", path);
    ++t->revision;
    if (id) *id = m->id;
    return 0;
}
int timeline_add_clip(CastTimeline *t, uint64_t media_id, unsigned track,
                      uint64_t start, uint64_t source, uint64_t duration,
                      uint64_t *id, char *e, size_t n)
{
    if (timeline_validate(t, e, n)) return -1;
    if (t->revision >= UINT64_MAX - 1) return fail(e,n,"timeline revision capacity reached");
    if (t->clip_count == CAST_TIMELINE_CLIP_MAX || t->next_id >= UINT64_MAX - 1 ||
        t->revision >= UINT64_MAX - 1) return fail(e, n, "timeline capacity reached");
    CastTimelineClip c = {.id = t->next_id, .media_id = media_id, .track = track,
                         .start_ns = start, .source_in_ns = source, .duration_ns = duration,
                         .gain = 1};
    if (valid_clip(t, &c, 0, e, n)) return -1;
    t->clips[t->clip_count++] = c;
    ++t->next_id;
    ++t->revision;
    if (id) *id = c.id;
    return 0;
}
int timeline_trim(CastTimeline *t, uint64_t id, uint64_t source, uint64_t duration,
                  char *e, size_t n)
{
    CastTimelineClip *c;
    if (editable(t, id, &c, e, n)) return -1;
    CastTimelineClip candidate = *c;
    candidate.source_in_ns = source;
    candidate.duration_ns = duration;
    if (valid_clip(t, &candidate, id, e, n)) return -1;
    *c = candidate;
    ++t->revision;
    return 0;
}
int timeline_move(CastTimeline *t, uint64_t id, unsigned track, uint64_t start,
                  char *e, size_t n)
{
    CastTimelineClip *c;
    if (editable(t, id, &c, e, n)) return -1;
    CastTimelineClip candidate = *c;
    candidate.track = track;
    candidate.start_ns = start;
    if (valid_clip(t, &candidate, id, e, n)) return -1;
    *c = candidate;
    ++t->revision;
    return 0;
}
int timeline_split(CastTimeline *t, uint64_t id, uint64_t position, uint64_t *new_id,
                   char *e, size_t n)
{
    CastTimelineClip *c;
    if (editable(t, id, &c, e, n)) return -1;
    if (t->clip_count == CAST_TIMELINE_CLIP_MAX || t->next_id >= UINT64_MAX - 1 ||
        position <= c->start_ns || position >= c->start_ns + c->duration_ns)
        return fail(e, n, "split must fall inside an editable clip");
    uint64_t offset = position - c->start_ns;
    CastTimelineClip right = *c;
    right.id = t->next_id++;
    right.start_ns = position;
    right.source_in_ns += offset;
    right.duration_ns -= offset;
    c->duration_ns = offset;
    t->clips[t->clip_count++] = right;
    ++t->revision;
    if (new_id) *new_id = right.id;
    return 0;
}
int timeline_remove(CastTimeline *t, uint64_t id, char *e, size_t n)
{
    CastTimelineClip *c;
    if (editable(t, id, &c, e, n)) return -1;
    size_t index = (size_t)(c - t->clips);
    memmove(c, c + 1, (t->clip_count - index - 1) * sizeof *c);
    memset(&t->clips[--t->clip_count], 0, sizeof *c);
    ++t->revision;
    return 0;
}
int timeline_gain(CastTimeline *t, uint64_t id, float gain, bool muted, char *e, size_t n)
{
    CastTimelineClip *c;
    if (editable(t, id, &c, e, n)) return -1;
    if (!isfinite(gain) || gain < 0 || gain > 4) return fail(e, n, "gain must be between 0 and 4");
    c->gain = gain;
    c->muted = muted;
    ++t->revision;
    return 0;
}
int timeline_capture_update(CastTimeline *t, uint64_t id, uint64_t duration, bool recording,
                            char *e, size_t n)
{
    if (timeline_validate(t, e, n)) return -1;
    if (t->revision >= UINT64_MAX - 1) return fail(e,n,"timeline revision capacity reached");
    CastTimelineClip *c = (CastTimelineClip *)timeline_clip(t, id);
    if (!c || c->source_in_ns || duration < c->duration_ns || !span(c->start_ns, duration))
        return fail(e, n, "invalid captured clip update");
    CastTimelineClip candidate = *c;
    candidate.duration_ns = duration;
    if (collision(t, &candidate, id)) return fail(e, n, "recording would overlap another clip");
    CastTimelineMedia *m = (CastTimelineMedia *)timeline_media(t, c->media_id);
    if (m->duration_ns < duration) m->duration_ns = duration;
    c->duration_ns = duration;
    c->capture_locked = recording;
    ++t->revision;
    return 0;
}

struct CastTimelineHistory {
    CastTimeline *undo, *redo;
    unsigned capacity, undo_count, redo_count;
};
CastTimelineHistory *timeline_history_open(unsigned capacity)
{
    if (!capacity || capacity > 32) return NULL;
    CastTimelineHistory *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->capacity = capacity;
    h->undo = calloc(capacity, sizeof *h->undo);
    h->redo = calloc(capacity, sizeof *h->redo);
    if (!h->undo || !h->redo) {
        timeline_history_close(h);
        return NULL;
    }
    return h;
}
void timeline_history_close(CastTimelineHistory *h)
{
    if (h) { free(h->undo); free(h->redo); free(h); }
}
static void push(CastTimeline *stack, unsigned *count, unsigned capacity, const CastTimeline *t)
{
    if (*count == capacity) {
        memmove(stack, stack + 1, (capacity - 1) * sizeof *stack);
        --*count;
    }
    stack[(*count)++] = *t;
}
static bool capture_active(const CastTimeline *t)
{
    for (unsigned i = 0; i < t->clip_count; ++i)
        if (t->clips[i].capture_locked) return true;
    return false;
}
int timeline_checkpoint(CastTimelineHistory *h, const CastTimeline *t, char *e, size_t n)
{
    if (!h || timeline_validate(t, e, n)) return fail(e, n, "invalid timeline history");
    push(h->undo, &h->undo_count, h->capacity, t);
    h->redo_count = 0;
    return 0;
}
static int restore(CastTimelineHistory *h, CastTimeline *t, bool redo, char *e, size_t n)
{
    if (!h || timeline_validate(t, e, n) || t->revision >= UINT64_MAX - 1 || capture_active(t))
        return fail(e, n, "history is unavailable during an active recording");
    unsigned *count = redo ? &h->redo_count : &h->undo_count;
    CastTimeline *from = redo ? h->redo : h->undo;
    CastTimeline *to = redo ? h->undo : h->redo;
    unsigned *to_count = redo ? &h->undo_count : &h->redo_count;
    if (!*count || capture_active(&from[*count - 1]))
        return fail(e, n, redo ? "nothing to redo" : "nothing to undo");
    uint64_t revision = t->revision + 1;
    push(to, to_count, h->capacity, t);
    *t = from[--*count];
    t->revision = revision;
    return 0;
}
int timeline_undo(CastTimelineHistory *h, CastTimeline *t, char *e, size_t n)
{
    return restore(h, t, false, e, n);
}
int timeline_redo(CastTimelineHistory *h, CastTimeline *t, char *e, size_t n)
{
    return restore(h, t, true, e, n);
}
