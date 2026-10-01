#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast.h"
#include "media_internal.h"
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
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
        assert(media_live(m, f, false, error, sizeof(error)) == 0);
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
static struct Inspection inspect(const char *path)
{
    struct Inspection out = {0};
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
static void timeline_test(const char *directory, bool live)
{
    Config cfg;
    Media *m = new_media(&cfg);
    cfg.live_enabled = live;
    Frame f = {0};
    assert(frame_alloc(&f, cfg.width, cfg.height) == 0);
    char path[PATH_MAX], error[CAST_ERR], actual[PATH_MAX];
    snprintf(path, sizeof(path), "%s/timeline-%d.mkv", directory, live);
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
           live ? "+ live" : "only", x.video_frames, active, x.audio_last, x.max_video_gap);
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
    media_privacy(m, true, false, true);
    float silence[960];
    audio_read(media_test_audio(m), before, silence, 480);
    for (int i = 0; i < 960; ++i) {
        assert(silence[i] == 0);
    }
    recorder_test_slow(media_test_recorder(m), 0);
    delay_ms(220);
    media_privacy(m, false, false, false);
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
    assert(media_live(m, &f, true, error, sizeof(error)) == 0);
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
    puts("encoder failure isolates live output, permits restart, and failed reconfigure preserves "
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
    /* Exercise live-only before starting simultaneous operation. */
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
    assert(media_live(m, &f, false, error, sizeof(error)) == 0);
    assert(media_record_stop(m, error, sizeof(error)) == 0);
    assert(media_record_finalizing(m));
    /* Live privacy must not wait on the recording's drain/trailer/fsync. */
    media_privacy(m, true, false, true);
    assert(media_live(m, &f, true, error, sizeof(error)) == 0);
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
    printf("slow encoder: bounded queue dropped %llu frames; status/live/stop/privacy %.2f ms\n",
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
    assert(media_live(m, &f, false, error, sizeof(error)) == 0);
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
    puts("ENOSPC write failure: partial file retained, live continues, next recording decodes");
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
    /* Virtual audio is independently silent during live pause/freeze while the
     * recording mix is available, then resumes only from new captured buffers. */
    audio_test_virtual_read(audio, now, out, 480);
    for (int i = 0; i < 960; ++i) {
        assert(out[i] == 0);
    }
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
static void webcam_probe(void)
{
    const char *path = getenv("CAST_TEST_CAMERA");
    if (!path) {
        return;
    }
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 1280;
    cfg.height = 720;
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
    printf(
        "physical webcam %s: decoded unique RGBA %dx%d frames and rejected pre-barrier backlog\n",
        path, frame.width, frame.height);
    frame_free(&frame);
    camera_close(camera);
}
int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    char directory[] = "/tmp/cast-media-test-XXXXXX";
    assert(mkdtemp(directory));
    timeline_test(directory, true);
    timeline_test(directory, false);
    privacy_test(directory);
    failure_test(directory);
    responsiveness_test(directory);
    disk_failure_test(directory);
    audio_lanes_test();
    camera_color_test();
    webcam_probe();
    /* Test artifacts deliberately remain available for ffprobe/visual inspection. */
    printf("media tests passed; inspection artifacts: %s\n", directory);
    return 0;
}
