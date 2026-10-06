#ifndef CAST_PRO_RUNTIME_H
#define CAST_PRO_RUNTIME_H
#include "cast.h"
#include "edition.h"

/* Edition-neutral native integration. All pointers supplied to a tick/render
 * callback are borrowed for that call. Private modules own bounded workers and
 * caches; the capture thread never loads models, parses files or waits on jobs. */
typedef enum {
    CAST_CONSUMER_VIRTUAL,
    CAST_CONSUMER_RECORD,
    CAST_CONSUMER_STREAM,
    CAST_CONSUMER_NOTES,
    CAST_CONSUMER_COUNT
} CastRuntimeConsumer;
typedef enum {
    CAST_AUDIO_MIC,
    CAST_AUDIO_DESKTOP,
    CAST_AUDIO_MIX
} CastRuntimeAudioSource;
typedef enum {
    CAST_BARRIER_PRESENTATION,
    CAST_BARRIER_GLOBAL_PRIVACY,
    CAST_BARRIER_SOURCE,
    CAST_BARRIER_AUDIO,
    CAST_BARRIER_CONFIG,
    CAST_BARRIER_DISCONNECT,
    CAST_BARRIER_SHUTDOWN
} CastRuntimeBarrier;
typedef struct {
    double x, y, width, height;
} CastRuntimeRect;
typedef struct CastMotionEvent CastMotionEvent;
typedef struct {
    void *context;
    /* Authoritative cached entitlement, never an environment-supplied grant. */
    bool (*authorize)(void *, const char *feature_id, char *, size_t);
    /* Worker-only: submit an atomic session batch and wait for its acknowledged
     * main-thread commit. No config-file writes or output starts are implied. */
    int (*settings)(void *, unsigned pairs, const char *const *keys, const char *const *values,
                    char *, size_t);
    /* Worker-only bounded PCM read from already enabled capture nodes. Samples
     * are 48kHz stereo floats; no device acquisition, consent or source fallback. */
    int (*audio_read)(void *, CastRuntimeConsumer, CastRuntimeAudioSource, uint64_t timestamp_ns,
                      float *, unsigned frames, uint64_t expected_epoch, char *, size_t);
    /* Native-client-only identity-checked ordinary IPC. Document chunks are
     * requested explicitly; ordinary state responses never contain script text. */
    int (*client_command)(const Config *, int, const char *const *, char *, size_t);
    int (*launch_notes)(void *, const Config *, char *, size_t);
    int (*motion_command)(void *, int, char **, char *, size_t);
    int (*settings_motion)(void *, unsigned, const char *const *, const char *const *,
                           const CastMotionEvent *, char *, size_t);
    int (*config_reload)(void *, char *, size_t);
} CastRuntimeHost;
typedef struct {
    const Config *config;
    const State *state;
    const Capabilities *capabilities;
    uint64_t timestamp_ns, source_generation;
    uint64_t consumer_epoch[CAST_CONSUMER_COUNT];
    bool consumer_permitted[CAST_CONSUMER_COUNT];
    uint64_t recording_time_ns;
    const char *recording_path;
    bool recording_finalizing;
    bool notes_window_registered, notes_exclusion_effective;
    CastEditionSnapshot entitlement;
} CastRuntimeContext;
typedef struct {
    uint64_t timestamp_ns, source_generation;
    int source_width, source_height;
    Cursor cursor;
    Capabilities capabilities;
    CastRuntimeRect screen, camera;
    bool camera_visible;
    double legacy_factor, legacy_center_x, legacy_center_y;
    bool legacy_pose_valid;
} CastMotionInput;
typedef struct {
    double factor, viewport_x, viewport_y, viewport_width, viewport_height;
    double cursor_x, cursor_y;
    bool cursor_valid, cursor_visible;
    unsigned samples;
    /* Source-local subpixel geometry; samples use the same eligible source. */
    CastRuntimeRect shutter[8];
    double shutter_cursor_x[8], shutter_cursor_y[8];
    char filter[16], requested_filter[16];
    double requested_factor, velocity_log_factor, velocity_x, velocity_y;
    double focal_x, focal_y, click_x, click_y;
    bool follow, focus_locked, automatic, manual_override, camera_conflict;
    bool moving, click_visible, cursor_smoothing_effective, cursor_click_snap;
    unsigned requested_samples;
    char limitation[128];
} CastMotionSnapshot;
typedef enum {
    CAST_MOTION_TARGET,
    CAST_MOTION_FOCUS,
    CAST_MOTION_FOLLOW,
    CAST_MOTION_AUTO,
    CAST_MOTION_CLICK,
    CAST_MOTION_RESET
} CastMotionEventKind;
struct CastMotionEvent {
    CastMotionEventKind kind;
    uint64_t timestamp_ns, source_generation;
    double x, y, factor;
    bool enabled;
    int button;
};
typedef struct {
    bool notes_loaded, notes_running, notes_window_open;
    bool notes_exclusion_requested, notes_exclusion_effective;
    unsigned notes_line;
    char notes_state[24], notes_error[256];
    char notes_path[1024];
    bool speech_compiled, speech_ready, speech_enabled, speech_active[3];
    unsigned speech_processing_ms, speech_backlog_ms;
    unsigned speech_grace_remaining_seconds;
    uint64_t speech_dropped_work, speech_job_id;
    unsigned speech_job_progress;
    char speech_state[32], speech_error[256];
    char speech_model[1024], speech_language[16], speech_source[16];
    char speech_job_state[32], speech_job_srt[1024], speech_job_vtt[1024];
} CastWorkflowStatus;
typedef struct CastRuntimeHooks {
    /* One singleton service integrates shared speech/notes. Call initialize and
     * shutdown off timing threads; tick and barrier must remain bounded. */
    int (*initialize)(const CastRuntimeHost *, const Config *, char *, size_t);
    void (*tick)(const CastRuntimeContext *);
    void (*barrier)(CastRuntimeBarrier, unsigned consumer_mask,
                    const uint64_t epochs[CAST_CONSUMER_COUNT]);
    int (*prepare_config)(const Config *, char *, size_t);
    void (*commit_config)(const Config *);
    void (*draw_captions)(CastRuntimeConsumer, Frame *, uint64_t epoch);
    void (*recording_finished)(const char *, uint64_t duration_ns);
    void (*status)(char *, size_t, bool json);
    void (*snapshot)(CastWorkflowStatus *);
    /* Per-compositor motion storage. The public core keeps its legacy path. */
    void *(*motion_create)(const CastRuntimeHost *);
    void (*motion_destroy)(void *);
    int (*motion_advance)(void *, const Config *, const CastMotionInput *, CastMotionSnapshot *,
                          char *, size_t);
    int (*motion_event_validate)(void *, const CastMotionEvent *, char *, size_t);
    int (*motion_event)(void *, const CastMotionEvent *, char *, size_t);
    void (*motion_status)(void *, char *, size_t, bool json);
    void (*motion_barrier)(void *, CastRuntimeBarrier, uint64_t timestamp_ns);
    int (*motion_render)(void *, Frame *, const Frame *, const CastMotionSnapshot *,
                         CastRuntimeRect output, int corner_radius, char *, size_t);
    /* Separate process/window lifecycle; main-panel Close never calls this. */
    int (*notes_client)(const Config *, const CastRuntimeHost *, char *, size_t);
    int (*transcribe)(const Config *, int, char **, char *, size_t);
    /* Main-thread stop/close/cancel only. Return 1 when not handled. This path
     * retires consent/cache immediately and never acquires resources or starts
     * work; it remains responsive while a model prepares on another worker. */
    int (*safety_command)(Config *, int, char **, char *, size_t);
    /* Explicit read-only diagnostics; never initializes inference or audio. */
    void (*doctor)(const Config *, char *, size_t);
} CastRuntimeHooks;

const CastRuntimeHooks *cast_runtime_hooks(void);
void cast_runtime_set_host(const CastRuntimeHost *);
const CastRuntimeHost *cast_runtime_host(void);
#endif
