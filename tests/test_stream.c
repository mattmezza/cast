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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
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
    assert(write(fd, "local-fixture-only\n", 19) == 19);
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
    assert(write(fd, "local-fixture-only\n", 19) == 19);
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
    while (!atomic_load(&tone->stop)) {
        float samples[480 * 2];
        uint64_t now = cast_now_ns();
        for (int i = 0; i < 480; i++) {
            float sample = sinf((float)i * 6.2831853f * 440.f / 48000.f) * 0.3f;
            samples[i * 2] = samples[i * 2 + 1] = sample;
        }
        audio_test_push(tone->audio, 0, now - 10000000ULL, samples, 480);
        wait_ms(10);
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
    double video_last, audio_last, max_gap, energy, quiet_energy;
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
    stream_privacy(stream, false);
    feed(stream, &frame, 2, false, 1100);
    StreamSnapshot status;
    stream_status(stream, &status);
    assert(status.retry_attempt == 0 && status.queue_depth <= 1 && status.bytes_written > 1000);
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
    assert(decoded.max_gap < 0.05 && fabs(decoded.video_last - decoded.audio_last) < 0.1);
    assert(decoded.active_samples > 10000 && decoded.energy / decoded.active_samples > 0.001);
    assert(decoded.quiet_samples > 10000 &&
           decoded.quiet_energy / decoded.quiet_samples < 0.000001);
    frame_free(&frame);
    unlink(output);
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
    assert(!strstr(status.error, "local-fixture-only"));
    assert(stream_stop(stream, error, sizeof(error)) == 0);
    wait_state(stream, STREAM_STOPPED, 1000);
    assert(stream_start(stream, config, error, sizeof(error)) == 0);
    stream_test_stage(stream, 1);
    stream_test_hold(stream, 10000);
    wait_ms(50);
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
    CastAudio *audio = audio_open(&config, error, sizeof(error));
    assert(audio);
    ingest_test(&config, audio, directory);
    cancellation_test(&config, audio);
    audio_close(audio);
    unlink(config.stream.key_file);
    assert(rmdir(directory) == 0);
    puts("streaming codec/cadence/privacy/key/cancellation tests passed");
    return 0;
}
