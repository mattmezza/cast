#include "timeline_preview.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
static int paint(void *user, uint64_t time, Frame *frame, char *error, size_t size)
{
    (void)user;
    (void)error;
    (void)size;
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            unsigned char *p = frame->data + (size_t)y * frame->stride + x * 4;
            p[0] = time < 500000000 ? 255 : 0;
            p[1] = 0;
            p[2] = time < 500000000 ? 0 : 255;
            p[3] = 255;
        }
    }
    return 0;
}

int main(void)
{
    char dir[] = "/tmp/cast-preview-test-XXXXXX", input[4096], movie[4096], error[1024];
    assert(mkdtemp(dir));
    snprintf(input, sizeof input, "%s/input.wav", dir);
    snprintf(movie, sizeof movie, "%s/video.mkv", dir);
    wave(input);
    Config c;
    config_defaults(&c);
    c.width = 64;
    c.height = 64;
    c.fps = 10;
    snprintf(c.video_codec, sizeof c.video_codec, "ffv1");
    snprintf(c.audio_codec, sizeof c.audio_codec, "flac");
    snprintf(c.record_container, sizeof c.record_container, "matroska");
    CastTimeline t;
    timeline_init(&t);
    uint64_t media, clip;
    assert(!timeline_add_media(&t, input, CAST_TIMELINE_AUDIO, 1000000000, &media, error,
                               sizeof error));
    assert(!timeline_add_clip(&t, media, 0, 0, 0, 1000000000, &clip, error, sizeof error));
    CastTimelineExport *job =
        timeline_export_start_filtered(&t, &c, movie, paint, NULL, error, sizeof error);
    assert(job);
    CastTimelineExportSnapshot status;
    for (unsigned i = 0; i < 5000; ++i) {
        timeline_export_snapshot(job, &status);
        if (status.state >= CAST_TIMELINE_EXPORT_COMPLETE) {
            break;
        }
        usleep(1000);
    }
    assert(status.state == CAST_TIMELINE_EXPORT_COMPLETE);
    timeline_export_close(job);
    timeline_init(&t);
    assert(!timeline_add_media(&t, movie, CAST_TIMELINE_VIDEO, 1000000000, &media, error,
                               sizeof error));
    assert(!timeline_add_clip(&t, media, 0, 0, 0, 1000000000, &clip, error, sizeof error));
    CastTimelinePreview *p = timeline_preview_open(&t, &c, error, sizeof error);
    assert(p);
    Frame frame = {0};
    uint64_t generation = 0;
    assert(timeline_preview_copy(p, &frame, &generation) == 1);
    assert(!timeline_preview_request(p, 100000000, 1));
    CastTimelinePreviewSnapshot snapshot;
    for (unsigned i = 0; i < 5000; ++i) {
        timeline_preview_snapshot(p, &snapshot);
        if (!snapshot.busy) {
            break;
        }
        usleep(1000);
    }
    assert(snapshot.ready && snapshot.completed_generation == 1 &&
           snapshot.revision == t.revision && !snapshot.error[0]);
    assert(!timeline_preview_copy(p, &frame, &generation) && generation == 1);
    assert(frame.data[0] > 240 && frame.data[2] < 15);
    assert(timeline_preview_request(p, 0, 1));
    for (uint64_t i = 2; i <= 1000; ++i) {
        assert(!timeline_preview_request(p, i == 1000 ? 700000000 : 100000000, i));
    }
    for (unsigned i = 0; i < 5000; ++i) {
        timeline_preview_snapshot(p, &snapshot);
        if (!snapshot.busy) {
            break;
        }
        usleep(1000);
    }
    assert(snapshot.completed_generation == 1000 && snapshot.position_ns == 700000000 &&
           !snapshot.error[0]);
    assert(!timeline_preview_copy(p, &frame, &generation) && generation == 1000);
    assert(frame.data[2] > 240 && frame.data[0] < 15);
    /* Immutable source copy survives caller edits and close cancels active requests. */
    timeline_init(&t);
    assert(!timeline_preview_request(p, 100000000, 1001));
    timeline_preview_close(p);
    frame_free(&frame);
    assert(!unlink(input));
    assert(!unlink(movie));
    assert(!timeline_add_media(&t, movie, CAST_TIMELINE_VIDEO, 1000000000, &media, error,
                               sizeof error));
    assert(!timeline_add_clip(&t, media, 0, 0, 0, 1000000000, &clip, error, sizeof error));
    t.clips[0].capture_locked = true;
    assert(!timeline_preview_open(&t, &c, error, sizeof error));
    t.clips[0].capture_locked = false;
    p = timeline_preview_open(&t, &c, error, sizeof error);
    assert(p);
    assert(!timeline_preview_request(p, 0, 1));
    for (unsigned i = 0; i < 5000; ++i) {
        timeline_preview_snapshot(p, &snapshot);
        if (!snapshot.busy) {
            break;
        }
        usleep(1000);
    }
    assert(!snapshot.ready && strstr(snapshot.error, "media"));
    timeline_preview_close(p);
    assert(!rmdir(dir));
    puts("Timeline preview generations, coalescing, immutable snapshots and cancellation passed");
    return 0;
}
