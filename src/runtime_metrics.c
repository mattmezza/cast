#include "runtime_metrics.h"
#include <string.h>
void runtime_metrics_tick(CastRuntimeMetrics *m, uint64_t start, uint64_t end,
                           uint64_t loop_drops, uint64_t recording_drops)
{
    if (!m || end < start) return;
    if (m->started && start < m->last_ns) memset(m, 0, sizeof *m);
    if (!m->started) { m->started = true; m->start_ns = start; }
    m->last_ns = start;
    uint64_t duration = end - start;
    ++m->count;
    m->total_ns += duration;
    if (duration > m->maximum_ns) m->maximum_ns = duration;
    m->view.loop_drops = loop_drops;
    m->view.recording_drops = recording_drops;
    uint64_t elapsed = end - m->start_ns;
    if (elapsed < UINT64_C(1000000000)) return;
    m->view.ready = true;
    m->view.window_ns = elapsed;
    m->view.tick_hz = (double)m->count * 1e9 / (double)elapsed;
    m->view.average_tick_ms = (double)m->total_ns / (double)m->count / 1e6;
    m->view.maximum_tick_ms = (double)m->maximum_ns / 1e6;
    m->count = m->total_ns = m->maximum_ns = 0;
    m->start_ns = end;
}
