#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast.h"
#include "media_codec.h"
#include "media_internal.h"
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
uint64_t cast_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr,
                "usage: media_sustained NEW_OUTPUT.mkv [WIDTH HEIGHT] (60 seconds at 30fps)\n");
        return 2;
    }
    av_log_set_level(AV_LOG_ERROR);
    Config cfg;
    config_defaults(&cfg);
    cfg.width = 1920;
    cfg.height = 1080;
    if (argc == 4) {
        cfg.width = atoi(argv[2]);
        cfg.height = atoi(argv[3]);
        assert(cfg.width >= 64 && cfg.width <= 7680 && !(cfg.width % 2));
        assert(cfg.height >= 64 && cfg.height <= 4320 && !(cfg.height % 2));
    }
    cfg.fps = 30;
    cfg.record_queue = 8;
    cfg.record_bitrate_kbps = 6000;
    char error[CAST_ERR];
    assert(!media_profile_validate(true, error, sizeof(error)));
    CastAudio *audio = audio_test_open(&cfg);
    CastRecorder *recorder = recorder_open(audio);
    assert(audio && recorder);
    if (recorder_start(recorder, &cfg, argv[1], error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    printf("%s\n", error);
    fflush(stdout);
    Frame frame = {0};
    assert(!frame_alloc(&frame, cfg.width, cfg.height));
    uint64_t began = cast_now_ns();
    unsigned frames = 0;
    while (cast_now_ns() - began < 60000000000ULL) {
        /* Moving gradients exercise a changing software source without capture. */
        for (int y = 0; y < cfg.height; y++) {
            uint8_t *row = frame.data + (size_t)y * frame.stride;
            for (int x = 0; x < cfg.width; x++) {
                row[x * 4] = (uint8_t)((x + frames * 3) >> 3);
                row[x * 4 + 1] = (uint8_t)((y + frames * 2) >> 2);
                row[x * 4 + 2] = (uint8_t)((x + y + frames) >> 4);
                row[x * 4 + 3] = 255;
            }
        }
        frame.ts_ns = cast_now_ns();
        assert(recorder_frame(recorder, &frame, error, sizeof(error)) >= 0);
        frames++;
        uint64_t due = began + (uint64_t)frames * 1000000000ULL / cfg.fps;
        struct timespec deadline = {.tv_sec = (time_t)(due / 1000000000ULL),
                                    .tv_nsec = (long)(due % 1000000000ULL)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    assert(!recorder_stop(recorder, error, sizeof(error)));
    while (recorder_finalizing(recorder)) {
        struct timespec wait = {.tv_nsec = 10000000};
        nanosleep(&wait, NULL);
    }
    bool active, paused;
    uint64_t drops;
    recorder_status(recorder, &active, &paused, &drops, error, sizeof(error));
    assert(!active && !error[0]);
    uint64_t scale_time, encode_time, encoded_frames;
    recorder_test_timings(recorder, &scale_time, &encode_time, &encoded_frames);
    printf("worker average: RGBA->YUV %.3fms; encode+mux %.3fms (%llu frames)\n",
           scale_time / (double)encoded_frames / 1000000,
           encode_time / (double)encoded_frames / 1000000, (unsigned long long)encoded_frames);
    frame_free(&frame);
    recorder_close(recorder);
    audio_close(audio);
    AVFormatContext *format = NULL;
    assert(!avformat_open_input(&format, argv[1], NULL, NULL));
    assert(avformat_find_stream_info(format, NULL) >= 0);
    int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    assert(video >= 0 && audio_index >= 0);
    assert(format->streams[video]->codecpar->codec_id == AV_CODEC_ID_H264);
    assert(format->duration > 59500000 && format->duration < 61000000);
    AVPacket *packet = av_packet_alloc();
    unsigned encoded = 0;
    while (av_read_frame(format, packet) >= 0) {
        encoded += packet->stream_index == video;
        av_packet_unref(packet);
    }
    printf("%dx%d@30 software H.264/AAC: submitted=%u packets=%u drops=%llu duration=%.3fs; %s\n",
           cfg.width, cfg.height, frames, encoded, (unsigned long long)drops,
           format->duration / 1000000.0, argv[1]);
    fflush(stdout);
    av_packet_free(&packet);
    avformat_close_input(&format);
    if (encoded < 1790 || drops > 10) {
        fprintf(stderr,
                "sustained 1080p30 acceptance failed: require >=1790 packets and <=10 drops\n");
        return 1;
    }
    return 0;
}
