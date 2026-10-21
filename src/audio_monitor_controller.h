#ifndef CAST_AUDIO_MONITOR_CONTROLLER_H
#define CAST_AUDIO_MONITOR_CONTROLLER_H
#include "audio_monitor_session.h"
typedef enum {
    CAST_MONITOR_OFF,CAST_MONITOR_STARTING,CAST_MONITOR_RUNNING,
    CAST_MONITOR_STOPPING,CAST_MONITOR_FAILED
} CastMonitorControllerState;
typedef struct {
    CastMonitorControllerState state;
    CastMonitorSessionStatus session;
    char error[256];
} CastMonitorControllerStatus;
typedef struct CastMonitorController CastMonitorController;
/* Creates an idle lifecycle worker, never opens devices. Session ownership
 * transfers only on success. Pass an unstarted session. Capture/Media must
 * outlive controller free; never call session lifecycle directly afterwards. */
CastMonitorController *audio_monitor_controller_create_with_session(CastMonitorSession *);
CastMonitorController *audio_monitor_controller_create(struct Media *);
/* Main/host thread: bounded state changes only, no device IO, capture reads,
 * worker joins or startup handshakes. Repeated requests coalesce, no retries.
 * STARTING/RUNNING/STOPPING are published independently from actual backend ack. */
void audio_monitor_controller_request_start(CastMonitorController *);
void audio_monitor_controller_request_stop(CastMonitorController *);
/* Updates retire queued PCM immediately even during slow startup/shutdown. */
void audio_monitor_controller_update(CastMonitorController *,CastMonitorSessionState);
/* Cached snapshot only: no native device queries on the host thread. */
void audio_monitor_controller_status(CastMonitorController *,CastMonitorControllerStatus *);
/* Shutdown may join. Completes lifecycle, producer and device callbacks before
 * returning; MUST run before media_close. */
void audio_monitor_controller_free(CastMonitorController *);
#endif
