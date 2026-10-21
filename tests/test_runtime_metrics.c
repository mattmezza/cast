#include "runtime_metrics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    CastRuntimeMetrics m = {0};
    for (unsigned i=0; i<100; ++i)
        runtime_metrics_tick(&m, UINT64_C(1000000000)+i*UINT64_C(10000000),
                             UINT64_C(1001000000)+i*UINT64_C(10000000), 3, 7);
    assert(!m.view.ready);
    runtime_metrics_tick(&m, UINT64_C(2000000000), UINT64_C(2002000000), 4, 8);
    assert(m.view.ready && m.view.loop_drops==4 && m.view.recording_drops==8);
    assert(fabs(m.view.tick_hz - 101e9 / 1002000000.0)<1e-9);
    assert(fabs(m.view.average_tick_ms - 102.0/101)<1e-9);
    assert(m.view.maximum_tick_ms==2);
    CastRuntimeMetrics before=m;
    runtime_metrics_tick(&m, 100, 99, 100, 100);
    assert(m.view.tick_hz==before.view.tick_hz && m.count==before.count);
    runtime_metrics_tick(&m, 1, 2, 0, 0);
    assert(!m.view.ready && m.count==1 && !m.view.loop_drops);
    runtime_metrics_tick(NULL, 0, 1, 0, 0);
    puts("runtime metrics: measured window cadence/cost, independent drop counters and clock reset passed");
}
