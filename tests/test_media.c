#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast.h"
#include "media_internal.h"
#include "media_codec.h"
#include "stream.h"
#include "composition_assets.h"
#include <assert.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
uint64_t cast_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static void delay_ms(int ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static void paint(Frame *f, int color)
{
    for (int y = 0; y < f->height; ++y) {
        for (int x = 0; x < f->width; ++x) {
            uint8_t *p = f->data + (size_t)y * f->stride + x * 4;
            p[0] = color == 0 ? 230 : 10;
            p[1] = color == 1 ? 230 : 10;
            p[2] = color == 2 ? 230 : 10;
            p[3] = 255;
        }
    }
    f->ts_ns = cast_now_ns();
}
static void feed(Media *m, Frame *f, int color, int count)
{
    char error[CAST_ERR] = {0};
    for (int i = 0; i < count; ++i) {
        paint(f, color);
        assert(media_record_frame(m, f, error, sizeof(error)) == 0);
        assert(media_virtual(m, f, false, error, sizeof(error)) == 0);
        delay_ms(33);
    }
}
struct Tone {
    Media *media;
    atomic_bool stop;
};
static void *tone_thread(void *data)
{
    struct Tone *tone = data;
    uint64_t next = cast_now_ns();
    uint64_t index = 0;
    while (!atomic_load(&tone->stop)) {
        float samples[480 * 2];
        for (int i = 0; i < 480; ++i) {
            float x =
                sinf((float)((index + (uint64_t)i) % 48000) * 6.283185307f * 440 / 48000) * 0.25f;
            samples[i * 2] = samples[i * 2 + 1] = x;
        }
        audio_test_push(media_test_audio(tone->media), 0, next, samples, 480);
        next += 10000000;
        index += 480;
        delay_ms(10);
    }
    return NULL;
}
struct Inspection {
    int video_frames, audio_frames, red, green, blue;
    double video_first, video_last, max_video_gap, audio_first, audio_last, audio_power, audio_peak;
    int64_t audio_samples;
    double window_begin, window_end, window_power;
    int64_t window_samples;
};
static void decoded_frame(struct Inspection *out, AVFrame *frame, AVStream *stream,
                          struct SwsContext **scale, bool audio)
{
    double pts = frame->best_effort_timestamp * av_q2d(stream->time_base);
    if (audio) {
        if (!out->audio_frames) {
            out->audio_first = pts;
        }
        out->audio_frames++;
        out->audio_last = pts + (double)frame->nb_samples / frame->sample_rate;
        out->audio_samples += frame->nb_samples;
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
            const float *p = (const float *)frame->data[0];
            for (int i = 0; i < frame->nb_samples; ++i) {
                out->audio_power += p[i] * p[i];
                double time = pts + (double)i / frame->sample_rate;
                if (time >= out->window_begin && time < out->window_end) {
                    out->window_power += p[i] * p[i];
                    out->window_samples++;
                }
                if (fabsf(p[i]) > out->audio_peak) {
                    out->audio_peak = fabsf(p[i]);
                }
            }
        }
        return;
    }
    if (!out->video_frames) {
        out->video_first = pts;
    } else {
        double gap = pts - out->video_last;
        assert(gap > 0);
        if (gap > out->max_video_gap) {
            out->max_video_gap = gap;
        }
    }
    out->video_frames++;
    out->video_last = pts;
    *scale = sws_getCachedContext(*scale, frame->width, frame->height, frame->format, 1, 1,
                                  AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    assert(*scale);
    uint8_t pixel[4] = {0};
    uint8_t *dst[4] = {pixel, NULL, NULL, NULL};
    int stride[4] = {4, 0, 0, 0};
    assert(sws_scale(*scale, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                     dst, stride) == 1);
    if (pixel[0] > 100 && pixel[0] > pixel[1] * 2) {
        out->red++;
    }
    if (pixel[1] > 100 && pixel[1] > pixel[0] * 2) {
        out->green++;
    }
    if (pixel[2] > 100 && pixel[2] > pixel[0] * 2) {
        out->blue++;
    }
}
static struct Inspection inspect_between(const char *path, double begin, double end)
{
    struct Inspection out = {.window_begin = begin, .window_end = end};
    AVFormatContext *format = NULL;
    assert(avformat_open_input(&format, path, NULL, NULL) == 0);
    assert(avformat_find_stream_info(format, NULL) >= 0);
    int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    assert(video >= 0 && audio >= 0);
    AVCodecContext *decoders[2] = {NULL, NULL};
    int indices[2] = {video, audio};
    for (int i = 0; i < 2; ++i) {
        AVStream *s = format->streams[indices[i]];
        const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
        assert(codec);
        decoders[i] = avcodec_alloc_context3(codec);
        assert(decoders[i]);
        assert(avcodec_parameters_to_context(decoders[i], s->codecpar) == 0);
        assert(avcodec_open2(decoders[i], codec, NULL) == 0);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    assert(packet && frame);
    struct SwsContext *scale = NULL;
    while (av_read_frame(format, packet) >= 0) {
        for (int i = 0; i < 2; ++i) {
            if (packet->stream_index == indices[i]) {
                assert(avcodec_send_packet(decoders[i], packet) == 0);
                while (avcodec_receive_frame(decoders[i], frame) == 0) {
                    decoded_frame(&out, frame, format->streams[indices[i]], &scale, i == 1);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    for (int i = 0; i < 2; ++i) {
        assert(avcodec_send_packet(decoders[i], NULL) == 0);
        while (avcodec_receive_frame(decoders[i], frame) == 0) {
            decoded_frame(&out, frame, format->streams[indices[i]], &scale, i == 1);
            av_frame_unref(frame);
        }
        avcodec_free_context(&decoders[i]);
    }
    sws_freeContext(scale);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avformat_close_input(&format);
    return out;
}
static struct Inspection inspect(const char *path)
{
    return inspect_between(path, 0, 0);
}
static Media *new_media(Config *cfg)
{
    config_defaults(cfg);
    cfg->width = 160;
    cfg->height = 90;
    cfg->fps = 30;
    cfg->record_queue = 3;
    strcpy(cfg->output_device, "none");
    strcpy(cfg->camera_device, "synthetic");
    cfg->mic = cfg->desktop = cfg->virtual_audio = false;
    char error[CAST_ERR] = {0};
    Media *m = media_open(cfg, error, sizeof(error));
    if (!m) {
        fprintf(stderr, "media_open: %s\n", error);
    }
    assert(m);
    return m;
}
static void finalized(Media *m)
{
    uint64_t deadline = cast_now_ns() + 5000000000ULL;
    while (media_record_finalizing(m) && cast_now_ns() < deadline) {
        delay_ms(5);
    }
    assert(!media_record_finalizing(m));
    bool active, paused;
    uint64_t dropped;
    char error[CAST_ERR];
    recorder_status(media_test_recorder(m), &active, &paused, &dropped, error, sizeof(error));
    assert(!active && !paused);
    if (error[0]) {
        fprintf(stderr, "finalization: %s\n", error);
    }
    assert(!error[0]);
}
static void timeline_test(const char *directory, bool virtual)
{
    Config cfg;
    Media *m = new_media(&cfg);
    cfg.virtual_enabled = virtual;
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    char path[PATH_MAX], error[CAST_ERR], actual[PATH_MAX];
    snprintf(path, sizeof(path), "%s/timeline-%d.mkv", directory, virtual);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    media_record_path(m, actual, sizeof(actual));
    assert(!strcmp(path, actual));
    struct Tone tone = {.media = m};
    pthread_t audio_worker;
    assert(pthread_create(&audio_worker, NULL, tone_thread, &tone) == 0);
    feed(m, &f, 0, 12);
    assert(media_record_pause(m, true, error, sizeof(error)) == 0);
    uint64_t paused_duration = media_record_duration(m);
    delay_ms(600);
    paint(&f, 1);
    assert(media_record_frame(m, &f, error, sizeof(error)) == 0);
    assert(media_record_duration(m) == paused_duration);
    assert(media_record_pause(m, false, error, sizeof(error)) == 0);
    feed(m, &f, 2, 10);
    assert(media_record_pause(m, true, error, sizeof(error)) == 0);
    delay_ms(140);
    assert(media_record_pause(m, false, error, sizeof(error)) == 0);
    feed(m, &f, 0, 10);
    double active = media_record_duration(m) / 1e9;
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    finalized(m);
    assert(strstr(error, path));
    atomic_store(&tone.stop, true);
    pthread_join(audio_worker, NULL);
    struct Inspection x = inspect(path);
    assert(x.video_frames >= 26);
    assert(x.red >= 15 && x.blue >= 7 && x.green == 0);
    assert(x.max_video_gap < 0.100);
    assert(fabs(x.audio_last - active) < 0.070);
    assert(fabs(x.audio_last - x.video_last) < 0.100);
    assert(x.audio_samples > 40000);
    assert(x.audio_power / x.audio_samples > 0.002);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) < 0);
    assert(strstr(error, "never overwritten"));
    frame_free(&f);
    media_close(m);
    printf("recording %s: decoded %d video frames, active %.3fs, audio %.3fs, max gap %.3fs\n",
           virtual ? "+ virtual" : "only", x.video_frames, active, x.audio_last, x.max_video_gap);
}
static void privacy_test(const char *directory)
{
    Config cfg;
    Media *m = new_media(&cfg);
    char path[PATH_MAX], error[CAST_ERR];
    snprintf(path, sizeof(path), "%s/privacy.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    feed(m, &f, 0, 3);
    recorder_test_slow(media_test_recorder(m), 200);
    paint(&f, 1);
    for (int i = 0; i < 15; ++i) {
        f.ts_ns = cast_now_ns();
        assert(media_record_frame(m, &f, error, sizeof(error)) == 0);
    }
    delay_ms(20);
    float sensitive[960];
    for (int i = 0; i < 960; ++i) {
        sensitive[i] = 1;
    }
    uint64_t before = cast_now_ns();
    audio_test_push(media_test_audio(m), 0, before, sensitive, 480);
    assert(media_privacy(m, true, false, true, error, sizeof error) == 0);
    float silence[960];
    audio_read(media_test_audio(m), before, silence, 480);
    for (int i = 0; i < 960; ++i) {
        assert(silence[i] == 0);
    }
    recorder_test_slow(media_test_recorder(m), 0);
    delay_ms(220);
    assert(media_privacy(m, false, false, false, error, sizeof error) == 0);
    /* An old pre-pause frame cannot cross the resume epoch. */
    f.ts_ns = before;
    assert(media_record_frame(m, &f, error, sizeof(error)) == 0);
    feed(m, &f, 2, 6);
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    finalized(m);
    bool recording, paused;
    uint64_t dropped;
    media_status(m, &recording, &paused, &dropped, error, sizeof(error));
    assert(!recording && dropped > 0);
    struct Inspection x = inspect(path);
    assert(x.green == 0 && x.blue > 0 && x.red > 0);
    assert(x.audio_peak < 0.001);
    frame_free(&f);
    media_close(m);
    printf("privacy barrier: no queued green frames or sensitive audio; %llu drops\n",
           (unsigned long long)dropped);
}
static void solid_and_cut_test(const char *directory)
{
    Config cfg;
    Media *m = new_media(&cfg);
    Frame frame = {0};
    assert(frame_alloc(&frame, cfg.width, cfg.height) == 0);
    char path[PATH_MAX], error[CAST_ERR];
    snprintf(path, sizeof path, "%s/solid-and-cut.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof error) == 0);
    struct Tone tone = {.media = m};
    pthread_t audio_worker;
    assert(pthread_create(&audio_worker, NULL, tone_thread, &tone) == 0);
    uint64_t wall_start = cast_now_ns();
    feed(m, &frame, 0, 12);
    double silent_begin = media_record_duration(m) / 1e9;
    assert(media_record_silence(m, true, error, sizeof error) == 0);
    feed(m, &frame, 1, 18); /* Styled solid frames remain on the file timeline. */
    double silent_end = media_record_duration(m) / 1e9;
    assert(silent_end - silent_begin > 0.5);
    bool active, cut;
    uint64_t dropped;
    media_status(m, &active, &cut, &dropped, error, sizeof error);
    assert(active && !cut);
    assert(media_record_pause(m, true, error, sizeof error) == 0);
    uint64_t duration = media_record_duration(m);
    delay_ms(200);
    feed(m, &frame, 2, 3);
    assert(media_record_duration(m) == duration);
    assert(media_record_pause(m, false, error, sizeof error) == 0);
    assert(media_record_silence(m, false, error, sizeof error) == 0);
    feed(m, &frame, 2, 12);
    double active_seconds = media_record_duration(m) / 1e9;
    double wall_seconds = (cast_now_ns() - wall_start) / 1e9;
    assert(wall_seconds - active_seconds > 0.25);
    assert(media_record_stop(m, error, sizeof error) == 0);
    uint64_t deadline = cast_now_ns() + UINT64_C(5000000000);
    while (media_record_finalizing(m) && cast_now_ns() < deadline) {
        delay_ms(1);
    }
    assert(!media_record_finalizing(m));
    media_record_error(m, error, sizeof error);
    assert(!error[0]);
    atomic_store(&tone.stop, true);
    pthread_join(audio_worker, NULL);
    struct Inspection result = inspect_between(path, silent_begin + 0.1, silent_end - 0.1);
    assert(result.green >= 14 && result.blue >= 9 && result.red >= 9);
    assert(result.video_frames <= 42 && result.max_video_gap < 0.1);
    assert(fabs(result.audio_last - active_seconds) < 0.08);
    assert(result.window_samples > 15000 && result.window_power / result.window_samples < 1e-7);
    assert(result.audio_power / result.audio_samples > 0.001);
    frame_free(&frame);
    media_close(m);
    printf("solid pause writes video + decoded silence; cut removes %.3fs; same-file AV %.3fs\n",
           wall_seconds - active_seconds, result.audio_last);
}
static void failure_test(const char *directory)
{
    Config cfg;
    Media *m = new_media(&cfg);
    char path[PATH_MAX], error[CAST_ERR] = {0};
    snprintf(path, sizeof(path), "%s/failure.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    recorder_test_failure(media_test_recorder(m), 1);
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    feed(m, &f, 0, 4);
    bool recording, paused;
    uint64_t dropped;
    media_status(m, &recording, &paused, &dropped, error, sizeof(error));
    assert(!recording && strstr(error, "encoder failure"));
    assert(access(path, R_OK) == 0);
    assert(media_virtual(m, &f, true, error, sizeof(error)) == 0);
    recorder_test_failure(media_test_recorder(m), -1);
    snprintf(path, sizeof(path), "%s/after-failure.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    feed(m, &f, 2, 3);
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    finalized(m);
    struct Inspection x = inspect(path);
    assert(x.blue > 0);
    Config invalid = cfg;
    strcpy(invalid.camera_device, "/dev/cast-nonexistent");
    assert(media_reconfigure(m, &invalid, false, error, sizeof(error)) < 0);
    assert(media_camera(m, &f, error, sizeof(error)) == 0);
    assert(f.width == 640 && f.height == 360);
    frame_free(&f);
    media_close(m);
    puts("encoder failure isolates virtual output, permits restart, and failed reconfigure "
         "preserves "
         "camera");
}
static void responsiveness_test(const char *directory)
{
    Config cfg;
    Media *m = new_media(&cfg);
    char path[PATH_MAX], actual[PATH_MAX], error[CAST_ERR];
    snprintf(path, sizeof(path), "%s/slow.mkv", directory);
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    /* Exercise virtual-only before starting simultaneous operation. */
    feed(m, &f, 0, 3);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    feed(m, &f, 0, 3);
    recorder_test_hold(media_test_recorder(m), 300);
    for (int i = 0; i < 20; ++i) {
        paint(&f, 2);
        assert(media_record_frame(m, &f, error, sizeof(error)) == 0);
        delay_ms(1);
    }
    uint64_t deadline = cast_now_ns() + 1000000000;
    while (!recorder_test_codec_busy(media_test_recorder(m)) && cast_now_ns() < deadline) {
        delay_ms(1);
    }
    assert(recorder_test_codec_busy(media_test_recorder(m)));
    uint64_t before = cast_now_ns();
    bool active, paused;
    uint64_t dropped;
    media_status(m, &active, &paused, &dropped, error, sizeof(error));
    assert(active && dropped > 0);
    assert(media_record_duration(m) > 0);
    media_record_path(m, actual, sizeof(actual));
    assert(!strcmp(actual, path));
    assert(media_virtual(m, &f, false, error, sizeof(error)) == 0);
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    assert(media_record_finalizing(m));
    /* Live privacy must not wait on the recording's drain/trailer/fsync. */
    assert(media_privacy(m, true, false, true, error, sizeof error) == 0);
    assert(media_virtual(m, &f, true, error, sizeof(error)) == 0);
    double control_ms = (cast_now_ns() - before) / 1e6;
    assert(control_ms < 100);
    assert(media_record_start(m, &cfg, NULL, error, sizeof(error)) < 0);
    assert(strstr(error, "finalizing"));
    finalized(m);
    struct Inspection x = inspect(path);
    assert(x.red > 0 && x.blue > 0 && x.green == 0);
    recorder_test_hold(media_test_recorder(m), 0);
    frame_free(&f);
    media_close(m);
    printf("slow encoder: bounded queue dropped %llu frames; status/virtual/stop/privacy %.2f ms\n",
           (unsigned long long)dropped, control_ms);
}
static void disk_failure_test(const char *directory)
{
    Config cfg;
    Media *m = new_media(&cfg);
    char path[PATH_MAX], error[CAST_ERR];
    snprintf(path, sizeof(path), "%s/disk-full.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    recorder_test_write_limit(media_test_recorder(m), 1000);
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    feed(m, &f, 0, 10);
    bool active, paused;
    uint64_t dropped;
    recorder_status(media_test_recorder(m), &active, &paused, &dropped, error, sizeof(error));
    if (active) {
        assert(media_record_stop(m, error, sizeof(error)) == 0);
    }
    uint64_t deadline = cast_now_ns() + 5000000000ULL;
    while (media_record_finalizing(m) && cast_now_ns() < deadline) {
        delay_ms(5);
    }
    assert(!media_record_finalizing(m));
    media_record_error(m, error, sizeof(error));
    assert(strstr(error, "No space left on device"));
    assert(access(path, R_OK) == 0);
    assert(media_virtual(m, &f, false, error, sizeof(error)) == 0);
    recorder_test_write_limit(media_test_recorder(m), -1);
    snprintf(path, sizeof(path), "%s/after-disk-full.mkv", directory);
    assert(media_record_start(m, &cfg, path, error, sizeof(error)) == 0);
    feed(m, &f, 2, 3);
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    finalized(m);
    struct Inspection x = inspect(path);
    assert(x.blue > 0);
    frame_free(&f);
    media_close(m);
    puts("ENOSPC write failure: partial file retained, virtual continues, next recording decodes");
}
static void audio_lanes_test(void)
{
    Config cfg;
    Media *m = new_media(&cfg);
    CastAudio *audio = media_test_audio(m);
    float samples[960], out[960];
    for (int i = 0; i < 960; ++i) {
        samples[i] = 0.8f;
    }
    uint64_t now = cast_now_ns() - 1000000;
    audio_test_push(audio, 0, now, samples, 480);
    audio_test_push(audio, 1, now, samples, 480);
    audio_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 1.0f);
    }
    /* Virtual audio is independently silent during virtual pause/freeze while the
     * recording mix is available, then resumes only from new captured buffers. */
    audio_test_virtual_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
    audio_virtual_privacy(audio, false);
    audio_test_virtual_read(audio, now, out, 1);
    assert(out[0] == 0);
    audio_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 1.0f); /* Live transitions preserve the recording mix. */
    }
    audio_test_push(audio, 0, now, samples, 480); /* A late pre-boundary chunk. */
    audio_test_virtual_read(audio, now + UINT64_C(10000000), out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
    audio_read(audio, now + UINT64_C(10000000), out, 480);
    assert(out[0] > 0.7f);
    now += UINT64_C(20000000);
    audio_test_push(audio, 0, now, samples, 480);
    audio_test_push(audio, 1, now, samples, 480);
    audio_test_virtual_read(audio, now, out, 480);
    assert(out[0] > 0.7f);
    audio_record_privacy(audio);
    audio_read(audio, now, out, 1);
    assert(out[0] == 0);
    audio_test_virtual_read(audio, now, out, 480);
    assert(out[0] > 0.7f); /* Recording transitions preserve the virtual mix. */
    now += UINT64_C(10000000);
    audio_test_push(audio, 0, now, samples, 480);
    audio_virtual_privacy(audio, true);
    audio_read(audio, now, out, 1);
    assert(out[0] > 0.7f);
    audio_test_virtual_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
    now = cast_now_ns() - UINT64_C(1000000);
    audio_barrier(audio, false);
    audio_test_push(audio, 0, now, samples, 480);
    audio_read(audio, now, out, 1);
    assert(out[0] == 0);
    now = cast_now_ns();
    audio_test_push(audio, 0, now, samples, 480);
    audio_test_virtual_read(audio, now, out, 480);
    assert(out[0] > 0.7f);
    audio_barrier(audio, true);
    audio_test_virtual_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
    char error[CAST_ERR];
    assert(audio_configure(audio, &cfg, error, sizeof(error)) == 0);
    audio_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
    media_close(m);
    puts("audio lanes: clipped mix, independent virtual silence, stale samples and disabled lanes");
}
static struct v4l2_capability output_capability;
static int output_format_error, output_format_calls;
static int output_probe_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    if (request == VIDIOC_QUERYCAP) {
        *(struct v4l2_capability *)argument = output_capability;
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        ++output_format_calls;
        if (output_format_error) {
            errno = output_format_error;
            return -1;
        }
        struct v4l2_format *format = argument;
        assert(format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
        format->fmt.pix.bytesperline = format->fmt.pix.width * 2;
        format->fmt.pix.sizeimage = format->fmt.pix.bytesperline * format->fmt.pix.height;
        return 0;
    }
    assert(request == VIDIOC_S_PARM);
    return 0;
}
static void output_negotiation_test(void)
{
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 64;
    cfg.height = 36;
    cfg.virtual_enabled = true;
    snprintf(cfg.output_device, sizeof(cfg.output_device), "/dev/null");
    char error[CAST_ERR] = {0};
    output_capability.capabilities = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_OUTPUT;
    output_capability.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE;
    snprintf((char *)output_capability.driver, sizeof(output_capability.driver), "v4l2 loopback");
    CastOutput *output = output_test_open(&cfg, output_probe_ioctl, error, sizeof(error));
    assert(output && output_format_calls == 1);
    output_close(output);

    /* An arbitrary capture device must never reach OUTPUT negotiation, even
     * if its overall capabilities include output on a different device node. */
    snprintf((char *)output_capability.driver, sizeof(output_capability.driver), "uvcvideo");
    output_format_calls = 0;
    output = output_test_open(&cfg, output_probe_ioctl, error, sizeof(error));
    assert(!output && output_format_calls == 0);

    snprintf((char *)output_capability.driver, sizeof(output_capability.driver), "v4l2 loopback");
    output_format_error = EBUSY;
    output = output_test_open(&cfg, output_probe_ioctl, error, sizeof(error));
    assert(!output && output_format_calls == 1 && strstr(error, "busy") &&
           strstr(error, "producer"));
    output_format_calls = 0;
    output_format_error = ENODEV;
    output = output_test_open(&cfg, output_probe_ioctl, error, sizeof(error));
    assert(!output && output_format_calls == 1 && strstr(error, strerror(ENODEV)));
    output_format_calls = 0;
    output_format_error = EINVAL;
    output = output_test_open(&cfg, output_probe_ioctl, error, sizeof(error));
    assert(!output && output_format_calls == 3 && strstr(error, "locked format"));
    output_format_error = 0;
    puts("virtual output: dynamic loopback caps negotiate; physical capture rejected; "
         "busy/disappeared errors preserved");
}
static int deprecated_pixel_warnings;
static void camera_color_log(void *context, int level, const char *format, va_list args)
{
    (void)context;
    if (level > AV_LOG_WARNING) {
        return;
    }
    char message[512];
    vsnprintf(message, sizeof(message), format, args);
    if (strstr(message, "deprecated pixel format")) {
        ++deprecated_pixel_warnings;
    }
}
static void camera_color_test(void)
{
    const enum AVPixelFormat legacy[] = {AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
                                         AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P};
    const enum AVPixelFormat planar[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
                                         AV_PIX_FMT_YUV440P};
    uint8_t planes[3][256 + AV_INPUT_BUFFER_PADDING_SIZE];
    for (int i = 0; i < 256; ++i) {
        planes[0][i] = (uint8_t)i;
        planes[1][i] = (uint8_t)(80 + i / 4);
        planes[2][i] = (uint8_t)(160 - i / 4);
    }
    const uint8_t *source[4] = {planes[0], planes[1], planes[2], NULL};
    const int strides[4] = {16, 16, 16, 0};
    const int output_strides[4] = {64, 0, 0, 0};
    uint8_t old_pixels[1024], new_pixels[1024], explicit_pixels[1024];
    uint8_t *old_dst[4] = {old_pixels, NULL, NULL, NULL};
    uint8_t *new_dst[4] = {new_pixels, NULL, NULL, NULL};
    uint8_t *explicit_dst[4] = {explicit_pixels, NULL, NULL, NULL};
    int old_level = av_log_get_level();
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(camera_color_log);
    for (int matrix = 0; matrix < 2; ++matrix) {
        const int *coefficients = sws_getCoefficients(matrix ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        for (size_t i = 0; i < sizeof(legacy) / sizeof(*legacy); ++i) {
            AVFrame frame = {.width = 16,
                             .height = 16,
                             .format = legacy[i],
                             .color_range = AVCOL_RANGE_UNSPECIFIED,
                             .colorspace = matrix ? AVCOL_SPC_BT709 : AVCOL_SPC_UNSPECIFIED};
            struct SwsContext *old = sws_getContext(16, 16, legacy[i], 16, 16, AV_PIX_FMT_RGBA,
                                                    SWS_BILINEAR, NULL, NULL, NULL);
            struct SwsContext *explicit = sws_getContext(16, 16, planar[i], 16, 16, AV_PIX_FMT_RGBA,
                                                         SWS_BILINEAR, NULL, NULL, NULL);
            assert(old && explicit);
            assert(sws_setColorspaceDetails(old, coefficients, 1, coefficients, 1, 0, 1 << 16,
                                            1 << 16) == 0);
            assert(sws_setColorspaceDetails(explicit, coefficients, 1, coefficients, 1, 0, 1 << 16,
                                            1 << 16) == 0);
            int warnings = deprecated_pixel_warnings;
            struct SwsContext *normalized = camera_test_scaler(NULL, &frame);
            assert(normalized && deprecated_pixel_warnings == warnings);
            assert(sws_scale(old, source, strides, 0, 16, old_dst, output_strides) == 16);
            assert(sws_scale(explicit, source, strides, 0, 16, explicit_dst, output_strides) == 16);
            assert(sws_scale(normalized, source, strides, 0, 16, new_dst, output_strides) == 16);
            assert(!memcmp(old_pixels, new_pixels, sizeof(new_pixels)));
            assert(!memcmp(explicit_pixels, new_pixels, sizeof(new_pixels)));
            sws_freeContext(old);
            sws_freeContext(explicit);
            sws_freeContext(normalized);
        }
    }
    /* Reusing the context must also update explicit range metadata. */
    memset(planes[0], 64, sizeof(planes[0]));
    memset(planes[1], 128, sizeof(planes[1]));
    memset(planes[2], 128, sizeof(planes[2]));
    AVFrame frame = {.width = 16,
                     .height = 16,
                     .format = AV_PIX_FMT_YUV444P,
                     .color_range = AVCOL_RANGE_JPEG,
                     .colorspace = AVCOL_SPC_BT709};
    struct SwsContext *scale = camera_test_scaler(NULL, &frame);
    assert(scale && sws_scale(scale, source, strides, 0, 16, new_dst, output_strides) == 16);
    frame.color_range = AVCOL_RANGE_MPEG;
    scale = camera_test_scaler(scale, &frame);
    assert(scale && sws_scale(scale, source, strides, 0, 16, explicit_dst, output_strides) == 16);
    assert(new_pixels[0] == 64 && new_pixels[0] > explicit_pixels[0]);
    assert(new_pixels[3] == 255 && explicit_pixels[3] == 255);
    sws_freeContext(scale);
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(old_level);
    puts("webcam colors: four legacy YUVJ formats match explicit full range without warnings; "
         "range metadata updates");
}
struct CameraFixture {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int pipes[2];
    uint8_t pixels[4][8 * 4 * 3];
    struct v4l2_buffer ready[4];
    unsigned head, count, decoded, requeued, stopped;
    bool hold;
};
static int camera_fixture_ioctl(void *data, unsigned long request, void *argument)
{
    struct CameraFixture *f = data;
    pthread_mutex_lock(&f->lock);
    int result = 0;
    if (request == VIDIOC_DQBUF) {
        if (!f->count) {
            errno = EAGAIN;
            result = -1;
        } else {
            *(struct v4l2_buffer *)argument = f->ready[f->head];
            f->head = (f->head + 1) % 4;
            --f->count;
            char byte;
            assert(read(f->pipes[0], &byte, 1) == 1);
        }
    } else if (request == VIDIOC_QBUF) {
        ++f->requeued;
    } else if (request == VIDIOC_STREAMOFF) {
        ++f->stopped;
    } else {
        assert(!"unexpected camera ioctl");
    }
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return result;
}
static void camera_fixture_decoded(void *data)
{
    struct CameraFixture *f = data;
    pthread_mutex_lock(&f->lock);
    ++f->decoded;
    pthread_cond_broadcast(&f->changed);
    while (f->hold) {
        pthread_cond_wait(&f->changed, &f->lock);
    }
    pthread_mutex_unlock(&f->lock);
}
static void camera_fixture_wait(struct CameraFixture *f, const unsigned *counter, unsigned target)
{
    uint64_t deadline = cast_now_ns() + 1000000000;
    for (;;) {
        pthread_mutex_lock(&f->lock);
        bool ready = *counter >= target;
        pthread_mutex_unlock(&f->lock);
        if (ready) {
            return;
        }
        assert(cast_now_ns() < deadline);
        delay_ms(1);
    }
}
static void camera_fixture_queue(struct CameraFixture *f, unsigned index, uint64_t stamp,
                                 bool monotonic, int color)
{
    pthread_mutex_lock(&f->lock);
    assert(f->count < 4 && index < 4);
    for (size_t i = 0; i < sizeof(f->pixels[index]); i += 3) {
        f->pixels[index][i] = color == 0 ? 230 : 10;
        f->pixels[index][i + 1] = color == 1 ? 230 : 10;
        f->pixels[index][i + 2] = color == 2 ? 230 : 10;
    }
    f->ready[(f->head + f->count) % 4] =
        (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                             .memory = V4L2_MEMORY_MMAP,
                             .index = index,
                             .bytesused = sizeof(f->pixels[index]),
                             .flags = monotonic ? V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC : 0,
                             .timestamp = {.tv_sec = (time_t)(stamp / 1000000000),
                                           .tv_usec = (suseconds_t)((stamp % 1000000000) / 1000)}};
    ++f->count;
    assert(write(f->pipes[1], "f", 1) == 1);
    pthread_mutex_unlock(&f->lock);
}
static CastCamera *camera_fixture_open(struct CameraFixture *f, struct CameraTest *test)
{
    memset(f, 0, sizeof(*f));
    assert(pthread_mutex_init(&f->lock, NULL) == 0);
    assert(pthread_cond_init(&f->changed, NULL) == 0);
    assert(pipe(f->pipes) == 0);
    *test = (struct CameraTest){.fd = f->pipes[0],
                                .width = 8,
                                .height = 4,
                                .stride = 8 * 3,
                                .format = V4L2_PIX_FMT_RGB24,
                                .count = 4,
                                .ioctl = camera_fixture_ioctl,
                                .decoded = camera_fixture_decoded,
                                .data = f};
    for (unsigned i = 0; i < 4; ++i) {
        test->buffers[i] = f->pixels[i];
        test->lengths[i] = sizeof(f->pixels[i]);
    }
    char error[CAST_ERR];
    CastCamera *camera = camera_test_open(test, error, sizeof(error));
    if (!camera) {
        fprintf(stderr, "camera worker fixture: %s\n", error);
    }
    assert(camera);
    return camera;
}
static void camera_fixture_close(struct CameraFixture *f, CastCamera *camera)
{
    uint64_t before = cast_now_ns();
    camera_close(camera);
    assert(cast_now_ns() - before < 200000000);
    assert(f->stopped == 1);
    close(f->pipes[0]);
    if (f->pipes[1] >= 0) {
        close(f->pipes[1]);
    }
    pthread_cond_destroy(&f->changed);
    pthread_mutex_destroy(&f->lock);
}
static void camera_wait_frame(CastCamera *camera, Frame *frame)
{
    uint64_t deadline = cast_now_ns() + 1000000000;
    char error[CAST_ERR];
    while (camera_frame(camera, frame, error, sizeof(error)) < 0) {
        assert(!camera_failed(camera) && cast_now_ns() < deadline);
        delay_ms(1);
    }
}
static void camera_worker_test(void)
{
    struct CameraFixture fixture;
    struct CameraTest test;
    CastCamera *camera = camera_fixture_open(&fixture, &test);
    char error[CAST_ERR];
    Frame frame = {0};
    camera_fixture_queue(&fixture, 0, cast_now_ns(), true, 0);
    camera_wait_frame(camera, &frame);
    assert(frame.data[0] == 230 && frame.data[1] == 10);
    uint64_t previous = frame.ts_ns;
    assert(camera_frame(camera, &frame, error, sizeof(error)) == 0 && frame.ts_ns == previous);
    pthread_mutex_lock(&fixture.lock);
    fixture.hold = true;
    pthread_mutex_unlock(&fixture.lock);
    camera_fixture_queue(&fixture, 1, cast_now_ns(), true, 1);
    camera_fixture_wait(&fixture, &fixture.decoded, 2);
    uint64_t boundary = cast_now_ns();
    camera_barrier(camera);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0);
    assert(cast_now_ns() - boundary < 100000000);
    /* Queue an untimestamped old frame while conversion is held across privacy. */
    camera_fixture_queue(&fixture, 2, 0, false, 1);
    pthread_mutex_lock(&fixture.lock);
    fixture.hold = false;
    pthread_cond_broadcast(&fixture.changed);
    pthread_mutex_unlock(&fixture.lock);
    camera_fixture_wait(&fixture, &fixture.requeued, 3);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0);
    /* A pre-boundary exposure with no timestamp can complete after the drain. */
    camera_fixture_queue(&fixture, 3, 0, false, 1);
    camera_fixture_wait(&fixture, &fixture.requeued, 4);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0);
    /* A bogus monotonic timestamp must use the same conservative queue epoch. */
    camera_fixture_queue(&fixture, 1, UINT64_MAX, true, 1);
    camera_fixture_wait(&fixture, &fixture.requeued, 5);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0);
    /* A monotonic pre-boundary capture delivered late must also be rejected. */
    camera_fixture_queue(&fixture, 0, boundary - 10000000, true, 1);
    camera_fixture_wait(&fixture, &fixture.requeued, 6);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0);
    camera_fixture_queue(&fixture, 3, 0, false, 2);
    camera_wait_frame(camera, &frame);
    assert(frame.ts_ns >= boundary && frame.data[0] == 10 && frame.data[2] == 230);
    delay_ms(520);
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0 && strstr(error, "fresh frames"));
    close(fixture.pipes[1]);
    fixture.pipes[1] = -1;
    uint64_t deadline = cast_now_ns() + 1000000000;
    while (!camera_failed(camera)) {
        assert(cast_now_ns() < deadline);
        delay_ms(1);
    }
    assert(camera_frame(camera, &frame, error, sizeof(error)) < 0 && strstr(error, "disappeared"));
    camera_fixture_close(&fixture, camera);
    frame_free(&frame);
    /* Shutdown remains bounded when the worker is waiting on an idle device. */
    camera = camera_fixture_open(&fixture, &test);
    delay_ms(5);
    camera_fixture_close(&fixture, camera);
    puts("webcam worker: latest-frame handoff, in-flight privacy, queued and late unknown "
         "timestamps, malformed and old capture rejection, stale timeout, disconnect, and "
         "bounded shutdown passed");
}
static void webcam_probe(void)
{
    const char *path = getenv("CAST_TEST_CAMERA");
    if (!path) {
        return;
    }
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 1920;
    cfg.height = 1080;
    snprintf(cfg.camera_device, sizeof(cfg.camera_device), "%s", path);
    char error[CAST_ERR] = {0};
    CastCamera *camera = camera_open(&cfg, error, sizeof(error));
    if (!camera) {
        fprintf(stderr, "physical webcam probe failed: %s\n", error);
    }
    assert(camera);
    Frame frame = {0};
    int captured = 0;
    uint64_t previous = 0;
    for (int i = 0; i < 100 && captured < 3; ++i) {
        if (camera_frame(camera, &frame, error, sizeof(error)) == 0 && frame.ts_ns != previous) {
            previous = frame.ts_ns;
            ++captured;
        }
        delay_ms(35);
    }
    assert(captured == 3 && frame.width > 0 && frame.height > 0 && frame.stride >= frame.width * 4);
    /* Queue a backlog, then prove a privacy boundary rejects those captures. */
    delay_ms(150);
    uint64_t boundary = cast_now_ns();
    camera_barrier(camera);
    bool fresh = false;
    for (int i = 0; i < 100 && !fresh; ++i) {
        if (camera_frame(camera, &frame, error, sizeof(error)) == 0) {
            assert(frame.ts_ns >= boundary);
            fresh = true;
        }
        delay_ms(35);
    }
    assert(fresh);
    uint64_t total = 0, longest = 0;
    int delivered = 0, unique = 0;
    previous = frame.ts_ns;
    for (int i = 0; i < 90; ++i) {
        uint64_t start = cast_now_ns();
        int result = camera_frame(camera, &frame, error, sizeof(error));
        uint64_t elapsed = cast_now_ns() - start;
        total += elapsed;
        if (elapsed > longest) {
            longest = elapsed;
        }
        if (result == 0) {
            ++delivered;
            if (frame.ts_ns != previous) {
                ++unique;
                previous = frame.ts_ns;
            }
        }
        delay_ms(33);
    }
    assert(delivered >= 85 && unique >= 10);
    printf("physical webcam %s: RGBA %dx%d, %d/90 reads (%d unique), main read mean %.3f ms "
           "max %.3f ms; pre-barrier backlog rejected\n",
           path, frame.width, frame.height, delivered, unique, total / 90e6, longest / 1e6);
    frame_free(&frame);
    camera_close(camera);
}
static void codec_selection_test(void)
{
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 160;
    cfg.height = 90;
    char error[CAST_ERR];
    AVCodecContext *context = NULL;
    MediaCodecSelection selection;
    assert(media_video_open(&cfg, false, true, &context, &selection, error, sizeof(error)) == 0);
    assert(context && selection.encoder[0] && selection.detail[0]);
    printf("recording codec probe: %s; %s\n", selection.encoder, selection.detail);
    avcodec_free_context(&context);
    snprintf(cfg.video_codec, sizeof(cfg.video_codec), "cast_missing_encoder_fixture");
    assert(media_video_open(&cfg, false, true, &context, &selection, error, sizeof(error)) < 0);
    assert(context == NULL && strstr(error, "cast_missing_encoder_fixture"));
    snprintf(cfg.stream.video_encoder, sizeof(cfg.stream.video_encoder), "auto");
    assert(!media_video_open(&cfg, true, true, &context, &selection, error, sizeof(error)));
    assert(context->codec_id == AV_CODEC_ID_H264);
    assert(context->bit_rate == (int64_t)cfg.stream.video_bitrate_kbps * 1000);
    avcodec_free_context(&context);
    snprintf(cfg.stream.video_encoder, sizeof(cfg.stream.video_encoder), "mpeg4");
    assert(media_video_open(&cfg, true, true, &context, &selection, error, sizeof(error)) < 0);
    assert(context == NULL && strstr(error, "H.264"));
    if (avcodec_find_encoder_by_name("libopenh264")) {
        snprintf(cfg.video_codec, sizeof(cfg.video_codec), "libopenh264");
        snprintf(cfg.record_rate_control, sizeof(cfg.record_rate_control), "crf");
        assert(media_video_open(&cfg, false, true, &context, &selection, error, sizeof(error)) < 0);
        assert(context == NULL && strstr(error, "not translated"));
        snprintf(cfg.record_rate_control, sizeof(cfg.record_rate_control), "bitrate");
        assert(media_video_open(&cfg, false, true, &context, &selection, error, sizeof(error)) == 0);
        assert(!strcmp(selection.encoder, "libopenh264") && strstr(selection.detail, "inactive"));
        assert(context->bit_rate == (int64_t)cfg.record_bitrate_kbps * 1000);
        avcodec_free_context(&context);
    }
    const char *license = avcodec_license();
    int validation = media_profile_validate(true, error, sizeof(error));
    assert((strncmp(license, "LGPL", 4) != 0) ? validation < 0 : validation == 0);
    char report[8192] = "";
    media_profile_report(report, sizeof(report));
    assert(strstr(report, "FFmpeg avcodec:") && strstr(report, "FFmpeg configuration:"));
}
static void flv_codec_test(const char *directory)
{
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 160;
    cfg.height = 90;
    cfg.fps = 30;
    cfg.pause_color = 0x0000ff;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/stream-codecs.flv", directory);
    assert(stream_test_file(&cfg, path) == 0);
    struct Inspection decoded = inspect(path);
    assert(decoded.video_frames == 30 && decoded.red == 15 && decoded.blue == 15);
    assert(decoded.video_last > 0.95 && decoded.video_last < 1.05);
    struct Inspection audible = inspect_between(path, 0.1, 0.4);
    struct Inspection silent = inspect_between(path, 0.7, 0.9);
    assert(audible.window_samples > 0 && audible.window_power / audible.window_samples > 0.01);
    assert(silent.window_samples > 0 && silent.window_power / silent.window_samples < 0.000001);
    printf("production streaming codecs: H.264/AAC FLV file, 30 decoded frames, "
           "continuous timestamps and privacy epoch keyframe passed (network untested)\n");
}
static void container_codec_test(const char *directory, const char *container,
                                 const char *video, const char *sound)
{
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 160;
    cfg.height = 90;
    snprintf(cfg.record_container, sizeof(cfg.record_container), "%s", container);
    snprintf(cfg.video_codec, sizeof(cfg.video_codec), "%s", video);
    snprintf(cfg.audio_codec, sizeof(cfg.audio_codec), "%s", sound);
    CastAudio *audio = audio_test_open(&cfg);
    CastRecorder *recorder = recorder_open(audio);
    assert(audio && recorder);
    char path[PATH_MAX], error[CAST_ERR], selected[64], detail[CAST_ERR];
    snprintf(path, sizeof(path), "%s/%s-%s.media", directory, container, video);
    assert(!recorder_start(recorder, &cfg, path, error, sizeof(error)));
    recorder_encoder(recorder, selected, sizeof(selected), detail, sizeof(detail));
    assert(selected[0] && detail[0]);
    if (strcmp(video, "auto")) {
        assert(!strcmp(selected, video));
    }
    Frame frame = {0};
    assert(!frame_alloc(&frame, cfg.width, cfg.height));
    paint(&frame, 0);
    for (unsigned i = 0; i < 8; i++) {
        frame.ts_ns = cast_now_ns();
        assert(recorder_frame(recorder, &frame, error, sizeof(error)) >= 0);
        delay_ms(33);
    }
    assert(!recorder_stop(recorder, error, sizeof(error)));
    while (recorder_finalizing(recorder)) {
        delay_ms(10);
    }
    bool active, paused;
    uint64_t drops;
    recorder_status(recorder, &active, &paused, &drops, error, sizeof(error));
    assert(!active && !error[0]);
    struct Inspection result = inspect(path);
    assert(result.video_frames >= 6 && result.audio_frames > 0 && result.red >= 6);
    printf("container roundtrip: %s %s/%s, %d decoded video frames; %s\n",
           container, selected, sound, result.video_frames, detail);
    frame_free(&frame);
    recorder_close(recorder);
    audio_close(audio);
}
static void image_decode_test(const char *directory)
{
    /* Original solid-red format fixtures; no external images or camera access. */
    static const uint8_t webp[] = {
        0x52, 0x49, 0x46, 0x46, 0x1c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
        0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x01, 0x40, 0x00,
        0x00, 0x07, 0x10, 0xf5, 0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00
    };
    static const uint8_t bmp[] = {
        0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
        0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x00, 0xfd,
        0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x00, 0xfd, 0x00, 0x00
    };
    const uint8_t *bytes[] = {webp, bmp};
    const size_t lengths[] = {sizeof(webp), sizeof(bmp)};
    const char *names[] = {"webp", "bmp"};
    char path[PATH_MAX], error[CAST_ERR];
    for (unsigned i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/logo.%s", directory, names[i]);
        FILE *file = fopen(path, "wb");
        assert(file && fwrite(bytes[i], 1, lengths[i], file) == lengths[i]);
        assert(!fclose(file));
        Frame decoded = {0};
        assert(!composition_logo_load(path, &decoded, error, sizeof(error)));
        assert(decoded.width == 2 && decoded.height == 2 && decoded.data[0] > 200);
        frame_free(&decoded);
    }
    const AVCodec *jpeg = avcodec_find_encoder_by_name("mjpeg");
    AVCodecContext *encoder = avcodec_alloc_context3(jpeg);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    assert(jpeg && encoder && frame && packet);
    encoder->width = encoder->height = 16;
    encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encoder->time_base = (AVRational){1, 30};
    assert(!avcodec_open2(encoder, jpeg, NULL));
    frame->width = frame->height = 16;
    frame->format = encoder->pix_fmt;
    assert(!av_frame_get_buffer(frame, 32));
    for (unsigned plane = 0; plane < 3; plane++) {
        int height = plane ? 8 : 16;
        memset(frame->data[plane], plane == 0 ? 76 : plane == 1 ? 85 : 255,
               (size_t)height * frame->linesize[plane]);
    }
    assert(!avcodec_send_frame(encoder, frame));
    assert(!avcodec_receive_packet(encoder, packet));
    snprintf(path, sizeof(path), "%s/logo.jpg", directory);
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(packet->data, 1, (size_t)packet->size, file) == (size_t)packet->size);
    assert(!fclose(file));
    Frame decoded = {0};
    assert(!composition_logo_load(path, &decoded, error, sizeof(error)));
    assert(decoded.width == 16 && decoded.height == 16 && decoded.data[0] > 200);
    frame_free(&decoded);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    printf("configured images: real JPEG/MJPEG, lossless WebP and BMP decode to owned RGBA passed\n");
}
static void video_conversion_test(void)
{
    MediaVideoConverter *conversion = NULL;
    uint32_t random = 0x53731;
    /* Reuse/reconfigure one converter across padded inputs, changing source
     * dimensions and both original color matrices. Compare every valid output
     * sample against the pre-optimization single-thread scaler, including the
     * worker slice boundaries and nonuniform chroma detail. */
    for (unsigned shape = 0; shape < 3; shape++) {
        Frame source = {.width = shape == 1 ? 640 : 1280,
                        .height = shape == 1 ? 360 : shape == 2 ? 722 : 720};
        source.stride = source.width * 4 + 128;
        source.data = malloc((size_t)source.stride * source.height);
        assert(source.data);
        AVFrame *actual = av_frame_alloc(), *reference = av_frame_alloc();
        assert(actual && reference);
        actual->width = reference->width = 1280;
        actual->height = reference->height = shape == 2 ? 722 : 720;
        actual->format = reference->format = AV_PIX_FMT_YUV420P;
        assert(!av_frame_get_buffer(actual, 32) && !av_frame_get_buffer(reference, 32));
        for (unsigned matrix = 0; matrix < 2; matrix++) {
            struct SwsContext *old = sws_getContext(
                source.width, source.height, AV_PIX_FMT_RGBA, actual->width, actual->height,
                actual->format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            assert(old);
            if (matrix) {
                const int *colors = sws_getCoefficients(SWS_CS_ITU709);
                assert(!sws_setColorspaceDetails(old, colors, 1, colors, 0,
                                                0, 1 << 16, 1 << 16));
            }
            for (unsigned frame = 0; frame < 3; frame++) {
                for (int y = 0; y < source.height; y++) {
                    uint8_t *row = source.data + (size_t)y * source.stride;
                    for (int x = 0; x < source.width * 4; x++) {
                        random = random * 1664525U + 1013904223U;
                        row[x] = (uint8_t)(random >> 24);
                    }
                }
                assert(!av_frame_make_writable(actual));
                assert(!media_video_convert(&conversion, &source, actual, matrix != 0));
                const uint8_t *pixels[4] = {source.data};
                int strides[4] = {source.stride};
                assert(sws_scale(old, pixels, strides, 0, source.height, reference->data,
                                 reference->linesize) == reference->height);
                for (unsigned plane = 0; plane < 3; plane++) {
                    int width = plane ? actual->width / 2 : actual->width;
                    int height = plane ? actual->height / 2 : actual->height;
                    for (int y = 0; y < height; y++) {
                        assert(!memcmp(actual->data[plane] + (size_t)y * actual->linesize[plane],
                                       reference->data[plane] + (size_t)y * reference->linesize[plane],
                                       (size_t)width));
                    }
                }
                assert(av_buffer_get_ref_count(actual->buf[0]) == 1);
            }
            sws_freeContext(old);
        }
        frame_free(&source);
        av_frame_free(&actual);
        av_frame_free(&reference);
    }
    media_video_converter_free(&conversion);
    assert(!conversion);
    printf("video conversion: padded/resized RGBA and both color matrices match legacy YUV exactly; no retained destination refs\n");
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_ERROR);
    if (argc == 2 && !strcmp(argv[1], "--camera-worker")) {
        camera_worker_test();
        return 0;
    }
    codec_selection_test();
    video_conversion_test();
    char directory[] = "/tmp/cast-media-test-XXXXXX";
    assert(mkdtemp(directory));
    image_decode_test(directory);
    flv_codec_test(directory);
    container_codec_test(directory, "mp4", "auto", "aac");
    container_codec_test(directory, "matroska", "ffv1", "flac");
    timeline_test(directory, true);
    timeline_test(directory, false);
    privacy_test(directory);
    solid_and_cut_test(directory);
    failure_test(directory);
    responsiveness_test(directory);
    disk_failure_test(directory);
    audio_lanes_test();
    output_negotiation_test();
    camera_color_test();
    camera_worker_test();
    webcam_probe();
    /* Test artifacts deliberately remain available for ffprobe/visual inspection. */
    printf("media tests passed; inspection artifacts: %s\n", directory);
    return 0;
}
