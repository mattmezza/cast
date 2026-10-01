/* Composition microbenchmark: deliberately excludes capture, codecs and device I/O. */
#include "cast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static int compare_samples(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

int main(void)
{
    enum {
        SAMPLE_COUNT = 90
    };
    Config config;
    config_defaults(&config);
    config.zoom_factor = 1;
    Frame screen = {0}, camera = {0}, live = {0}, record = {0};
    if (frame_alloc(&screen, 1920, 1080) || frame_alloc(&camera, 640, 480)) {
        fprintf(stderr, "benchmark: allocation failed\n");
        return 1;
    }
    memset(screen.data, 0x80, (size_t)screen.stride * screen.height);
    memset(camera.data, 0xc0, (size_t)camera.stride * camera.height);
    Compositor *compositor = compositor_create();
    if (!compositor) {
        return 1;
    }
    Cursor cursor = {.x = 960, .y = 540, .valid = true};
    char error[CAST_ERR];
    for (int outputs = 1; outputs <= 2; outputs++) {
        double samples[SAMPLE_COUNT];
        uint64_t start = cast_now_ns();
        struct rusage before, after;
        getrusage(RUSAGE_SELF, &before);
        for (int i = 0; i < SAMPLE_COUNT; i++) {
            screen.ts_ns = cast_now_ns();
            uint64_t sample_start = screen.ts_ns;
            if (compositor_render(compositor, &config, &screen, &camera, &cursor, false, &live,
                                  error, sizeof error) ||
                (outputs == 2 && compositor_render(compositor, &config, &screen, &camera, &cursor,
                                                   true, &record, error, sizeof error))) {
                fprintf(stderr, "benchmark: %s\n", error);
                return 1;
            }
            samples[i] = (cast_now_ns() - sample_start) / 1e6;
        }
        double elapsed = (cast_now_ns() - start) / 1e9;
        getrusage(RUSAGE_SELF, &after);
        double user_cpu = after.ru_utime.tv_sec - before.ru_utime.tv_sec +
                          (after.ru_utime.tv_usec - before.ru_utime.tv_usec) / 1e6;
        double system_cpu = after.ru_stime.tv_sec - before.ru_stime.tv_sec +
                            (after.ru_stime.tv_usec - before.ru_stime.tv_usec) / 1e6;
        qsort(samples, SAMPLE_COUNT, sizeof samples[0], compare_samples);
        printf("synthetic RGBA overlay 1920x1080 camera640x480 outputs=%d: "
               "throughput=%.2f compositions/s median=%.2fms p95=%.2fms "
               "cpu=%.2fs peak_rss=%ldKiB; estimated render budget at30fps=%.1f%% of one CPU\n",
               outputs, SAMPLE_COUNT / elapsed, samples[SAMPLE_COUNT / 2],
               samples[SAMPLE_COUNT * 95 / 100], user_cpu + system_cpu, after.ru_maxrss,
               (user_cpu + system_cpu) / SAMPLE_COUNT * 30 * 100);
    }
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&live);
    frame_free(&record);
    compositor_destroy(compositor);
    return 0;
}
