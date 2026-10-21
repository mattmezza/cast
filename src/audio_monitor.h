#ifndef CAST_AUDIO_MONITOR_H
#define CAST_AUDIO_MONITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CAST_MONITOR_RATE 48000u
#define CAST_MONITOR_CHANNELS 2u
#define CAST_MONITOR_CAPACITY 4800u /* 100 ms; never a growing playback queue. */
typedef struct CastAudioMonitor CastAudioMonitor;
typedef void (*CastMonitorRender)(void *, float *stereo, unsigned frames);
typedef struct {
    /* Only start opens an output device. close MUST quiesce render callbacks.
     * Lifecycle/status run on one host thread. Submit may run on a producer.
     * Device callback invokes render without allocation or blocking locks. */
    int (*open)(void *, CastMonitorRender, void *, char *, size_t);
    void (*close)(void *);
    int (*health)(void *, char *, size_t); /* Optional; 0 healthy, -1 failed. */
    void (*destroy)(void *);             /* Optional; called once on free. */
    uint64_t (*now_ns)(void *);          /* Optional test clock; MONOTONIC default. */
} CastMonitorBackend;
typedef struct {
    bool enabled, healthy;
    uint64_t epoch, played_frames, dropped_frames;
    unsigned queued_frames;
    char error[256];
} CastMonitorStatus;
/* Creation is inert. No Config, capture route, device opening or output consent
 * is inferred. PCM is already mixed and post-gain. Do not feed desktop loopback
 * into monitoring: OS feedback exclusion requires host routing/headphones.
 * Native adapters select the default output; macOS follows OS route changes.
 * There is no device picker or headphone detection in this service. */
CastAudioMonitor *audio_monitor_create(const CastMonitorBackend *, void *backend);
/* Native default-output adapter: PipeWire/Linux, AVAudioEngine/macOS13+.
 * Creates inert state only; no native API availability claim until start. */
CastAudioMonitor *audio_monitor_native_create(void);
int audio_monitor_start(CastAudioMonitor *, char *, size_t);
void audio_monitor_stop(CastAudioMonitor *);
void audio_monitor_free(CastAudioMonitor *);
/* Epoch barriers clear all queued PCM. Return current epoch for new producers.
 * Caller must use a barrier for seek, privacy, source replacement and cancel.
 * Already submitted hardware audio cannot be retracted; stop closes the route. */
uint64_t audio_monitor_barrier(CastAudioMonitor *);
/* 1 accepted, 0 disabled/stale/retired epoch, -1 invalid bounds/timestamps.
 * first_ns uses CLOCK_MONOTONIC, not media PTS. Max future lead 50 ms.
 * Entire batches <=100ms; overflow drops oldest, never blocks playback. */
int audio_monitor_submit(CastAudioMonitor *, uint64_t epoch, uint64_t first_ns,
                         const float *stereo, unsigned frames);
/* enabled is explicit consent; healthy is current output state. A failed
 * backend is never reopened automatically. Host may stop after a health error. */
void audio_monitor_status(CastAudioMonitor *, CastMonitorStatus *);
#endif
