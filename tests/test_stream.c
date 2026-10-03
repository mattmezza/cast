#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast.h"
#include "media_internal.h"
#include "stream.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static char fixture_key[65];
uint64_t cast_now_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000ULL + (uint64_t)time.tv_nsec;
}
static void wait_ms(int milliseconds)
{
    struct timespec pause = {.tv_sec = milliseconds / 1000,
                             .tv_nsec = milliseconds % 1000 * 1000000L};
    nanosleep(&pause, NULL);
}
static void key_tests(const char *directory)
{
    char path[PATH_MAX], other[PATH_MAX], error[CAST_ERR];
    snprintf(path, sizeof(path), "%s/key", directory);
    snprintf(other, sizeof(other), "%s/other", directory);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, fixture_key, strlen(fixture_key)) == (ssize_t)strlen(fixture_key));
    close(fd);
    assert(stream_key_validate(path, error, sizeof(error)) == 0);
    assert(chmod(path, 0640) == 0);
    assert(stream_key_validate(path, error, sizeof(error)) < 0);
    assert(strstr(error, "600"));
    assert(chmod(path, 0600) == 0);
    assert(symlink(path, other) == 0);
    assert(stream_key_validate(other, error, sizeof(error)) < 0);
    unlink(other);
    assert(link(path, other) == 0);
    assert(stream_key_validate(path, error, sizeof(error)) < 0);
    unlink(other);
    fd = open(path, O_TRUNC | O_WRONLY);
    assert(write(fd, "line\nsecond", 11) == 11);
    close(fd);
    assert(stream_key_validate(path, error, sizeof(error)) < 0);
    fd = open(path, O_TRUNC | O_WRONLY);
    char oversized[1026];
    memset(oversized, 'X', sizeof(oversized));
    assert(write(fd, oversized, sizeof(oversized)) == sizeof(oversized));
    close(fd);
    assert(stream_key_validate(path, error, sizeof(error)) < 0);
    unlink(path);
    assert(mkfifo(path, 0600) == 0);
    uint64_t before = cast_now_ns();
    assert(stream_key_validate(path, error, sizeof(error)) < 0);
    assert(cast_now_ns() - before < 100000000ULL);
    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY, 0600);
    assert(write(fd, fixture_key, strlen(fixture_key)) == (ssize_t)strlen(fixture_key));
    close(fd);
}
static unsigned free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    unsigned port = ntohs(address.sin_port);
    close(fd);
    return port;
}
static pid_t ingest_start(unsigned port, const char *output)
{
    char url[128];
    snprintf(url, sizeof(url), "rtmp://127.0.0.1:%u/ingest", port);
    char *arguments[] = {"ffmpeg", "-hide_banner", "-loglevel", "quiet", "-y",   "-listen",
                         "1",      "-i",           url,         "-c",    "copy", "-f",
                         "flv",    (char *)output, NULL};
    pid_t child;
    assert(posix_spawnp(&child, "ffmpeg", NULL, NULL, arguments, environ) == 0);
    wait_ms(250);
    return child;
}
struct Tone {
    CastAudio *audio;
    atomic_bool stop;
};
static void *tone_main(void *opaque)
{
    struct Tone *tone = opaque;
    uint64_t next = cast_now_ns(), index = 0;
    while (!atomic_load(&tone->stop)) {
        float samples[480 * 2];
        for (int i = 0; i < 480; i++) {
            float sample =
                sinf((float)((index + (unsigned)i) % 48000) * 6.2831853f * 440.f / 48000.f) * 0.3f;
            samples[i * 2] = samples[i * 2 + 1] = sample;
        }
        audio_test_push(tone->audio, 0, next - 10000000ULL, samples, 480);
        index += 480;
        next += 10000000ULL;
        struct timespec deadline = {.tv_sec = next / 1000000000ULL,
                                    .tv_nsec = next % 1000000000ULL};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    return NULL;
}

static void paint(Frame *frame, int color)
{
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            uint8_t *pixel = frame->data + (size_t)y * frame->stride + x * 4;
            pixel[0] = color == 0 ? 230 : 10;
            pixel[1] = color == 1 ? 230 : 10;
            pixel[2] = color == 2 ? 230 : 10;
            pixel[3] = 255;
        }
    }
    frame->ts_ns = cast_now_ns();
}
static void feed(CastStream *stream, Frame *frame, int color, bool silent, int milliseconds)
{
    char error[CAST_ERR];
    uint64_t deadline = cast_now_ns() + (uint64_t)milliseconds * 1000000ULL;
    while (cast_now_ns() < deadline) {
        paint(frame, color);
        assert(stream_frame(stream, frame, silent, error, sizeof(error)) == 0);
        wait_ms(33);
    }
}
struct Decoded {
    int video_count, audio_count, colors[3];
    double video_last, audio_last, max_gap, energy, quiet_energy, late_energy, late_quiet;
    int late_samples, late_quiet_samples;
    int active_samples, quiet_samples, forbidden;
};
static struct Decoded inspect(const char *path)
{
    struct Decoded result = {0};
    AVFormatContext *format = NULL;
    assert(avformat_open_input(&format, path, NULL, NULL) == 0);
    assert(avformat_find_stream_info(format, NULL) == 0);
    AVCodecContext *decoder[2] = {NULL};
    int indices[2] = {-1, -1};
    for (unsigned i = 0; i < format->nb_streams; i++) {
        int lane = format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? 0 : 1;
        const AVCodec *codec = avcodec_find_decoder(format->streams[i]->codecpar->codec_id);
        decoder[lane] = avcodec_alloc_context3(codec);
        assert(decoder[lane]);
        assert(avcodec_parameters_to_context(decoder[lane], format->streams[i]->codecpar) == 0);
        assert(avcodec_open2(decoder[lane], codec, NULL) == 0);
        indices[lane] = (int)i;
    }
    assert(decoder[0] && decoder[1]);
    assert(decoder[0]->codec_id == AV_CODEC_ID_H264 && decoder[1]->codec_id == AV_CODEC_ID_AAC);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *scale = NULL;
    Frame pixels = {0};
    assert(frame_alloc(&pixels, 160, 90) == 0);
    int64_t last_dts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    while (av_read_frame(format, packet) >= 0) {
        int lane = packet->stream_index == indices[0] ? 0 : 1;
        assert(last_dts[lane] == AV_NOPTS_VALUE || packet->dts > last_dts[lane]);
        last_dts[lane] = packet->dts;
        if (!lane && !result.video_count) {
            assert(packet->flags & AV_PKT_FLAG_KEY);
        }
        assert(avcodec_send_packet(decoder[lane], packet) == 0);
        while (avcodec_receive_frame(decoder[lane], frame) == 0) {
            double seconds =
                frame->best_effort_timestamp * av_q2d(format->streams[indices[lane]]->time_base);
            if (!lane) {
                if (result.video_count) {
                    double gap = seconds - result.video_last;
                    assert(gap > 0);
                    if (gap > result.max_gap) {
                        result.max_gap = gap;
                    }
                }
                result.video_last = seconds;
                result.video_count++;
                scale =
                    sws_getCachedContext(scale, frame->width, frame->height, frame->format, 160, 90,
                                         AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                uint8_t *data[] = {pixels.data};
                int strides[] = {pixels.stride};
                assert(sws_scale(scale, (const uint8_t *const *)frame->data, frame->linesize, 0,
                                 frame->height, data, strides) == 90);
                uint8_t *pixel = pixels.data + 45 * pixels.stride + 80 * 4;
                int color = pixel[0] > pixel[1] && pixel[0] > pixel[2] ? 0
                            : pixel[1] > pixel[2]                      ? 1
                                                                       : 2;
                result.colors[color]++;
                /* Red raw source is retired on pause at approximately 1.1s.
                 * Initial red frames are allowed only before that boundary. */
                if (seconds > 1.5 && color == 0) {
                    result.forbidden++;
                }
            } else {
                result.audio_last = seconds;
                result.audio_count++;
                assert(frame->format == AV_SAMPLE_FMT_FLTP);
                for (int i = 0; i < frame->nb_samples; i++) {
                    float sample = ((float *)frame->data[0])[i];
                    if (seconds > 0.2 && seconds < 0.9) {
                        result.energy += sample * sample;
                        result.active_samples++;
                    }
                    if (seconds > 50 && seconds < 55) {
                        result.late_energy += sample * sample;
                        result.late_samples++;
                    }
                    if (seconds > 61 && seconds < 64) {
                        result.late_quiet += sample * sample;
                        result.late_quiet_samples++;
                    }
                    if (seconds > 1.5 && seconds < 2.5) {
                        result.quiet_energy += sample * sample;
                        result.quiet_samples++;
                    }
                }
            }
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
    }
    frame_free(&pixels);
    sws_freeContext(scale);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder[0]);
    avcodec_free_context(&decoder[1]);
    avformat_close_input(&format);
    return result;
}
static void wait_state(CastStream *stream, StreamLifecycle expected, int milliseconds)
{
    uint64_t deadline = cast_now_ns() + (uint64_t)milliseconds * 1000000ULL;
    StreamSnapshot status;
    do {
        stream_status(stream, &status);
        if (status.state == expected) {
            return;
        }
        wait_ms(5);
    } while (cast_now_ns() < deadline);
    fprintf(stderr, "wanted %s, got %s: %s\n", stream_lifecycle_name(expected),
            stream_lifecycle_name(status.state), status.error);
    assert(false);
}
static void ingest_test(Config *config, CastAudio *audio, const char *directory)
{
    char output[PATH_MAX], error[CAST_ERR];
    snprintf(output, sizeof(output), "%s/received.flv", directory);
    unsigned port = free_port();
    pid_t ingest = ingest_start(port, output);
    snprintf(config->stream.server_url, sizeof(config->stream.server_url),
             "rtmp://127.0.0.1:%u/ingest", port);
    CastStream *stream = stream_open(audio);
    assert(stream);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    assert(stream_start(stream, config, error, sizeof(error)) < 0);
    Frame frame = {0};
    assert(frame_alloc(&frame, config->width, config->height) == 0);
    struct Tone tone = {.audio = audio};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, tone_main, &tone) == 0);
    stream_privacy(stream, false);
    feed(stream, &frame, 0, false, 1100);
    wait_state(stream, STREAM_STREAMING, 2000);
    uint64_t before = cast_now_ns();
    stream_privacy(stream, true);
    double privacy_ms = (cast_now_ns() - before) / 1e6;
    assert(privacy_ms < 100);
    feed(stream, &frame, 1, true, 1600);
    /* Hold a converted-but-unsubmitted frame independently of network I/O.
     * A red sensitive job enters the raw queue, then a new epoch retires it
     * while the worker is held. Neither the raw queue nor the local conversion
     * may submit that red frame after acknowledgement. */
    stream_test_stage(stream, 3);
    stream_test_hold(stream, 200);
    paint(&frame, 0);
    assert(stream_frame(stream, &frame, true, error, sizeof(error)) == 0);
    uint64_t held_deadline = cast_now_ns() + 1000000000ULL;
    while (!stream_test_blocked(stream) && cast_now_ns() < held_deadline) {
        wait_ms(2);
    }
    assert(stream_test_blocked(stream));
    /* The child publishes its new phase after an older supervisor clock
     * sample. A future timestamp must not cause an unsigned false timeout. */
    stream_test_clock_delay(stream, 1000);
    wait_ms(40);
    StreamSnapshot healthy;
    stream_status(stream, &healthy);
    assert(healthy.state == STREAM_STREAMING && healthy.retry_attempt == 0);
    stream_test_clock_delay(stream, 0);
    before = cast_now_ns();
    stream_privacy(stream, true);
    assert(cast_now_ns() - before < 100000000ULL);
    feed(stream, &frame, 1, true, 500);
    stream_privacy(stream, false);
    feed(stream, &frame, 2, false, 1100);
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.retry_attempt == 0 &&
           status.queue_depth <= (unsigned)config->stream.queue_frames &&
           status.bytes_written > 1000);
    before = cast_now_ns();
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    double stop_ms = (cast_now_ns() - before) / 1e6;
    assert(stop_ms < 100);
    wait_state(stream, STREAM_STOPPED, 1000);
    atomic_store(&tone.stop, true);
    pthread_join(thread, NULL);
    stream_close(stream);
    int child_status;
    uint64_t deadline = cast_now_ns() + 3000000000ULL;
    while (waitpid(ingest, &child_status, WNOHANG) == 0 && cast_now_ns() < deadline) {
        wait_ms(10);
    }
    if (waitpid(ingest, &child_status, WNOHANG) == 0) {
        kill(ingest, SIGKILL);
        waitpid(ingest, &child_status, 0);
    }
    struct Decoded decoded = inspect(output);
    fprintf(stderr,
            "stream decoded: frames=%d audio=%d colors=%d/%d/%d maxgap=%.3f AVdelta=%.3f RMS=%.4f "
            "quiet=%.6f privacy=%.3fms stop=%.3fms\n",
            decoded.video_count, decoded.audio_count, decoded.colors[0], decoded.colors[1],
            decoded.colors[2], decoded.max_gap, fabs(decoded.video_last - decoded.audio_last),
            sqrt(decoded.energy / decoded.active_samples),
            sqrt(decoded.quiet_energy / decoded.quiet_samples), privacy_ms, stop_ms);
    assert(decoded.video_count > 90 && decoded.audio_count > 100);
    assert(decoded.colors[0] > 15 && decoded.colors[1] > 30 && decoded.colors[2] > 15 &&
           decoded.forbidden == 0);
    assert(decoded.max_gap < 0.08 && fabs(decoded.video_last - decoded.audio_last) < 0.1);
    assert(decoded.active_samples > 10000 && decoded.energy / decoded.active_samples > 0.001);
    assert(decoded.quiet_samples > 10000 &&
           decoded.quiet_energy / decoded.quiet_samples < 0.000001);
    frame_free(&frame);
    unlink(output);
}

