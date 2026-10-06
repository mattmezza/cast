#include "app_internal.h"
#include "compositor.h"
#include "edition_extensions.h"
#include "ipc_identity.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#define RUNTIME_QUEUE 4
#define ALL_CONSUMERS ((1u << CAST_CONSUMER_COUNT) - 1)
typedef struct {
    bool occupied, queued, processing, complete, motion, status;
    uint64_t revision;
    unsigned pairs, required_features;
    char keys[(CAST_MAX_ARGS - 1) / 2][128];
    char values[(CAST_MAX_ARGS - 1) / 2][1024];
    CastMotionEvent event;
    int result;
    bool json, configuration;
    Config config;
    void *prepared_motion;
    char output[CAST_IPC_MAX - 16];
} RuntimeRequest;
struct AppRuntime {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool stopped;
    App *app;
    Config config;
    uint64_t revision, timestamp, epoch[CAST_CONSUMER_COUNT], after[CAST_CONSUMER_COUNT];
    bool permitted[CAST_CONSUMER_COUNT];
    bool motion_ready, motion_pending;
    uint64_t motion_retry_after;
    CastEditionSnapshot entitlement;
    RuntimeRequest queue[RUNTIME_QUEUE];
    pid_t notes_child;
};
static bool authorize(void *context, const char *feature, char *error, size_t size)
{
    AppRuntime *r = context;
    pthread_mutex_lock(&r->lock);
    CastEditionSnapshot snapshot = r->entitlement;
    uint64_t timestamp = r->timestamp;
    bool stopped = r->stopped;
    pthread_mutex_unlock(&r->lock);
    uint64_t now = cast_now_ns();
    if (stopped || !timestamp || now < timestamp || now - timestamp > UINT64_C(2000000000)) {
        snprintf(error, size,
                 "authoritative entitlement snapshot unavailable; retry after reconnect");
        return false;
    }
    return cast_edition_require(feature, &snapshot, error, size) == 0;
}
static unsigned setting_feature_mask(const char *section, const char *key)
{
    if (!cast_extension_setting_find(section, key)) {
        return 0;
    }
    if (!strcmp(section, "zoom") || !strcmp(section, "cursor")) {
        return 1;
    }
    if (!strcmp(section, "transcription") || !strcmp(section, "subtitles")) {
        return 2;
    }
    return !strcmp(section, "notes") ? 4 : 0;
}
static unsigned active_configuration_mask(const Config *config)
{
    unsigned mask = !strcmp(config_extension_value(config, "zoom", "motion"), "cinematic") ? 1 : 0;
    if (!strcmp(config_extension_value(config, "transcription", "enabled"), "true") ||
        !strcmp(config_extension_value(config, "subtitles", "virtual"), "true") ||
        !strcmp(config_extension_value(config, "subtitles", "record"), "true") ||
        !strcmp(config_extension_value(config, "subtitles", "stream"), "true")) {
        mask |= 2;
    }
    const CastExtensionCapability *notes = cast_extension_capability("speech_teleprompter");
    if (notes && notes->active && notes->active()) {
        mask |= 4;
    }
    return mask;
}
static int submit(AppRuntime *r, unsigned pairs, const char *const *keys, const char *const *values,
                  const CastMotionEvent *motion, bool status, bool json, const Config *override,
                  char *error, size_t size)
{
    if (pairs > (CAST_MAX_ARGS - 1) / 2) {
        snprintf(error, size, "session batch exceeds its bound");
        return -1;
    }
    Config *candidate = malloc(sizeof *candidate);
    if (!candidate) {
        return app_error(error, size, "cannot allocate session candidate");
    }
    unsigned touched_features = motion ? 1 : 0;
    pthread_mutex_lock(&r->lock);
    *candidate = override ? *override : r->config;
    if (override) {
        size_t count;
        const CastExtensionSetting *schema = cast_shared_extension_schema(&count);
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(config_extension_value(candidate, schema[i].section, schema[i].key),
                       config_extension_value(&r->config, schema[i].section, schema[i].key))) {
                touched_features |= setting_feature_mask(schema[i].section, schema[i].key);
            }
        }
    }
    uint64_t revision = r->revision;
    bool motion_ready = r->motion_ready;
    pthread_mutex_unlock(&r->lock);
    for (unsigned i = 0; i < pairs; ++i) {
        if (!keys[i] || !values[i] || strlen(keys[i]) >= 128 || strlen(values[i]) >= 1024 ||
            config_set_value(candidate, keys[i], values[i], error, size)) {
            free(candidate);
            return -1;
        }
    }
    for (unsigned i = 0; i < pairs; ++i) {
        char section[128];
        snprintf(section, sizeof section, "%s", keys[i]);
        char *dot = strchr(section, '.');
        if (dot) {
            *dot++ = 0;
            touched_features |= setting_feature_mask(section, dot);
        }
    }
    unsigned required_features = touched_features & active_configuration_mask(candidate);
    if (motion) {
        required_features |= 1;
    }
    if (motion && (motion->kind == CAST_MOTION_TARGET || motion->kind == CAST_MOTION_FOCUS) &&
        motion->factor > candidate->zoom_max) {
        free(candidate);
        return app_error(error, size, "cinematic target exceeds the candidate zoom maximum");
    }
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!status &&
        (config_validate(candidate, error, size) ||
         (hooks && hooks->prepare_config && hooks->prepare_config(candidate, error, size)))) {
        free(candidate);
        return -1;
    }
    void *prepared_motion = NULL;
    if (!status && !motion_ready && hooks && hooks->motion_create &&
        !strcmp(config_extension_value(candidate, "zoom", "motion"), "cinematic")) {
        if (!authorize(r, "cinematic_zoom", error, size)) {
            free(candidate);
            return -1;
        }
        prepared_motion = hooks->motion_create(cast_runtime_host());
        if (!prepared_motion) {
            free(candidate);
            return app_error(error, size, "cannot prepare authorized cinematic renderer");
        }
    }
    pthread_mutex_lock(&r->lock);
    RuntimeRequest *request = NULL;
    for (unsigned i = 0; i < RUNTIME_QUEUE; ++i) {
        if (!r->queue[i].occupied) {
            request = &r->queue[i];
            break;
        }
    }
    if (!request || r->stopped || revision != r->revision) {
        pthread_mutex_unlock(&r->lock);
        free(candidate);
        if (prepared_motion && hooks->motion_destroy) {
            hooks->motion_destroy(prepared_motion);
        }
        return app_error(error, size,
                         "session changed or command queue busy; retry after acknowledgement");
    }
    *request = (RuntimeRequest){.occupied = true,
                                .queued = true,
                                .revision = revision,
                                .pairs = pairs,
                                .required_features = required_features,
                                .status = status,
                                .json = json,
                                .prepared_motion = prepared_motion};
    if (override) {
        request->configuration = true;
        request->config = *candidate;
    }
    free(candidate);
    for (unsigned i = 0; i < pairs; ++i) {
        snprintf(request->keys[i], sizeof request->keys[i], "%s", keys[i]);
        snprintf(request->values[i], sizeof request->values[i], "%s", values[i]);
    }
    if (motion) {
        request->motion = true;
        request->event = *motion;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    while (!request->complete && !r->stopped) {
        int rc = pthread_cond_timedwait(&r->wake, &r->lock, &until);
        if (rc == ETIMEDOUT && !request->processing) {
            request->queued = false;
            request->result = -1;
            snprintf(request->output, sizeof request->output,
                     "session command cancelled before commit; retry");
            request->complete = true;
        } else if (rc == ETIMEDOUT) {
            /* Once the main thread starts a transaction, completion is definitive.
             * Do not report failure and allow a delayed mutation after that reply. */
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec++;
        }
    }
    int result = r->stopped && !request->complete ? -1 : request->result;
    snprintf(error, size, "%s", request->complete ? request->output : "daemon shutting down");
    prepared_motion = request->prepared_motion;
    memset(request, 0, sizeof *request);
    pthread_mutex_unlock(&r->lock);
    if (prepared_motion && hooks && hooks->motion_destroy) {
        hooks->motion_destroy(prepared_motion);
    }
    return result;
}
static int settings(void *context, unsigned pairs, const char *const *keys,
                    const char *const *values, char *error, size_t size)
{
    return submit(context, pairs, keys, values, NULL, false, false, NULL, error, size);
}
static int settings_motion(void *context, unsigned pairs, const char *const *keys,
                           const char *const *values, const CastMotionEvent *event, char *error,
                           size_t size)
{
    return submit(context, pairs, keys, values, event, false, false, NULL, error, size);
}
static int motion_command(void *context, int argc, char **argv, char *error, size_t size)
{
    bool json = argc > 2 && !strcmp(argv[2], "--json");
    if (argc < 2 || strcmp(argv[1], "status") || argc > 3 || (argc == 3 && !json)) {
        return app_error(error, size, "zoom status [--json]");
    }
    return submit(context, 0, NULL, NULL, NULL, true, json, NULL, error, size);
}
static int config_reload(void *context, char *error, size_t size)
{
    AppRuntime *r = context;
    Config *candidate = malloc(sizeof *candidate);
    if (!candidate) {
        return app_error(error, size, "cannot allocate reload candidate");
    }
    Startup startup = r->app->startup; /* Startup arguments are immutable session storage. */
    int result = config_load(candidate, startup.config_path, startup.explicit_config, error, size);
    if (!result) {
        result = app_apply_overrides(candidate, &startup, error, size);
    }
    if (!result) {
        result = app_socket_path(candidate, error, size);
    }
    pthread_mutex_lock(&r->lock);
    candidate->virtual_enabled = r->config.virtual_enabled;
    pthread_mutex_unlock(&r->lock);
    if (!result) {
        result = submit(r, 0, NULL, NULL, NULL, false, false, candidate, error, size);
    }
    free(candidate);
    return result;
}
static int audio_read(void *context, CastRuntimeConsumer consumer, CastRuntimeAudioSource source,
                      uint64_t timestamp, float *samples, unsigned frames, uint64_t epoch,
                      char *error, size_t size)
{
    AppRuntime *r = context;
    if ((unsigned)consumer >= CAST_CONSUMER_COUNT || frames > 4800 || !frames) {
        return app_error(error, size, "invalid consumer audio request");
    }
    pthread_mutex_lock(&r->lock);
    bool allowed = !r->stopped && r->permitted[consumer] && epoch == r->epoch[consumer] &&
                   timestamp >= r->after[consumer] &&
                   (consumer != CAST_CONSUMER_NOTES || source == CAST_AUDIO_MIC);
    int result = allowed ? media_audio_selection(r->app->media, source, timestamp, samples, frames,
                                                 error, size)
                         : -1;
    if (!allowed) {
        memset(samples, 0, frames * 2 * sizeof *samples);
        snprintf(error, size, "consumer audio epoch retired or presentation private");
    }
    pthread_mutex_unlock(&r->lock);
    return result;
}
static int launch_notes(void *context, const Config *config, char *error, size_t size)
{
    AppRuntime *r = context;
    if (!authorize(context, "speech_teleprompter", error, size)) {
        return 5;
    }
    pthread_mutex_lock(&r->lock);
    if (r->notes_child > 0 && kill(r->notes_child, 0) == 0) {
        pthread_mutex_unlock(&r->lock);
        return 0;
    }
    char *argv[] = {(char *)CAST_APPLICATION_NAME, "--socket", (char *)config->socket_path,
                    "notes-window", NULL};
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    bool initialized = !rc;
    if (!rc) {
        rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    }
    pid_t child = 0;
    if (!rc) {
        rc = posix_spawn(&child, "/proc/self/exe", &actions, NULL, argv, environ);
    }
    if (initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (!rc) {
        r->notes_child = child;
    }
    pthread_mutex_unlock(&r->lock);
    if (rc) {
        return app_error(error, size, "cannot open notes window: %s", strerror(rc));
    }
    snprintf(error, size, "notes window launched");
    return 0;
}
int app_runtime_open(App *a, char *error, size_t size)
{
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!hooks) {
        return 0;
    }
    AppRuntime *r = calloc(1, sizeof *r);
    if (!r) {
        return app_error(error, size, "cannot allocate workflow host");
    }
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->wake, NULL);
    r->app = a;
    r->config = a->config;
    r->revision = 1;
    for (unsigned i = 0; i < CAST_CONSUMER_COUNT; ++i) {
        r->epoch[i] = 1;
        r->after[i] = cast_now_ns();
    }
    a->runtime = r;
    CastRuntimeHost host = {.context = r,
                            .authorize = authorize,
                            .settings = settings,
                            .settings_motion = settings_motion,
                            .audio_read = audio_read,
                            .client_command = app_runtime_client,
                            .launch_notes = launch_notes,
                            .motion_command = motion_command,
                            .config_reload = config_reload};
    cast_runtime_set_host(&host);
    app_runtime_sync(a);
    if (hooks->initialize && hooks->initialize(cast_runtime_host(), &a->config, error, size)) {
        app_runtime_close(a);
        return -1;
    }
    return 0;
}
void app_runtime_barrier(App *a, CastRuntimeBarrier reason, unsigned mask)
{
    AppRuntime *r = a->runtime;
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!r || !mask) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    uint64_t now = cast_now_ns();
    for (unsigned i = 0; i < CAST_CONSUMER_COUNT; ++i) {
        if (mask & (1u << i)) {
            ++r->epoch[i];
            r->after[i] = now;
        }
    }
    uint64_t epochs[CAST_CONSUMER_COUNT];
    memcpy(epochs, r->epoch, sizeof epochs);
    pthread_mutex_unlock(&r->lock);
    if (hooks && hooks->barrier) {
        hooks->barrier(reason, mask, epochs);
    }
}
void app_runtime_sync(App *a)
{
    AppRuntime *r = a->runtime;
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!r || !hooks) {
        return;
    }
    bool permitted[CAST_CONSUMER_COUNT] = {
        a->config.virtual_enabled && !a->state.virtual_paused && !a->state.virtual_frozen &&
            !a->state.virtual_blurred,
        a->state.recording && !a->state.record_cut && !a->state.record_paused &&
            !a->state.record_frozen && !a->state.record_blurred,
        a->state.stream_active && !a->state.stream_paused && !a->state.stream_frozen &&
            !a->state.stream_blurred,
        !a->state.group_paused && a->config.mic};
    unsigned changed = 0;
    pthread_mutex_lock(&r->lock);
    for (unsigned i = 0; i < CAST_CONSUMER_COUNT; ++i) {
        if (r->permitted[i] != permitted[i]) {
            changed |= 1u << i;
        }
    }
    pthread_mutex_unlock(&r->lock);
    if (changed) {
        app_runtime_barrier(
            a, a->state.group_paused ? CAST_BARRIER_GLOBAL_PRIVACY : CAST_BARRIER_PRESENTATION,
            changed);
    }
    CastRuntimeContext context = {.config = &a->config,
                                  .state = &a->state,
                                  .timestamp_ns = cast_now_ns(),
                                  .source_generation = a->source_generation,
                                  .recording_time_ns = media_record_duration(a->media),
                                  .recording_path = a->state.record_path,
                                  .recording_finalizing = a->record_finalizing};
    platform_notes_exclusion(
        a->platform,
        !strcmp(config_extension_value(&a->config, "notes", "exclude_from_capture"), "true"));
    context.notes_window_registered = platform_notes_registered(a->platform);
    context.notes_exclusion_effective = platform_notes_excluded(a->platform);
    Capabilities capabilities = platform_capabilities(a->platform);
    context.capabilities = &capabilities;
    edition_service_snapshot(a->edition, &context.entitlement);
    pthread_mutex_lock(&r->lock);
    r->timestamp = edition_service_checked_at(a->edition);
    r->entitlement = context.entitlement;
    r->motion_ready = compositor_motion_ready(a->compositor);
    if (memcmp(&r->config, &a->config, sizeof a->config)) {
        r->config = a->config;
        ++r->revision;
    }
    memcpy(r->permitted, permitted, sizeof permitted);
    memcpy(context.consumer_permitted, permitted, sizeof permitted);
    memcpy(context.consumer_epoch, r->epoch, sizeof r->epoch);
    if (r->notes_child > 0 && waitpid(r->notes_child, NULL, WNOHANG) == r->notes_child) {
        r->notes_child = 0;
    }
    pthread_mutex_unlock(&r->lock);
    compositor_runtime_context(a->compositor, &capabilities, a->source_generation);
    if (hooks->tick) {
        hooks->tick(&context);
    }
}
void app_runtime_drain(App *a)
{
    AppRuntime *r = a->runtime;
    if (!r) {
        return;
    }
    for (unsigned i = 0; i < RUNTIME_QUEUE; ++i) {
        pthread_mutex_lock(&r->lock);
        RuntimeRequest *request = &r->queue[i];
        if (!request->occupied || !request->queued) {
            pthread_mutex_unlock(&r->lock);
            continue;
        }
        request->queued = false;
        request->processing = true;
        bool stale = request->revision != r->revision;
        pthread_mutex_unlock(&r->lock);
        int result = 0;
        if (stale && !request->status) {
            result = app_error(request->output, sizeof request->output,
                               "session changed before commit; retry");
        } else if (request->status) {
            compositor_motion_status(a->compositor, request->output, sizeof request->output,
                                     request->json);
        } else {
            static const char *const features[] = {"cinematic_zoom", "transcription_subtitles",
                                                   "speech_teleprompter"};
            for (unsigned feature = 0; feature < 3 && !result; ++feature) {
                if ((request->required_features & (1u << feature)) &&
                    !authorize(r, features[feature], request->output, sizeof request->output)) {
                    result = -1;
                }
            }
            if (!result && request->prepared_motion) {
                if (!authorize(r, "cinematic_zoom", request->output, sizeof request->output)) {
                    result = -1;
                } else {
                    result = compositor_motion_adopt(a->compositor, request->prepared_motion,
                                                     request->output, sizeof request->output);
                }
                if (!result) {
                    request->prepared_motion = NULL;
                }
            }
            if (!result && request->motion &&
                compositor_motion_prepare(a->compositor, request->output, sizeof request->output)) {
                result = -1;
            }
            CastMotionEvent event = request->event;
            if (request->motion) {
                if (!event.timestamp_ns) {
                    event.timestamp_ns = cast_now_ns();
                }
                if (!event.source_generation) {
                    event.source_generation = a->source_generation;
                }
                if (!result) {
                    result = compositor_motion_event_validate(
                        a->compositor, &event, request->output, sizeof request->output);
                }
            }
            char *argv[CAST_MAX_ARGS] = {"settings"};
            int argc = 1;
            for (unsigned k = 0; k < request->pairs; ++k) {
                argv[argc++] = request->keys[k];
                argv[argc++] = request->values[k];
            }
            a->runtime_prepared = true;
            if (!result && request->configuration) {
                result = app_apply_candidate(a, &request->config, request->output,
                                             sizeof request->output);
                if (!result) {
                    a->defaults = request->config;
                    a->zoom_last = request->config.zoom_factor;
                    snprintf(request->output, sizeof request->output,
                             "configuration reloaded atomically; output states preserved");
                }
            } else if (!result && request->pairs) {
                result = app_command(a, argc, argv, request->output, sizeof request->output);
            }
            a->runtime_prepared = false;
            if (!result && request->motion) {
                result = compositor_motion_event(a->compositor, &event, request->output,
                                                 sizeof request->output);
            }
            app_runtime_sync(a);
        }
        pthread_mutex_lock(&r->lock);
        request->result = result;
        request->processing = false;
        request->complete = true;
        pthread_cond_broadcast(&r->wake);
        pthread_mutex_unlock(&r->lock);
    }
}
void app_runtime_startup_motion(App *a)
{
    AppRuntime *r = a->runtime;
    if (!r || !a->edition ||
        strcmp(config_extension_value(&a->config, "zoom", "motion"), "cinematic")) {
        return;
    }
    uint64_t now = cast_now_ns();
    pthread_mutex_lock(&r->lock);
    bool needed =
        !r->stopped && !r->motion_ready && !r->motion_pending && now >= r->motion_retry_after;
    pthread_mutex_unlock(&r->lock);
    char error[CAST_ERR];
    if (!needed || !authorize(r, "cinematic_zoom", error, sizeof error)) {
        return;
    }
    char *arguments[] = {"settings", "zoom.motion", "cinematic"};
    if (!edition_service_submit(a->edition, -1, 3, arguments, &a->config, error, sizeof error)) {
        pthread_mutex_lock(&r->lock);
        r->motion_pending = true;
        pthread_mutex_unlock(&r->lock);
    }
}
void app_runtime_internal_reply(App *a, int result, const char *message)
{
    AppRuntime *r = a->runtime;
    if (!r) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->motion_pending = false;
    r->motion_retry_after = cast_now_ns() + UINT64_C(5000000000);
    pthread_mutex_unlock(&r->lock);
    if (result) {
        fprintf(stderr, "cast: cinematic renderer preparation: %s\n", message);
    }
}
void app_runtime_commit(App *a)
{
    edition_service_path(a->edition, a->config.licensing_file);
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (a->runtime && hooks && hooks->commit_config) {
        hooks->commit_config(&a->config);
    }
    app_runtime_sync(a);
}
int app_runtime_safety(App *a, int argc, char **argv, char *output, size_t size)
{
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!a->runtime || !hooks || !hooks->safety_command) {
        return 1;
    }
    Config candidate = a->config;
    int result = hooks->safety_command(&candidate, argc, argv, output, size);
    if (!result) {
        a->config = candidate;
        app_runtime_sync(a);
        panel_transport_barrier(a->panel, a, false);
    }
    return result;
}
void app_runtime_captions(App *a, int lane, Frame *frame)
{
    AppRuntime *r = a->runtime;
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (!r || !frame->data || lane < 0 || lane > 2 || !hooks || !hooks->draw_captions) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    uint64_t epoch = r->epoch[lane];
    bool allowed = r->permitted[lane];
    pthread_mutex_unlock(&r->lock);
    if (allowed) {
        hooks->draw_captions((CastRuntimeConsumer)lane, frame, epoch);
    }
}
void app_runtime_recording_finished(App *a)
{
    const CastRuntimeHooks *hooks = cast_runtime_hooks();
    if (a->runtime && hooks && hooks->recording_finished) {
        hooks->recording_finished(a->state.record_path, media_record_duration(a->media));
    }
}
void app_runtime_stop(App *a)
{
    AppRuntime *r = a->runtime;
    if (!r) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    bool stopped = r->stopped;
    r->stopped = true;
    pthread_cond_broadcast(&r->wake);
    pthread_mutex_unlock(&r->lock);
    if (!stopped) {
        /* The first stop retires consumers while their services are alive.
         * close() may follow edition shutdown and must not call freed modules. */
        app_runtime_barrier(a, CAST_BARRIER_SHUTDOWN, ALL_CONSUMERS);
    }
}
void app_runtime_close(App *a)
{
    AppRuntime *r = a->runtime;
    if (!r) {
        return;
    }
    app_runtime_stop(a);
    cast_runtime_set_host(NULL);
    pthread_cond_destroy(&r->wake);
    pthread_mutex_destroy(&r->lock);
    free(r);
    a->runtime = NULL;
}
int app_runtime_notes_register(App *a, int argc, char **argv, int peer_pid, char *error,
                               size_t size)
{
    if (!a->runtime || argc != 3 || strcmp(argv[1], "--window-id")) {
        return app_error(error, size, "notes window registration unavailable or malformed");
    }
    char *end;
    errno = 0;
    unsigned long long id = strtoull(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || argv[2][0] == '-' || id > UINT32_MAX) {
        return app_error(error, size, "invalid notes window id");
    }
    AppRuntime *r = a->runtime;
    pthread_mutex_lock(&r->lock);
    pid_t launched = r->notes_child;
    pthread_mutex_unlock(&r->lock);
    if (peer_pid != launched) {
        return app_error(error, size,
                         "notes registration must come from the launched native client");
    }
    if (id && !authorize(r, "speech_teleprompter", error, size)) {
        return 5;
    }
    bool exclude =
        !strcmp(config_extension_value(&a->config, "notes", "exclude_from_capture"), "true");
    int result = platform_notes_register(a->platform, id, peer_pid, exclude, error, size);
    if (!result) {
        /* Refresh future live readbacks before ACK. Keep each explicitly held
         * Freeze frame: window visibility must not replace its presentation. */
        frame_free(&a->screen);
        frame_free(&a->virtual_raw);
        frame_free(&a->record_raw);
        frame_free(&a->stream_raw);
        media_stream_barrier(a->media);
        panel_transport_barrier(a->panel, a, true);
        app_runtime_sync(a);
    }
    return result;
}

