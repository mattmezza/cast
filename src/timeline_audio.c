#define _POSIX_C_SOURCE 200809L
#include "timeline_audio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHUNK 480u
#define STEP UINT64_C(10000000)
struct CastTimelineAudio {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t worker;
    atomic_bool closing, finished;
    atomic_uint_fast64_t generation;
    uint64_t active_generation, position, anchor;
    bool desired;
    CastTimeline timeline;
    Config config;
    CastAudioMonitor *monitor;
    CastTimelineAudioStatus status;
};
static uint64_t sample_at(uint64_t ns)
{
    return ns / UINT64_C(1000000000) * 48000 +
           ns % UINT64_C(1000000000) * 48000 / UINT64_C(1000000000);
}
static uint64_t ns_at(uint64_t sample)
{
    return sample / 48000 * UINT64_C(1000000000) + sample % 48000 * UINT64_C(1000000000) / 48000;
}
static bool cancelled(void *context)
{
    CastTimelineAudio *s = context;
    return atomic_load(&s->closing) || s->active_generation != atomic_load(&s->generation);
}
static void wait_tick(CastTimelineAudio *s)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += STEP / 2;
    if (t.tv_nsec >= 1000000000) {
        t.tv_nsec -= 1000000000;
        t.tv_sec++;
    }
    pthread_cond_timedwait(&s->wake, &s->lock, &t);
}
static void *produce(void *context)
{
    CastTimelineAudio *s = context;
    CastTimelineRenderer *r = NULL;
    bool active = false;
    uint64_t epoch = 0, cursor = 0, position = 0, anchor = 0, previous = 0;
    float pcm[CHUNK * 2];
    for (;;) {
        pthread_mutex_lock(&s->lock);
        if (atomic_load(&s->closing)) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        bool desired = s->desired;
        uint64_t generation = atomic_load(&s->generation);
        if (generation != previous) {
            position = s->position;
            anchor = s->anchor;
            cursor = sample_at(position);
            previous = generation;
            s->active_generation = generation;
            epoch = audio_monitor_barrier(s->monitor);
        }
        pthread_mutex_unlock(&s->lock);
        if (!desired) {
            if (active) {
                audio_monitor_stop(s->monitor);
                active = false;
            }
            pthread_mutex_lock(&s->lock);
            s->status.running = false;
            memset(&s->status.output, 0, sizeof s->status.output);
            if (!s->desired && !atomic_load(&s->closing)) {
                pthread_cond_wait(&s->wake, &s->lock);
            }
            pthread_mutex_unlock(&s->lock);
            continue;
        }
        char error[256] = "";
        int result = 0;
        if (!active) {
            result = audio_monitor_start(s->monitor, error, sizeof error);
            if (!result) {
                active = true;
                epoch = audio_monitor_barrier(s->monitor);
            }
        }
        if (!result && !r) {
            r = timeline_renderer_open(&s->timeline, &s->config, cancelled, s, error, sizeof error);
        }
        if (!result && !r) {
            result = -1;
        }
        uint64_t now = cast_now_ns(), duration = timeline_duration(&s->timeline),
                 total = sample_at(duration);
        uint64_t elapsed = now >= anchor ? now - anchor : 0,
                 target = sample_at(position) + sample_at(elapsed);
        if (cursor < target) {
            cursor = target; /* Never replay a decode backlog after delay. */
        }
        bool end = cursor >= total;
        if (!result && !end && ns_at(cursor - sample_at(position)) <= elapsed + STEP * 2) {
            unsigned count = total - cursor < CHUNK ? (unsigned)(total - cursor) : CHUNK;
            result = timeline_renderer_audio(r, cursor, count, pcm, error, sizeof error);
            pthread_mutex_lock(&s->lock);
            bool current = s->desired && !cancelled(s);
            if (!result && current) {
                uint64_t first = anchor + ns_at(cursor - sample_at(position));
                int accepted = audio_monitor_submit(s->monitor, epoch, first, pcm, count);
                if (accepted < 0) {
                    result = -1;
                    snprintf(error, sizeof error, "Timeline audio timestamp rejected");
                }
                cursor += count;
            }
            pthread_mutex_unlock(&s->lock);
            memset(pcm, 0, sizeof pcm);
        }
        CastMonitorStatus output = {0};
        if (active) {
            audio_monitor_status(s->monitor, &output);
        }
        if (active && !output.healthy && !result) {
            result = -1;
            snprintf(error, sizeof error, "%s",
                     output.error[0] ? output.error : "Timeline output unavailable");
        }
        pthread_mutex_lock(&s->lock);
        if (generation == atomic_load(&s->generation)) {
            s->status.running = active && !result && !end;
            s->status.output = output;
            if (result || end) {
                s->desired = false;
                s->status.requested = false;
                audio_monitor_barrier(s->monitor);
            }
            if (result) {
                snprintf(s->status.error, sizeof s->status.error, "%s",
                         error[0] ? error : "Timeline audio failed");
            }
        }
        if (!atomic_load(&s->closing)) {
            wait_tick(s);
        }
        pthread_mutex_unlock(&s->lock);
        if (result) {
            timeline_renderer_close(r);
            r = NULL;
            if (active) {
                audio_monitor_stop(s->monitor);
                active = false;
            }
        }
    }
    memset(pcm, 0, sizeof pcm);
    timeline_renderer_close(r);
    audio_monitor_stop(s->monitor);
    atomic_store(&s->finished, true);
    return NULL;
}
CastTimelineAudio *timeline_audio_open(const CastTimeline *t, const Config *c, CastAudioMonitor *m)
{
    if (!t || !c || !m) {
        return NULL;
    }
    char error[256];
    if (timeline_validate(t, error, sizeof error) ||
        config_validate(c, error, sizeof error)) {
        return NULL;
    }
    for (unsigned i = 0; i < t->clip_count; ++i) {
        if (t->clips[i].capture_locked) {
            return NULL;
        }
    }
    CastTimelineAudio *s = calloc(1, sizeof *s);
    if (!s) {
        return NULL;
    }
    s->timeline = *t;
    s->config = *c;
    s->monitor = m;
    s->status.revision = t->revision;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    atomic_init(&s->generation, 1);
    atomic_init(&s->closing, false);
    atomic_init(&s->finished, false);
    if (pthread_create(&s->worker, NULL, produce, s)) {
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}
static void update(CastTimelineAudio *s, bool play, uint64_t position, uint64_t anchor, bool close)
{
    if (!s) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    audio_monitor_barrier(s->monitor);
    if (close) {
        atomic_store(&s->closing, true);
    }
    s->desired = play && !atomic_load(&s->closing);
    s->position = position;
    s->anchor = anchor;
    s->status.requested = s->desired;
    s->status.running = false;
    s->status.error[0] = 0;
    memset(&s->status.output, 0, sizeof s->status.output);
    s->status.generation = atomic_fetch_add(&s->generation, 1) + 1;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
}
void timeline_audio_play(CastTimelineAudio *s, uint64_t position, uint64_t anchor)
{
    if (s && anchor && position < timeline_duration(&s->timeline)) {
        update(s, true, position, anchor, false);
    }
}
void timeline_audio_pause(CastTimelineAudio *s)
{
    update(s, false, 0, 0, false);
}
void timeline_audio_cancel(CastTimelineAudio *s)
{
    update(s, false, 0, 0, true);
}
bool timeline_audio_finished(CastTimelineAudio *s)
{
    return !s || atomic_load(&s->finished);
}
void timeline_audio_status(CastTimelineAudio *s, CastTimelineAudioStatus *out)
{
    if (!s || !out) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    *out = s->status;
    out->finished = atomic_load(&s->finished);
    pthread_mutex_unlock(&s->lock);
}
void timeline_audio_close(CastTimelineAudio *s)
{
    if (s) {
        timeline_audio_cancel(s);
        pthread_join(s->worker, NULL);
        audio_monitor_free(s->monitor);
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
}
