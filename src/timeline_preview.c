#include "timeline_preview.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct CastTimelinePreview {
    pthread_t worker;
    pthread_mutex_t mutex, frame_mutex;
    pthread_cond_t wake;
    atomic_bool stop, finished;
    atomic_uint_fast64_t serial;
    uint64_t active_serial, pending_position, pending_generation, published_generation;
    bool pending;
    CastTimeline timeline;
    Config config;
    Frame published;
    CastTimelinePreviewSnapshot snapshot;
};
static bool cancelled(void *opaque)
{
    CastTimelinePreview *p = opaque;
    return atomic_load(&p->stop) || atomic_load(&p->serial) != p->active_serial;
}
static void *worker(void *opaque)
{
    CastTimelinePreview *p = opaque;
    CastTimelineRenderer *renderer = NULL;
    Frame frame = {0};
    for (;;) {
        pthread_mutex_lock(&p->mutex);
        while (!p->pending && !atomic_load(&p->stop)) {
            pthread_cond_wait(&p->wake, &p->mutex);
        }
        if (atomic_load(&p->stop)) {
            pthread_mutex_unlock(&p->mutex);
            break;
        }
        uint64_t position = p->pending_position, generation = p->pending_generation;
        p->active_serial = atomic_load(&p->serial);
        p->pending = false;
        pthread_mutex_unlock(&p->mutex);
        char error[1024] = "";
        if (!renderer) {
            renderer =
                timeline_renderer_open(&p->timeline, &p->config, cancelled, p, error, sizeof error);
        }
        int result = renderer
                         ? timeline_renderer_frame(renderer, position, &frame, error, sizeof error)
                         : -1;
        pthread_mutex_lock(&p->frame_mutex);
        pthread_mutex_lock(&p->mutex);
        if (!cancelled(p)) {
            if (!result) {
                Frame previous = p->published;
                p->published = frame;
                frame = previous;
                p->published_generation = generation;
                p->snapshot.ready = true;
                p->snapshot.position_ns = position;
                p->snapshot.completed_generation = generation;
            }
            snprintf(p->snapshot.error, sizeof p->snapshot.error, "%s", error);
            p->snapshot.busy = false;
        }
        pthread_mutex_unlock(&p->mutex);
        pthread_mutex_unlock(&p->frame_mutex);
        /* A cancelled decoder can retain partial packet state: reset it before
         * servicing a newer generation rather than reusing interrupted IO. */
        if (result) {
            timeline_renderer_close(renderer);
            renderer = NULL;
        }
    }
    frame_free(&frame);
    timeline_renderer_close(renderer);
    atomic_store(&p->finished, true);
    return NULL;
}
CastTimelinePreview *timeline_preview_open(const CastTimeline *timeline, const Config *config,
                                           char *error, size_t size)
{
    if (!timeline || !config) {
        return NULL;
    }
    CastTimeline t = *timeline;
    Config c = *config;
    if (timeline_validate(&t, error, size) || config_validate(&c, error, size)) {
        return NULL;
    }
    for (unsigned i = 0; i < t.clip_count; ++i) {
        if (t.clips[i].capture_locked) {
            if (size) {
                snprintf(error, size, "preview requires finalized clips");
            }
            return NULL;
        }
    }
    CastTimelinePreview *p = calloc(1, sizeof *p);
    if (!p) {
        if (size) {
            snprintf(error, size, "preview allocation failed");
        }
        return NULL;
    }
    p->timeline = t;
    p->config = c;
    p->snapshot.revision = t.revision;
    atomic_init(&p->stop, false);
    atomic_init(&p->finished, false);
    atomic_init(&p->serial, 0);
    pthread_mutex_init(&p->mutex, NULL);
    pthread_mutex_init(&p->frame_mutex, NULL);
    pthread_cond_init(&p->wake, NULL);
    if (pthread_create(&p->worker, NULL, worker, p)) {
        pthread_cond_destroy(&p->wake);
        pthread_mutex_destroy(&p->mutex);
        pthread_mutex_destroy(&p->frame_mutex);
        free(p);
        if (size) {
            snprintf(error, size, "preview worker creation failed");
        }
        return NULL;
    }
    return p;
}
int timeline_preview_request(CastTimelinePreview *p, uint64_t position, uint64_t generation)
{
    if (!p || !generation || position > timeline_duration(&p->timeline)) {
        return -1;
    }
    pthread_mutex_lock(&p->mutex);
    if (generation <= p->snapshot.requested_generation || atomic_load(&p->stop)) {
        pthread_mutex_unlock(&p->mutex);
        return -1;
    }
    p->pending_position = position;
    p->pending_generation = generation;
    p->pending = true;
    p->snapshot.requested_generation = generation;
    p->snapshot.busy = true;
    p->snapshot.error[0] = 0;
    atomic_fetch_add(&p->serial, 1);
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->mutex);
    return 0;
}
void timeline_preview_snapshot(CastTimelinePreview *p, CastTimelinePreviewSnapshot *out)
{
    if (!p || !out) {
        return;
    }
    pthread_mutex_lock(&p->mutex);
    *out = p->snapshot;
    pthread_mutex_unlock(&p->mutex);
}
int timeline_preview_copy(CastTimelinePreview *p, Frame *out, uint64_t *generation)
{
    if (!p || !out) {
        return -1;
    }
    pthread_mutex_lock(&p->frame_mutex);
    if (!p->published.data) {
        pthread_mutex_unlock(&p->frame_mutex);
        return 1;
    }
    if (frame_alloc(out, p->published.width, p->published.height)) {
        pthread_mutex_unlock(&p->frame_mutex);
        return -1;
    }
    for (int y = 0; y < out->height; ++y) {
        memcpy(out->data + (size_t)y * out->stride,
               p->published.data + (size_t)y * p->published.stride, (size_t)out->width * 4);
    }
    out->ts_ns = p->published.ts_ns;
    if (generation) {
        *generation = p->published_generation;
    }
    pthread_mutex_unlock(&p->frame_mutex);
    return 0;
}
bool timeline_preview_finished(CastTimelinePreview *p)
{
    return !p || atomic_load(&p->finished);
}
void timeline_preview_cancel(CastTimelinePreview *p)
{
    if (!p) {
        return;
    }
    atomic_store(&p->stop, true);
    pthread_mutex_lock(&p->mutex);
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->mutex);
}
void timeline_preview_close(CastTimelinePreview *p)
{
    if (!p) {
        return;
    }
    atomic_store(&p->stop, true);
    pthread_mutex_lock(&p->mutex);
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->mutex);
    pthread_join(p->worker, NULL);
    frame_free(&p->published);
    pthread_cond_destroy(&p->wake);
    pthread_mutex_destroy(&p->mutex);
    pthread_mutex_destroy(&p->frame_mutex);
    free(p);
}
