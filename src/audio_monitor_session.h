#ifndef CAST_AUDIO_MONITOR_SESSION_H
#define CAST_AUDIO_MONITOR_SESSION_H
#include "audio_monitor.h"
typedef struct {
    uint64_t epoch,latest_ns;
    bool enabled,available;
} CastMonitorInput;
typedef struct {
    void (*snapshot)(void *, CastMonitorInput *);
    /* Bounded, no device opening. 0 ready, 1 pending, -1 unavailable. */
    int (*read)(void *,uint64_t first_ns,float *stereo,unsigned frames,char *,size_t);
} CastMonitorCapture;
typedef struct {
    bool mic_enabled,privacy;
    uint64_t route_epoch;
} CastMonitorSessionState;
typedef struct {
    bool running,capture_ready;
    CastMonitorStatus output;
    char error[256];
} CastMonitorSessionStatus;
typedef struct CastMonitorSession CastMonitorSession;
/* Inert, privacy=true initially. Takes monitor ownership only on success.
 * Lifecycle/update/status on one host thread; worker owns all capture reads. */
CastMonitorSession *audio_monitor_session_create(CastAudioMonitor *,const CastMonitorCapture *,void *capture);
struct Media;
/* Native output + Media source0 (microphone) only. Never enables capture,
 * requests permission, selects desktop, or changes Config. Media is borrowed. */
CastMonitorSession *audio_monitor_session_media_create(struct Media *);
int audio_monitor_session_start(CastMonitorSession *,char *,size_t);
void audio_monitor_session_update(CastMonitorSession *,CastMonitorSessionState);
/* Stop joins capture producer FIRST, then quiesces output. Required before
 * media_close; free also stops and releases the owned output monitor. */
void audio_monitor_session_stop(CastMonitorSession *);
void audio_monitor_session_free(CastMonitorSession *);
void audio_monitor_session_status(CastMonitorSession *,CastMonitorSessionStatus *);
#endif
