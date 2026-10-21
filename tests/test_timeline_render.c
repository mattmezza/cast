#include "timeline_render.h"
#include <assert.h>
#include <math.h>
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
    char dir[] = "/tmp/cast-render-test-XXXXXX", input[4096], output[4096], error[CAST_ERR];
    assert(mkdtemp(dir));
    snprintf(input, sizeof input, "%s/input.wav", dir);
    snprintf(output, sizeof output, "%s/movie.mkv", dir);
    wave(input);
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 64;
    cfg.height = 64;
    cfg.fps = 10;
    snprintf(cfg.video_codec, sizeof cfg.video_codec, "ffv1");
    snprintf(cfg.audio_codec, sizeof cfg.audio_codec, "flac");
    snprintf(cfg.record_container, sizeof cfg.record_container, "matroska");
    CastTimeline t;
    timeline_init(&t);
    uint64_t media, clip;
    assert(!timeline_add_media(&t, input, CAST_TIMELINE_AUDIO, 1000000000, &media, error,
                               sizeof error));
    assert(!timeline_add_clip(&t, media, 0, 0, 0, 1000000000, &clip, error, sizeof error));
    assert(!timeline_gain(&t, clip, .5f, false, error, sizeof error));
    CastTimelineRenderer *r = timeline_renderer_open(&t, &cfg, NULL, NULL, error, sizeof error);
    if (!r) {
        fprintf(stderr, "open: %s\n", error);
        abort();
    }
    float pcm[2048];
    assert(!timeline_renderer_audio(r, 0, 1024, pcm, error, sizeof error));
    assert(fabsf(pcm[100] - .125f) < .0001f);
    assert(!timeline_renderer_audio(r, 30000, 1024, pcm, error, sizeof error));
    assert(fabsf(pcm[100] - .25f) < .0001f);
    assert(!timeline_renderer_audio(r, 1000, 1024, pcm, error, sizeof error));
    assert(fabsf(pcm[100] - .125f) < .0001f);
    Frame frame = {0};
    assert(!timeline_renderer_frame(r, 0, &frame, error, sizeof error));
    assert(frame.width == 64 && frame.data[0] == 0 && frame.data[3] == 255);
    frame_free(&frame);
    timeline_renderer_close(r);
    CastTimelineExport *job =
        timeline_export_start_filtered(&t, &cfg, output, paint, NULL, error, sizeof error);
    assert(job);
    CastTimelineExportSnapshot state;
    for (unsigned i = 0; i < 10000; ++i) {
        timeline_export_snapshot(job, &state);
        if (state.state >= CAST_TIMELINE_EXPORT_COMPLETE) {
            break;
        }
        usleep(1000);
    }
    if (state.state != CAST_TIMELINE_EXPORT_COMPLETE) {
        fprintf(stderr, "export: %s\n", state.error);
        abort();
    }
    timeline_export_close(job);
    assert(!access(output, R_OK));
    CastTimeline video;
    timeline_init(&video);
    assert(!timeline_add_media(&video, output, CAST_TIMELINE_VIDEO, 1000000000, &media, error,
                               sizeof error));
    assert(
        !timeline_add_clip(&video, media, 0, 0, 200000000, 600000000, &clip, error, sizeof error));
    uint64_t second;
    assert(!timeline_split(&video, clip, 300000000, &second, error, sizeof error));
    r = timeline_renderer_open(&video, &cfg, NULL, NULL, error, sizeof error);
    assert(r);
    assert(!timeline_renderer_frame(r, 100000000, &frame, error, sizeof error));
    assert(frame.data[0] > 240 && frame.data[2] < 15);
    assert(!timeline_renderer_frame(r, 400000000, &frame, error, sizeof error));
    assert(frame.data[2] > 240 && frame.data[0] < 15);
    assert(!timeline_renderer_audio(r, 2000, 1024, pcm, error, sizeof error));
    assert(fabsf(pcm[100] - .125f) < .005f);
    frame_free(&frame);
    timeline_renderer_close(r);
    FILE *sentinel = fopen(output, "wb");
    assert(sentinel);
    fputs("keep", sentinel);
    fclose(sentinel);
    job = timeline_export_start(&t, &cfg, output, error, sizeof error);
    assert(job);
    timeline_export_cancel(job);
    timeline_export_close(job);
    sentinel = fopen(output, "rb");
    assert(sentinel);
    char marker[5] = {0};
    assert(fread(marker, 1, 4, sentinel) == 4 && !strcmp(marker, "keep"));
    fclose(sentinel);

    assert(!unlink(input));
    r = timeline_renderer_open(&t, &cfg, NULL, NULL, error, sizeof error);
    assert(!r && strstr(error, "media"));
    unlink(output);
    rmdir(dir);
    puts("timeline render tests passed");
}
