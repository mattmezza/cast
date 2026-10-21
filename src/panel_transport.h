#ifndef PANEL_TRANSPORT_H
#define PANEL_TRANSPORT_H
#include "cast.h"
#include "edition.h"
#include "stream.h"
#include "project_session.h"
#include "pro_runtime.h"
#include <sys/types.h>

/* Private protocol: both ends must come from the same build and struct layout. */
#define PANEL_PROTOCOL_VERSION 7u
#define PANEL_PREVIEW_COUNT 4
/* Private composition is independent of the three outgoing privacy-gated lanes. */
#define PANEL_PREVIEW_COMPOSITION 3
#define PANEL_PREVIEW_WIDTH 640
#define PANEL_PREVIEW_HEIGHT 360
/* Only the private composition lane needs full-HD editing detail. */
#define PANEL_COMPOSITION_PREVIEW_WIDTH 1920
#define PANEL_COMPOSITION_PREVIEW_HEIGHT 1080
typedef struct {
    Config config;
    State state;
    StreamSnapshot stream;
    CastEditionSnapshot edition;
    CastWorkflowStatus workflow;
    CastMotionSnapshot motion;
    CastProjectSnapshot project;
    AudioLevels audio_levels;
    bool cinematic_active;
    Capabilities capabilities;
    bool connected, countdown, finalizing, command_failed;
    /* Generation changes for a new daemon or a replacement attachment. */
    uint64_t daemon_generation, frame_sequence, privacy_epoch;
    uint64_t duration_ns, countdown_remaining_ns, command_queued, command_completed;
    char current_preset[64];
    char composition_error[256];
    char record_encoder[64], record_encoder_detail[256];
    char audio_status[4096], exclusion[256], error[CAST_ERR], last_reply[CAST_ERR];
    /* Discovery replies survive later command acknowledgements until consumed. */
    uint64_t device_reply_id;
    bool device_reply_failed;
    char device_reply[CAST_IPC_MAX];
} PanelSnapshot;
typedef struct PanelClient PanelClient;
/* Connection/reconnection and acknowledged commands run on a bounded worker. */
PanelClient *panel_client_open(const Config *, uint64_t x11_window, char *, size_t);
void panel_client_close(PanelClient *);
/* Snapshot reads briefly lock metadata; they never wait for worker socket I/O.
 * A snapshot may describe a disconnected daemon. */
bool panel_client_snapshot(PanelClient *, PanelSnapshot *);
/* 1 copied a new frame, 0 unchanged/busy, -1 disconnected. Caller owns Frame. */
int panel_client_frame(PanelClient *, int target, Frame *, char *, size_t);
int panel_client_command(PanelClient *, int, const char *const *, char *, size_t);
int panel_client_setting(PanelClient *, const char *dotted_key, const char *value, char *, size_t);

typedef struct PanelTransport PanelTransport;
struct App;
PanelTransport *panel_transport_create(void);
void panel_transport_destroy(PanelTransport *, struct App *);
/* Returns true when this private packet was handled, taking ownership of fd. */
bool panel_transport_request(PanelTransport *, struct App *, int fd, const void *, ssize_t, uid_t,
                             pid_t);
void panel_transport_publish(PanelTransport *, struct App *, const Frame *, const Frame *);
/* Refresh state and privacy gates before acknowledging a command. */
void panel_transport_barrier(PanelTransport *, struct App *, bool invalidate);
void panel_transport_check(PanelTransport *, struct App *);
bool panel_transport_attached(const PanelTransport *);
bool panel_transport_authorize(PanelTransport *, uint64_t generation, pid_t);
#endif
