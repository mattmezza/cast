/* CPU detail-preservation acceptance, independent of sustained throughput.
 * Thresholds are fixed before measurement: each decoded Y/U/V plane >=35 dB,
 * stripe contrast >=90%, text luma MAE <=8. No resizing or bitrate fallback.
 * The reference includes the unavoidable original RGB -> 4:2:0 conversion;
 * this is not a claim about subjective desktop or untested GPU quality. */
#include "cast.h"
#include "media_codec.h"
#include "media_internal.h"
#include <assert.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 1920
#define HEIGHT 1080
#define SUBMISSIONS 90

uint64_t cast_now_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}
static void source_frame(Frame *frame)
{
    /* C, A, S and T: five-by-seven glyphs, with two-pixel stems. */
    static const unsigned char glyphs[4][7] = {{15, 16, 16, 16, 16, 16, 15},
                                               {14, 17, 17, 31, 17, 17, 17},
                                               {15, 16, 16, 14, 1, 1, 30},
                                               {31, 4, 4, 4, 4, 4, 4}};
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            unsigned char *pixel = frame->data + (size_t)y * frame->stride + x * 4;
            unsigned char value = (unsigned char)(30 + x * 110 / WIDTH + y * 60 / HEIGHT);
            pixel[0] = pixel[1] = pixel[2] = value;
            if (x >= 32 && x < WIDTH - 32 && y < 256) {
                unsigned stripe = 1u << (y / 64);
                value = (x / stripe) & 1 ? 220 : 40;
                pixel[0] = pixel[1] = pixel[2] = value;
            } else if (x >= 32 && x < WIDTH - 32 && y >= 320 && y < 768) {
                int gx = (x - 32) % 64 / 2;
                int gy = (y - 320) % 24 / 2;
                int letter = gx / 8;
                int column = gx % 8;
                bool ink = gy < 7 && column < 5 && (glyphs[letter][gy] & (1u << (4 - column)));
                pixel[0] = pixel[1] = pixel[2] = ink ? 230 : 30;
            } else if (y >= 800) {
                /* Chroma ramps contain no one-pixel colored text, avoiding an
                 * unrealistic demand to preserve chroma discarded by 4:2:0. */
                pixel[0] = (unsigned char)(x * 255 / WIDTH);
                pixel[1] = (unsigned char)((y - 800) * 255 / (HEIGHT - 800));
                pixel[2] = (unsigned char)(255 - x * 255 / WIDTH);
                if (!(x % 128) || !(y % 64)) {
                    pixel[0] = pixel[1] = pixel[2] = 240;
                }
            }
            pixel[3] = 255;
        }
    }
}
static AVFrame *reference_frame(const Frame *source)
{
    AVFrame *reference = av_frame_alloc();
    assert(reference);
    reference->width = WIDTH;
    reference->height = HEIGHT;
    reference->format = AV_PIX_FMT_YUV420P;
    assert(!av_frame_get_buffer(reference, 32));
    struct SwsContext *converter =
        sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_RGBA, WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
                       SWS_FAST_BILINEAR, NULL, NULL, NULL);
    assert(converter);
    const unsigned char *planes[4] = {source->data};
    int strides[4] = {source->stride};
    assert(sws_scale(converter, planes, strides, 0, HEIGHT, reference->data, reference->linesize) ==
           HEIGHT);
    sws_freeContext(converter);
    return reference;
}
typedef struct {
    unsigned frames;
    double minimum_psnr[3], minimum_contrast, maximum_text_error;
    uint64_t video_bytes;
    bool accepted;
} Quality;
static void inspect_frame(const AVFrame *frame, const AVFrame *reference, Quality *quality)
{
    assert(frame->width == WIDTH && frame->height == HEIGHT);
    assert(frame->format == AV_PIX_FMT_YUV420P);
    assert(!(frame->flags & AV_FRAME_FLAG_CORRUPT) && !frame->decode_error_flags);
    quality->frames++;
    for (unsigned plane = 0; plane < 3; ++plane) {
        unsigned width = plane ? WIDTH / 2 : WIDTH;
        unsigned height = plane ? HEIGHT / 2 : HEIGHT;
        double squared_error = 0;
        for (unsigned y = 0; y < height; ++y) {
            const unsigned char *actual = frame->data[plane] + y * frame->linesize[plane];
            const unsigned char *expected = reference->data[plane] + y * reference->linesize[plane];
            for (unsigned x = 0; x < width; ++x) {
                int difference = (int)actual[x] - expected[x];
                squared_error += difference * difference;
            }
        }
        double mse = squared_error / (width * height);
        double psnr = mse ? 10 * log10(255.0 * 255.0 / mse) : 100;
        if (psnr < quality->minimum_psnr[plane]) {
            quality->minimum_psnr[plane] = psnr;
        }
        quality->accepted &= psnr >= 35;
    }
    for (unsigned band = 0; band < 4; ++band) {
        unsigned stripe = 1u << band;
        double actual_sums[2] = {0}, expected_sums[2] = {0};
        unsigned counts[2] = {0};
        for (unsigned y = band * 64 + 8; y < band * 64 + 56; ++y) {
            for (unsigned x = 48; x < WIDTH - 48; ++x) {
                unsigned side = (x / stripe) & 1;
                actual_sums[side] += frame->data[0][y * frame->linesize[0] + x];
                expected_sums[side] += reference->data[0][y * reference->linesize[0] + x];
                counts[side]++;
            }
        }
        double actual = actual_sums[1] / counts[1] - actual_sums[0] / counts[0];
        double expected = expected_sums[1] / counts[1] - expected_sums[0] / counts[0];
        double ratio = actual / expected;
        if (ratio < quality->minimum_contrast) {
            quality->minimum_contrast = ratio;
        }
        quality->accepted &= ratio >= 0.90;
    }
    double text_error = 0;
    for (unsigned y = 320; y < 768; ++y) {
        for (unsigned x = 32; x < WIDTH - 32; ++x) {
            int difference = frame->data[0][y * frame->linesize[0] + x] -
                             reference->data[0][y * reference->linesize[0] + x];
            text_error += abs(difference);
        }
    }
    text_error /= (768 - 320) * (WIDTH - 64);
    if (text_error > quality->maximum_text_error) {
        quality->maximum_text_error = text_error;
    }
    quality->accepted &= text_error <= 8;
}
static AVFrame *resize_reference(const AVFrame *source, int width, int height)
{
    AVFrame *resized = av_frame_alloc();
    assert(resized);
    resized->width = width;
    resized->height = height;
    resized->format = AV_PIX_FMT_YUV420P;
    assert(!av_frame_get_buffer(resized, 32));
    struct SwsContext *converter =
        sws_getContext(source->width, source->height, AV_PIX_FMT_YUV420P, width, height,
                       AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    assert(converter);
    assert(sws_scale(converter, (const uint8_t *const *)source->data, source->linesize, 0,
                     source->height, resized->data, resized->linesize) == height);
    sws_freeContext(converter);
    return resized;
}
static void check_downscale_control(const AVFrame *reference)
{
    /* Merely checking the final dimensions would accept a disguised 720p source.
     * The fixed fine-detail thresholds must reject downscale followed by upscale. */
    AVFrame *smaller = resize_reference(reference, 1280, 720);
    AVFrame *upscaled = resize_reference(smaller, WIDTH, HEIGHT);
    Quality control = {.minimum_psnr = {100, 100, 100}, .minimum_contrast = 100, .accepted = true};
    inspect_frame(upscaled, reference, &control);
    assert(!control.accepted);
    assert(control.minimum_contrast < 0.90);
    printf("negative 720p->1080p control rejected: Y PSNR %.3f dB; stripe contrast %.4f\n",
           control.minimum_psnr[0], control.minimum_contrast);
    av_frame_free(&upscaled);
    av_frame_free(&smaller);
}
static Quality inspect(const char *path, const AVFrame *reference)
{
    Quality quality = {.minimum_psnr = {100, 100, 100}, .minimum_contrast = 100, .accepted = true};
    AVFormatContext *format = NULL;
    assert(!avformat_open_input(&format, path, NULL, NULL));
    assert(avformat_find_stream_info(format, NULL) >= 0);
    int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    assert(video >= 0);
    AVStream *stream = format->streams[video];
    assert(stream->codecpar->codec_id == AV_CODEC_ID_H264);
    assert(stream->codecpar->width == WIDTH && stream->codecpar->height == HEIGHT);
    assert(format->duration > 0);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    assert(decoder);
    assert(!avcodec_parameters_to_context(decoder, stream->codecpar));
    assert(!avcodec_open2(decoder, decoder->codec, NULL));
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    assert(packet && frame);
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video) {
            quality.video_bytes += packet->size;
            assert(!avcodec_send_packet(decoder, packet));
            int result;
            while (!(result = avcodec_receive_frame(decoder, frame))) {
                inspect_frame(frame, reference, &quality);
                av_frame_unref(frame);
            }
            assert(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
        }
        av_packet_unref(packet);
    }
    assert(!avcodec_send_packet(decoder, NULL));
    int result;
    while (!(result = avcodec_receive_frame(decoder, frame))) {
        inspect_frame(frame, reference, &quality);
        av_frame_unref(frame);
    }
    assert(result == AVERROR_EOF);
    quality.accepted &= quality.frames >= 30;
    printf(
        "decoded quality: %u full1920x1080 frames; minimum Y/U/V PSNR %.3f/%.3f/%.3f dB; "
        "minimum stripe contrast %.4f; maximum text MAE %.4f; video packet bitrate %.3f Mbps; %s\n",
        quality.frames, quality.minimum_psnr[0], quality.minimum_psnr[1], quality.minimum_psnr[2],
        quality.minimum_contrast, quality.maximum_text_error,
        quality.video_bytes * 8.0 / (format->duration / 1000000.0) / 1000000.0,
        quality.accepted ? "PASS" : "FAIL");
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return quality;
}
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "usage: test_media_quality NEW_OUTPUT.mkv (3s CPU H.264, full1080p, 6Mbps)\n");
        return 2;
    }
    assert(access(argv[1], F_OK));
    av_log_set_level(AV_LOG_ERROR);
    Config config;
    config_defaults(&config);
    config.width = WIDTH;
    config.height = HEIGHT;
    config.fps = 30;
    config.record_bitrate_kbps = 6000;
    strcpy(config.record_rate_control, "bitrate");
    strcpy(config.video_codec, "libopenh264");
    char error[CAST_ERR], encoder[64], detail[256];
    AVCodecContext *probe = NULL;
    MediaCodecSelection selection;
    assert(!media_video_open(&config, false, true, &probe, &selection, error, sizeof error));
    assert(probe->width == WIDTH && probe->height == HEIGHT);
    assert(probe->bit_rate == 6000000 && probe->rc_max_rate == 6000000);
    avcodec_free_context(&probe);
    CastAudio *audio = audio_test_open(&config);
    CastRecorder *recorder = recorder_open(audio);
    assert(audio && recorder);
    assert(!recorder_start(recorder, &config, argv[1], error, sizeof error));
    recorder_encoder(recorder, encoder, sizeof encoder, detail, sizeof detail);
    assert(!strcmp(encoder, "libopenh264"));
    printf("CPU detail source: %dx%d@%d, requested bitrate %dkbps; encoder %s; %s\n", WIDTH, HEIGHT,
           config.fps, config.record_bitrate_kbps, encoder, detail);
    Frame source = {0};
    assert(!frame_alloc(&source, WIDTH, HEIGHT));
    source_frame(&source);
    AVFrame *reference = reference_frame(&source);
    check_downscale_control(reference);
    uint64_t started = cast_now_ns();
    for (unsigned i = 0; i < SUBMISSIONS; ++i) {
        source.ts_ns = cast_now_ns();
        assert(recorder_frame(recorder, &source, error, sizeof error) >= 0);
        uint64_t due = started + (uint64_t)(i + 1) * 1000000000ULL / config.fps;
        struct timespec deadline = {.tv_sec = due / 1000000000ULL, .tv_nsec = due % 1000000000ULL};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    assert(!recorder_stop(recorder, error, sizeof error));
    uint64_t finalization_deadline = cast_now_ns() + 20000000000ULL;
    while (recorder_finalizing(recorder)) {
        assert(cast_now_ns() < finalization_deadline);
        struct timespec interval = {.tv_nsec = 10000000};
        nanosleep(&interval, NULL);
    }
    bool active, paused;
    uint64_t dropped;
    recorder_status(recorder, &active, &paused, &dropped, error, sizeof error);
    assert(!active && !error[0]);
    printf("quality fixture submitted=%u, queue drops=%llu (throughput measured separately)\n",
           SUBMISSIONS, (unsigned long long)dropped);
    recorder_close(recorder);
    audio_close(audio);
    frame_free(&source);
    Quality quality = inspect(argv[1], reference);
    av_frame_free(&reference);
    return quality.accepted ? 0 : 1;
}