static void sustained_test(Config *config, CastAudio *audio, const char *directory)
{
    char output[PATH_MAX], recording[PATH_MAX], error[CAST_ERR];
    snprintf(output, sizeof(output), "%s/sustained.flv", directory);
    snprintf(recording, sizeof(recording), "%s/parallel.mkv", directory);
    unsigned port = free_port();
    pid_t ingest = ingest_start(port, output);
    snprintf(config->stream.server_url, sizeof(config->stream.server_url),
             "rtmp://127.0.0.1:%u/ingest", port);
    config->width = 1280;
    config->height = 720;
    config->stream.video_bitrate_kbps = 4000;
    config->stream.lag_ms = 500;
    CastStream *stream = stream_open(audio);
    CastRecorder *recorder = recorder_open(audio);
    assert(stream && recorder);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    stream_privacy(stream, false);
    struct Tone tone = {.audio = audio};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, tone_main, &tone) == 0);
    Frame frame = {0}, recording_frame = {0};
    assert(frame_alloc(&frame, 1280, 720) == 0 && frame_alloc(&recording_frame, 1280, 720) == 0);
    bool record_started = false, paused = false;
    uint64_t start = cast_now_ns(), next = start;
    printf("{\"phase\":\"streaming\"}\n");
    fflush(stdout);
    unsigned frame_index = 0;
    while (cast_now_ns() - start < 65000000000ULL) {
        uint64_t elapsed = cast_now_ns() - start;
        StreamSnapshot current;
        stream_status(stream, &current);
        if (current.state == STREAM_FAILED) {
            fprintf(stderr, "sustained failed at %.3fs: %s\n", elapsed / 1e9, current.error);
            assert(false);
        }
        if (!record_started && elapsed > 10000000000ULL) {
            assert(recorder_start(recorder, config, recording, error, sizeof(error)) == 0);
            record_started = true;
            printf("{\"phase\":\"simultaneous\"}\n");
            fflush(stdout);
        }
        if (!paused && elapsed > 60000000000ULL) {
            stream_privacy(stream, true);
            paused = true;
        }
        /* Synthetic moving checkerboard exercises independent software codecs;
         * this benchmark does not include capture/composition or a real device. */
        for (int y = 0; y < 720; y++) {
            for (int x = 0; x < 1280; x++) {
                uint8_t *pixel = recording_frame.data + (size_t)y * recording_frame.stride + x * 4;
                unsigned shade = ((x + frame_index * 2) / 32 + y / 32) & 1;
                pixel[0] = shade ? 20 : 30;
                pixel[1] = shade ? 200 : 100;
                pixel[2] = shade ? 40 : 70;
                pixel[3] = 255;
            }
        }
        recording_frame.ts_ns = cast_now_ns();
        if (paused) {
            paint(&frame, 2);
        } else {
            assert(frame_copy(&frame, &recording_frame) == 0);
        }
        assert(stream_frame(stream, &frame, paused, error, sizeof(error)) == 0);
        if (record_started) {
            assert(recorder_frame(recorder, &recording_frame, error, sizeof(error)) >= 0);
        }
        frame_index++;
        next += 1000000000ULL / 30;
        struct timespec deadline = {.tv_sec = next / 1000000000ULL,
                                    .tv_nsec = next % 1000000000ULL};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.state == STREAM_STREAMING && status.retry_attempt == 0);
    printf("{\"phase\":\"decoding\"}\n");
    fflush(stdout);
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    recorder_stop(recorder, error, sizeof(error));
    stream_close(stream);
    recorder_close(recorder);
    atomic_store(&tone.stop, true);
    pthread_join(thread, NULL);
    waitpid(ingest, NULL, 0);
    struct Decoded decoded = inspect(output), local = inspect(recording);
    /* Retiring an unsubmitted frame at the privacy boundary can leave one
     * two-frame interval; the paused lane then continues at the same 30fps. */
    assert(decoded.video_count > 1900 && decoded.max_gap < 0.08);
    assert(fabs(decoded.video_last - decoded.audio_last) < 0.1);
    assert(decoded.late_samples > 100000 && decoded.late_energy / decoded.late_samples > 0.001);
    assert(decoded.late_quiet_samples > 100000 &&
           decoded.late_quiet / decoded.late_quiet_samples < 0.000001);
    assert(local.video_count > 1600 && fabs(local.video_last - local.audio_last) < 0.1);
    assert(local.quiet_samples > 10000 && local.quiet_energy / local.quiet_samples > 0.001);
    printf("{\"phase\":\"complete\",\"duration\":%.3f,\"frames\":%d,\"max_frame_gap\":%.3f,\"av_"
           "delta\":%.3f,\"late_rms\":%.4f,\"paused_rms\":%.6f,\"record_frames\":%d,\"stream_"
           "drops\":%llu}\n",
           decoded.video_last, decoded.video_count, decoded.max_gap,
           fabs(decoded.video_last - decoded.audio_last),
           sqrt(decoded.late_energy / decoded.late_samples),
           sqrt(decoded.late_quiet / decoded.late_quiet_samples), local.video_count,
           (unsigned long long)status.dropped_frames);
    frame_free(&frame);
    frame_free(&recording_frame);
    unlink(output);
    unlink(recording);
}
static void audio_independence_test(CastAudio *audio)
{
    float source[480 * 2], recording[480 * 2], virtual_mix[480 * 2], streaming[480 * 2];
    for (unsigned i = 0; i < sizeof(source) / sizeof(source[0]); i++) {
        source[i] = 0.25f;
    }
    audio_virtual_privacy(audio, false);
    uint64_t timestamp = cast_now_ns() + 10000000ULL;
    audio_test_push(audio, 0, timestamp, source, 480);
    audio_stream_privacy(audio);
    audio_stream_read(audio, timestamp, streaming, 480);
    audio_read(audio, timestamp, recording, 480);
    audio_test_virtual_read(audio, timestamp, virtual_mix, 480);
    for (int i = 0; i < 480 * 2; i++) {
        assert(streaming[i] == 0 && recording[i] == 0.25f && virtual_mix[i] == 0.25f);
    }
}
static void reconnect_test(Config *config, CastAudio *audio, const char *directory)
{
    char first[PATH_MAX], second[PATH_MAX], error[CAST_ERR];
    snprintf(first, sizeof(first), "%s/disconnected.flv", directory);
    snprintf(second, sizeof(second), "%s/reconnected.flv", directory);
    unsigned port = free_port();
    pid_t ingest = ingest_start(port, first);
    snprintf(config->stream.server_url, sizeof(config->stream.server_url),
             "rtmp://127.0.0.1:%u/ingest", port);
    config->stream.reconnect_attempts = 2;
    config->stream.reconnect_initial_ms = config->stream.reconnect_max_ms = 500;
    CastStream *stream = stream_open(audio);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    struct Tone tone = {.audio = audio};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, tone_main, &tone) == 0);
    Frame frame = {0};
    assert(frame_alloc(&frame, config->width, config->height) == 0);
    stream_privacy(stream, false);
    feed(stream, &frame, 0, false, 500);
    wait_state(stream, STREAM_STREAMING, 2000);
    kill(ingest, SIGKILL);
    waitpid(ingest, NULL, 0);
    stream_privacy(stream, true);
    feed(stream, &frame, 1, true, 100);
    ingest = ingest_start(port, second);
    feed(stream, &frame, 1, true, 1300);
    wait_state(stream, STREAM_STREAMING, 2000);
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.retry_attempt >= 1 && status.retry_attempt <= 2);
    stream_privacy(stream, false);
    feed(stream, &frame, 2, false, 800);
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    wait_state(stream, STREAM_STOPPED, 1000);
    stream_close(stream);
    atomic_store(&tone.stop, true);
    pthread_join(thread, NULL);
    waitpid(ingest, NULL, 0);
    struct Decoded decoded = inspect(second);
    assert(decoded.video_count > 25 && decoded.audio_count > 25);
    assert(decoded.colors[0] == 0 && decoded.colors[1] > 5 && decoded.colors[2] > 5);
    assert(decoded.active_samples > 10000 && decoded.energy / decoded.active_samples < 0.000001);
    assert(decoded.quiet_samples > 1000 && decoded.quiet_energy / decoded.quiet_samples > 0.001);
    fprintf(stderr, "reconnect fresh headers/keyframe: %d video, no retired red frames\n",
            decoded.video_count);
    frame_free(&frame);
    unlink(first);
    unlink(second);
}
static void backpressure_test(Config *config, CastAudio *audio, const char *directory)
{
    char output[PATH_MAX], error[CAST_ERR];
    snprintf(output, sizeof(output), "%s/stalled.flv", directory);
    unsigned port = free_port();
    pid_t ingest = ingest_start(port, output);
    snprintf(config->stream.server_url, sizeof(config->stream.server_url),
             "rtmp://127.0.0.1:%u/ingest", port);
    config->stream.video_bitrate_kbps = 50000;
    config->stream.reconnect_attempts = 0;
    config->stream.write_timeout_ms = 1200;
    CastStream *stream = stream_open(audio);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    Frame frame = {0};
    assert(frame_alloc(&frame, config->width, config->height) == 0);
    feed(stream, &frame, 0, false, 500);
    wait_state(stream, STREAM_STREAMING, 2000);
    assert(kill(ingest, SIGSTOP) == 0);
    uint64_t deadline = cast_now_ns() + 5000000000ULL;
    while (stream_test_write_age(stream) < 100000000ULL && cast_now_ns() < deadline) {
        feed(stream, &frame, 0, false, 50);
    }
    assert(stream_test_write_age(stream) >= 100000000ULL);
    uint64_t before = cast_now_ns();
    stream_privacy(stream, true);
    double privacy_ms = (cast_now_ns() - before) / 1e6;
    assert(privacy_ms < 100);
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.queue_depth == 0);
    feed(stream, &frame, 1, true, 1400);
    wait_state(stream, STREAM_FAILED, 2000);
    stream_status(stream, &status);
    assert(!status.active && status.dropped_frames > 0);
    assert(status.queue_depth <= (unsigned)config->stream.queue_frames);
    before = cast_now_ns();
    stream_stop(stream, error, sizeof(error));
    stream_close(stream);
    assert(cast_now_ns() - before < 200000000ULL);
    kill(ingest, SIGKILL);
    waitpid(ingest, NULL, 0);
    frame_free(&frame);
    unlink(output);
    config->stream.video_bitrate_kbps = 300;
    fprintf(stderr,
            "actual stopped-reader backpressure: bounded queue, privacy %.3fms, timeout retired "
            "worker\n",
            privacy_ms);
}
static void cancellation_test(Config *config, CastAudio *audio)
{
    char error[CAST_ERR];
    snprintf(config->stream.server_url, sizeof(config->stream.server_url),
             "rtmp://127.0.0.1:%u/ingest", free_port());
    config->stream.connect_timeout_ms = 150;
    config->stream.reconnect_initial_ms = 100;
    config->stream.reconnect_max_ms = 100;
    config->stream.reconnect_attempts = 1;
    CastStream *stream = stream_open(audio);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    wait_state(stream, STREAM_FAILED, 2000);
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.retry_attempt == 1 && !status.active);
    assert(!strstr(status.error, fixture_key));
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    wait_state(stream, STREAM_STOPPED, 1000);
    config->stream.reconnect_attempts = 1;
    config->stream.reconnect_initial_ms = config->stream.reconnect_max_ms = 2000;
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    wait_state(stream, STREAM_RECONNECTING, 1000);
    uint64_t before = cast_now_ns();
    stream_privacy(stream, true);
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    assert(cast_now_ns() - before < 100000000ULL);
    wait_state(stream, STREAM_STOPPED, 1000);
    stream_close(stream);
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--internal-stream-worker")) {
        return stream_worker_main(atoi(argv[2]));
    }
    unsigned char random[32];
    assert(getrandom(random, sizeof(random), 0) == sizeof(random));
    for (unsigned i = 0; i < sizeof(random); i++) {
        snprintf(fixture_key + i * 2, 3, "%02x", random[i]);
    }
    char directory[] = "/tmp/cast-stream-test-XXXXXX";
    assert(mkdtemp(directory));
    key_tests(directory);
    Config config;
    config_defaults(&config);
    config.width = 160;
    config.height = 90;
    config.fps = 30;
    config.virtual_enabled = false;
    config.camera_enabled = false;
    config.mic = config.desktop = config.virtual_audio = false;
    strcpy(config.output_device, "none");
    strcpy(config.camera_device, "synthetic");
    snprintf(config.stream.key_file, sizeof(config.stream.key_file), "%s/key", directory);
    config.stream.video_bitrate_kbps = 300;
    config.stream.reconnect_attempts = 0;
    config.stream.lag_ms = 500;
    char error[CAST_ERR];
    CastAudio *audio = audio_test_open(&config);
    assert(audio);
    if (argc == 2 && !strcmp(argv[1], "--sustained")) {
        sustained_test(&config, audio, directory);
    } else if (argc == 5 && !strcmp(argv[1], "--fixture")) {
        snprintf(config.stream.server_url, sizeof(config.stream.server_url), "%s", argv[2]);
        snprintf(config.stream.tls_ca_file, sizeof(config.stream.tls_ca_file), "%s", argv[3]);
        config.stream.connect_timeout_ms = 1500;
        config.stream.write_timeout_ms = 1000;
        if (!strcmp(argv[4], "reject-key")) {
            config.stream.reconnect_attempts = 3;
        }
        CastStream *stream = stream_open(audio);
        assert(stream_start(stream, &config, error, sizeof(error)) == 0);
        Frame frame = {0};
        assert(frame_alloc(&frame, config.width, config.height) == 0);
        if (!strcmp(argv[4], "accept")) {
            wait_state(stream, STREAM_STREAMING, 2000);
            feed(stream, &frame, 1, true, 700);
            StreamSnapshot status;
            stream_status(stream, &status);
            assert(status.bytes_written > 1000);
        } else if (!strcmp(argv[4], "reject") || !strcmp(argv[4], "reject-key")) {
            wait_state(stream, STREAM_FAILED, 3000);
            if (!strcmp(argv[4], "reject-key")) {
                StreamSnapshot status;
                stream_status(stream, &status);
                assert(status.retry_attempt == 0 && strstr(status.error, "Permission denied"));
            }
        } else if (!strcmp(argv[4], "blocked")) {
            uint64_t deadline = cast_now_ns() + 2000000000ULL;
            const char *marker = getenv("CAST_TEST_DNS_MARKER");
            assert(marker);
            while (access(marker, F_OK) && cast_now_ns() < deadline) {
                wait_ms(5);
            }
            assert(access(marker, F_OK) == 0);
        } else if (!strcmp(argv[4], "stall")) {
            feed(stream, &frame, 1, true, 300);
        } else {
            assert(false);
        }
        uint64_t before = cast_now_ns();
        stream_privacy(stream, true);
        assert(stream_stop(stream, error, sizeof(error)) == 0);
        double control_ms = (cast_now_ns() - before) / 1e6;
        assert(control_ms < 100);
        wait_state(stream, STREAM_STOPPED, 1000);
        stream_close(stream);
        frame_free(&frame);
        fprintf(stderr, "isolated fixture %s control %.3fms\n", argv[4], control_ms);
    } else {
        ingest_test(&config, audio, directory);
        audio_independence_test(audio);
        reconnect_test(&config, audio, directory);
        backpressure_test(&config, audio, directory);
        cancellation_test(&config, audio);
    }
    audio_close(audio);
    unlink(config.stream.key_file);
    assert(rmdir(directory) == 0);
    puts("streaming codec/cadence/privacy/key/cancellation tests passed");
    return 0;
}