static int workflow_parent(const char *path, char *e, size_t n)
{
    if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return app_error(e, n, "socket path must be absolute and shorter than %zu bytes",
                         sizeof(((struct sockaddr_un *)0)->sun_path));
    }
    char parent[PATH_MAX];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        slash[1] = 0;
    } else {
        *slash = 0;
    }
    struct stat st;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        return app_error(e, n, "socket parent must be owned by you and private (mode 0700): %s",
                         parent);
    }
    return 0;
}
int app_ipc_connect(const char *path, int timeout, char *e, size_t n)
{
    if (workflow_parent(path, e, n)) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
        return app_error(e, n, "no owned user-only cast socket at %s; start cast first", path);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return app_error(e, n, "socket: %s", strerror(errno));
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, path);
    if (connect(fd, (void *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        return app_error(e, n, "connect %s: %s", path, strerror(errno));
    }
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, timeout) <= 0) {
        close(fd);
        return app_error(e, n, "daemon connection timeout");
    }
    int result = 0;
    socklen_t len = sizeof result;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &len) || result) {
        close(fd);
        return app_error(e, n, "daemon connection failed: %s", strerror(result ? result : errno));
    }
    struct ucred cred;
    len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
        close(fd);
        return app_error(e, n, "daemon peer ownership mismatch");
    }
    return fd;
}
int app_runtime_client(const Config *config, int argc, const char *const *argv, char *output,
                       size_t size)
{
    char packet[CAST_IPC_MAX];
    if (argc < 1 || argc > CAST_MAX_ARGS) {
        return app_error(output, size, "invalid command arguments");
    }
    size_t length = cast_command_header_write(packet);
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        if (n > sizeof packet - length) {
            return app_error(output, size, "command exceeds IPC limit");
        }
        memcpy(packet + length, argv[i], n);
        length += n;
    }
    int fd = app_ipc_connect(config->socket_path, config->ipc_timeout_ms, output, size);
    if (fd < 0) {
        return -1;
    }
    if (send(fd, packet, length, MSG_NOSIGNAL) != (ssize_t)length) {
        close(fd);
        return app_error(output, size, "cannot send command: %s", strerror(errno));
    }
    struct pollfd poller = {fd, POLLIN, 0};
    if (poll(&poller, 1, cast_command_reply_timeout(argc, argv, config->ipc_timeout_ms)) <= 0) {
        close(fd);
        return app_error(output, size,
                         "command timed out; its completion is unknown (check status)");
    }
    ssize_t received = recv(fd, packet, sizeof packet - 1, MSG_TRUNC);
    close(fd);
    if (received < 2 || received >= (ssize_t)sizeof packet ||
        !cast_command_reply_valid(packet, (size_t)received)) {
        return app_error(output, size, "invalid daemon response");
    }
    packet[received] = 0;
    if ((size_t)received - 2 >= size) {
        return app_error(output, size, "daemon response exceeds client bound");
    }
    memcpy(output, packet + 2, (size_t)received - 1);
    return packet[0] == '0' ? 0 : packet[0] - '0';
}
