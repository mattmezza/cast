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

static int camera_background_benchmark(Config *config, Compositor *compositor, Frame *screen,
                                       Frame *camera, Frame *output)
{
    enum {
        SAMPLES = 60
    };
    char error[CAST_ERR];
    strcpy(config->aspect, "4:3");
    for (int layout = 0; layout < 2; layout++) {
        strcpy(config->layout, layout ? "split" : "camera");
        for (int cached = 0; cached < 2; cached++) {
            double baseline = 0;
            for (int dynamic = 0; dynamic < 2; dynamic++) {
                strcpy(config->camera_background, dynamic ? "blurred" : "solid");
                compositor_clear(compositor);
                camera->ts_ns = cast_now_ns();
                /* Warm allocation and the same-snapshot processed-background cache. */
                if (compositor_render(compositor, config, screen, camera, NULL, false, output,
                                      error, sizeof error)) {
                    return fprintf(stderr, "background benchmark: %s\n", error), -1;
                }
                double samples[SAMPLES];
                for (int i = 0; i < SAMPLES; i++) {
                    if (!cached) {
                        camera->ts_ns++;
                    }
                    screen->ts_ns = cast_now_ns();
                    uint64_t started = screen->ts_ns;
                    if (compositor_render(compositor, config, screen, camera, NULL, false, output,
                                          error, sizeof error)) {
                        return fprintf(stderr, "background benchmark: %s\n", error), -1;
                    }
                    samples[i] = (cast_now_ns() - started) / 1e6;
                }
                qsort(samples, SAMPLES, sizeof *samples, compare_samples);
                double median = samples[SAMPLES / 2];
                if (!dynamic) {
                    baseline = median;
                }
                printf("synthetic RGBA %s 1920x1080 camera%dx%d background=%s snapshot=%s: "
                       "median=%.2fms p95=%.2fms added_vs_solid=%.2fms "
                       "render_frame_budget_at30fps=%.1f%%\n",
                       config->layout, camera->width, camera->height, config->camera_background,
                       cached ? "unchanged" : "fresh", median, samples[SAMPLES * 95 / 100],
                       dynamic ? median - baseline : 0, median / (1000.0 / 30) * 100);
            }
        }
    }
    return 0;
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
    if (frame_alloc(&camera, 1920, 1080)) {
        return fprintf(stderr, "background benchmark: camera allocation failed\n"), 1;
    }
    memset(camera.data, 0xc0, (size_t)camera.stride * camera.height);
    if (camera_background_benchmark(&config, compositor, &screen, &camera, &live)) {
        return 1;
    }
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&live);
    frame_free(&record);
    compositor_destroy(compositor);
    return 0;
}
