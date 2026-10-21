#include "timeline_audio.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct {
    pthread_mutex_t lock;
    CastMonitorRender render;
    void *context;
    unsigned opens, closes;
    bool fail;
    atomic_bool hold_open, entered;
} Fake;
static int output_open(void *opaque, CastMonitorRender render, void *context, char *error,
                       size_t size)
{
    Fake *f = opaque;
    pthread_mutex_lock(&f->lock);
    ++f->opens;
    bool fail = f->fail;
    pthread_mutex_unlock(&f->lock);
    atomic_store(&f->entered, true);
    while (atomic_load(&f->hold_open)) {
        usleep(1000);
    }
    if (fail) {
        snprintf(error, size, "fake unavailable");
        return -1;
    }
    pthread_mutex_lock(&f->lock);
    f->render = render;
    f->context = context;
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static void output_close(void *opaque)
{
    Fake *f = opaque;
    pthread_mutex_lock(&f->lock);
    f->render = NULL;
    f->context = NULL;
    ++f->closes;
    pthread_mutex_unlock(&f->lock);
}
static float consume(Fake *f)
{
    float pcm[960] = {0};
    pthread_mutex_lock(&f->lock);
    if (f->render) {
        f->render(f->context, pcm, 480);
    }
    pthread_mutex_unlock(&f->lock);
    float peak = 0;
    for (unsigned i = 0; i < 960; ++i) {
        peak = fmaxf(peak, fabsf(pcm[i]));
    }
    return peak;
}
static unsigned opens(Fake *f)
{
    pthread_mutex_lock(&f->lock);
    unsigned n = f->opens;
    pthread_mutex_unlock(&f->lock);
    return n;
}
static void le(FILE *f, unsigned value, unsigned bytes)
{
    while (bytes--) {
        fputc(value & 255, f);
        value >>= 8;
    }
}
static void wave(const char *path)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite("RIFF", 1, 4, f);
    le(f, 36 + 48000 * 4, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    le(f, 16, 4);
    le(f, 1, 2);
    le(f, 2, 2);
    le(f, 48000, 4);
    le(f, 192000, 4);
    le(f, 4, 2);
    le(f, 16, 2);
    fwrite("data", 1, 4, f);
    le(f, 48000 * 4, 4);
    for (unsigned i = 0; i < 48000; ++i) {
        le(f, i < 24000 ? 8192 : 16384, 2);
        le(f, i < 24000 ? 8192 : 16384, 2);
    }
    assert(!fclose(f));
}

int main(void)
{
    char directory[] = "/tmp/cast-timeline-audio-XXXXXX", path[4096], error[1024];
    assert(mkdtemp(directory));
    snprintf(path, sizeof path, "%s/input.wav", directory);
    wave(path);
    Config c;
    config_defaults(&c);
    c.width = 64;
    c.height = 64;
    c.fps = 10;
    CastTimeline t;
    timeline_init(&t);
    uint64_t media, clip;
    assert(!timeline_add_media(&t, path, CAST_TIMELINE_AUDIO, 1000000000, &media, error,
                               sizeof error));
    assert(!timeline_add_clip(&t, media, 0, 0, 0, 1000000000, &clip, error, sizeof error));
    assert(!timeline_gain(&t, clip, .5f, false, error, sizeof error));
    Fake f = {0};
    pthread_mutex_init(&f.lock, NULL);
    atomic_init(&f.hold_open, false);
    atomic_init(&f.entered, false);
    CastMonitorBackend ops = {.open = output_open, .close = output_close};
    CastAudioMonitor *m = audio_monitor_create(&ops, &f);
    assert(m);
    CastTimelineAudio *s = timeline_audio_open(&t, &c, m);
    assert(s && opens(&f) == 0);
    usleep(20000);
    assert(opens(&f) == 0);
    timeline_audio_play(s, 0, cast_now_ns());
    bool heard = false;
    for (unsigned i = 0; i < 200; ++i) {
        float peak = consume(&f);
        if (peak > .01f) {
            assert(fabsf(peak - .125f) < .001f);
            heard = true;
            break;
        }
        usleep(1000);
    }
    assert(heard && opens(&f) == 1);
    timeline_audio_pause(s);
    assert(consume(&f) == 0);
    CastTimelineAudioStatus status;
    timeline_audio_status(s, &status);
    assert(!status.requested && !status.running && !status.output.queued_frames);
    timeline_audio_play(s, 700000000, cast_now_ns());
    heard = false;
    for (unsigned i = 0; i < 200; ++i) {
        float peak = consume(&f);
        if (peak > .01f) {
            assert(fabsf(peak - .25f) < .001f);
            heard = true;
            break;
        }
        usleep(1000);
    }
    assert(heard);
    /* Rapid seek/play/pause coalescing cannot leak an earlier generation. */
    for (unsigned i = 0; i < 100; ++i) {
        timeline_audio_play(s, i % 2 ? 700000000 : 0, cast_now_ns());
        timeline_audio_pause(s);
        assert(consume(&f) == 0);
    }
    timeline_audio_play(s, 700000000, cast_now_ns());
    heard = false;
    for (unsigned i = 0; i < 200; ++i) {
        float peak = consume(&f);
        if (peak > .01f) {
            assert(fabsf(peak - .25f) < .001f);
            heard = true;
            break;
        }
        usleep(1000);
    }
    assert(heard);
    /* Generation retirement must silence queued PCM before lifecycle close. */
    timeline_audio_cancel(s);
    assert(consume(&f) == 0);
    for (unsigned i = 0; i < 1000 && !timeline_audio_finished(s); ++i) {
        usleep(1000);
    }
    assert(timeline_audio_finished(s));
    timeline_audio_close(s);
    f.fail = true;
    m = audio_monitor_create(&ops, &f);
    s = timeline_audio_open(&t, &c, m);
    assert(s);
    unsigned before = opens(&f);
    timeline_audio_play(s, 0, cast_now_ns());
    for (unsigned i = 0; i < 1000; ++i) {
        timeline_audio_status(s, &status);
        if (*status.error) {
            break;
        }
        usleep(1000);
    }
    assert(strstr(status.error, "fake unavailable"));
    usleep(30000);
    assert(opens(&f) == before + 1);
    timeline_audio_close(s);
    /* Privacy pause during a slow device open never releases buffered PCM. */
    f.fail = false;
    atomic_store(&f.hold_open, true);
    atomic_store(&f.entered, false);
    m = audio_monitor_create(&ops, &f);
    s = timeline_audio_open(&t, &c, m);
    assert(s);
    timeline_audio_play(s, 0, cast_now_ns());
    for (unsigned i = 0; i < 1000 && !atomic_load(&f.entered); ++i) {
        usleep(1000);
    }
    assert(atomic_load(&f.entered));
    timeline_audio_pause(s);
    assert(consume(&f) == 0);
    atomic_store(&f.hold_open, false);
    for (unsigned i = 0; i < 40; ++i) {
        assert(consume(&f) == 0);
        usleep(1000);
    }
    timeline_audio_status(s, &status);
    assert(!status.requested && !status.running);
    timeline_audio_close(s);
    pthread_mutex_destroy(&f.lock);
    assert(!unlink(path));
    assert(!rmdir(directory));
    puts("Timeline audio fake-output decode/gain/seek/barrier/failure lifecycle passed");
}
