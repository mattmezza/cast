#ifndef CAST_RUNTIME_METRICS_H
#define CAST_RUNTIME_METRICS_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool ready;
    double tick_hz, average_tick_ms, maximum_tick_ms;
    uint64_t window_ns, loop_drops, recording_drops;
} CastRuntimeMetricsView;
typedef struct {
    uint64_t start_ns, last_ns, count, total_ns, maximum_ns;
    bool started;
    CastRuntimeMetricsView view;
} CastRuntimeMetrics;
/* Main-thread only; uses measured monotonic tick times, no IO or allocation.
 * Output remains unready until a complete >=1 second window is available. */
void runtime_metrics_tick(CastRuntimeMetrics *, uint64_t start_ns, uint64_t end_ns,
                           uint64_t loop_drops, uint64_t recording_drops);
#endif
