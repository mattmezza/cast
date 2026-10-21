/* Isolated, device-free 4K RGBA->YUV420P conversion benchmark. Build against
 * the SDK being measured; three warmups, 30 measured frames per slice count.
 * Reports wall/CPU cost and verifies byte-identical output across thread counts.
 * This does not measure capture, composition, encoder throughput or total FPS. */
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t cpu(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return ((uint64_t)r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000;
}
int main(void)
{
    AVFrame *baseline = NULL;
    for (int threads = 1; threads <= 8; threads *= 2) {
        AVFrame *s = av_frame_alloc(), *d = av_frame_alloc();
        if (!s || !d) {
            return 1;
        }
        s->format = AV_PIX_FMT_RGBA;
        d->format = AV_PIX_FMT_YUV420P;
        s->width = d->width = 3840;
        s->height = d->height = 2160;
        if (av_frame_get_buffer(s, 32) || av_frame_get_buffer(d, 32)) {
            return 1;
        }
        for (int y = 0; y < 2160; y++) {
            for (int x = 0; x < 3840; x++) {
                unsigned char *p = s->data[0] + y * s->linesize[0] + x * 4;
                p[0] = x * 7 + y;
                p[1] = x + y * 3;
                p[2] = x * 5 + y * 11;
                p[3] = 255;
            }
        }
        struct SwsContext *c = sws_alloc_context();
        if (!c) {
            return 2;
        }
        av_opt_set_int(c, "srcw", 3840, 0);
        av_opt_set_int(c, "srch", 2160, 0);
        av_opt_set_int(c, "src_format", AV_PIX_FMT_RGBA, 0);
        av_opt_set_int(c, "dstw", 3840, 0);
        av_opt_set_int(c, "dsth", 2160, 0);
        av_opt_set_int(c, "dst_format", AV_PIX_FMT_YUV420P, 0);
        av_opt_set_int(c, "sws_flags", SWS_FAST_BILINEAR, 0);
        av_opt_set_int(c, "threads", threads, 0);
        if (sws_init_context(c, NULL, NULL) < 0) {
            return 2;
        }
        for (int i = 0; i < 3; i++) {
            if (sws_scale_frame(c, d, s) < 0) {
                return 3;
            }
        }
        uint64_t cpu_start = cpu(), start = now();
        for (int i = 0; i < 30; i++) {
            if (sws_scale_frame(c, d, s) < 0) {
                return 4;
            }
        }
        double ms = (now() - start) / 30000000., cpu_ms = (cpu() - cpu_start) / 30000000.;
        uint64_t hash = 1469598103934665603ULL;
        for (int p = 0; p < 3; p++) {
            for (int y = 0; y < (p ? 1080 : 2160); y++) {
                for (int x = 0; x < (p ? 1920 : 3840); x++) {
                    hash = (hash ^ d->data[p][y * d->linesize[p] + x]) * 1099511628211ULL;
                }
            }
        }
        if (!baseline) {
            baseline = av_frame_clone(d);
            if (!baseline) {
                return 1;
            }
        } else {
            for (int p = 0; p < 3; p++) {
                for (int y = 0; y < (p ? 1080 : 2160); y++) {
                    if (memcmp(baseline->data[p] + y * baseline->linesize[p],
                               d->data[p] + y * d->linesize[p], p ? 1920 : 3840)) {
                        fprintf(stderr, "threaded output differs\n");
                        return 5;
                    }
                }
            }
        }
        printf("threads=%d mean_ms=%.3f cpu_ms=%.3f hash=%016llx\n", threads, ms, cpu_ms,
               (unsigned long long)hash);
        sws_freeContext(c);
        av_frame_free(&s);
        av_frame_free(&d);
    }
    av_frame_free(&baseline);
    return 0;
}
