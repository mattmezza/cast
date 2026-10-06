/* Production daemon transaction path with an MIT fake workflow provider.
 * No inference implementation, licensed algorithm or capture device is used. */
#define main cast_runtime_cli_entry
int cast_runtime_cli_entry(int, char **);
#include "../src/main.c"
#undef main
#ifdef WITH_PANEL
int panel_run(const Config *config, char *error, size_t size)
{
    (void)config;
    return app_error(error, size, "workflow fixture never opens the control panel");
}
int panel_run_application(const Config *config, int argc, const char *const *argv, bool auto_start,
                          char *error, size_t size)
{
    (void)argc;
    (void)argv;
    (void)auto_start;
    return panel_run(config, error, size);
}
#endif
#include "license_store.h"
#include "pro_extension.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

static pthread_mutex_t preparation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t preparation_wake = PTHREAD_COND_INITIALIZER;
static bool hold_preparation, preparation_entered, release_preparation;
static atomic_uint preparations, commits, barriers, cancelled;
static uint64_t observed_epochs[CAST_CONSUMER_COUNT];

static int fake_verify(const unsigned char *data, size_t bytes, const CastEditionIdentity *identity,
                       int64_t now, CastLicenseInfo *info, char *error, size_t size)
{
    (void)identity;
    (void)now;
    memset(info, 0, sizeof *info);
    if (bytes != 8 || memcmp(data, "accepted", 8)) {
        info->state = CAST_LICENSE_INVALID;
        snprintf(error, size, "test-only bytes rejected");
        return -1;
    }
    info->state = CAST_LICENSE_VALID;
    info->perpetual = info->platform_supported = true;
    info->feature_count = 3;
    strcpy(info->features[0], "cinematic_zoom");
    strcpy(info->features[1], "transcription_subtitles");
    strcpy(info->features[2], "speech_teleprompter");
    return 0;
}
static int fake_update(const unsigned char *data, size_t bytes, const char *binary,
                       const CastEditionIdentity *identity, CastUpdateMetadata *metadata,
                       char *error, size_t size)
{
    (void)data;
    (void)bytes;
    (void)binary;
    (void)identity;
    (void)metadata;
    snprintf(error, size, "test double has no update verifier");
    return -1;
}
static bool fake_ready(void)
{
    return true;
}
static int fake_prepare(const Config *config, char *error, size_t size)
{
    atomic_fetch_add(&preparations, 1);
    if (!strcmp(config_extension_value(config, "notes", "font"), "missing-test-font")) {
        snprintf(error, size, "test candidate resource is unavailable");
        return -1;
    }
    pthread_mutex_lock(&preparation_lock);
    preparation_entered = true;
    pthread_cond_broadcast(&preparation_wake);
    while (hold_preparation && !release_preparation) {
        pthread_cond_wait(&preparation_wake, &preparation_lock);
    }
    pthread_mutex_unlock(&preparation_lock);
    return 0;
}
static void fake_commit(const Config *config)
{
    assert(config);
    atomic_fetch_add(&commits, 1);
}
static void fake_barrier(CastRuntimeBarrier reason, unsigned mask, const uint64_t *epochs)
{
    (void)reason;
    atomic_fetch_add(&barriers, 1);
    for (unsigned i = 0; i < CAST_CONSUMER_COUNT; ++i) {
        if (mask & (1u << i)) {
            assert(epochs[i] > observed_epochs[i]);
            observed_epochs[i] = epochs[i];
        }
    }
}
static int fake_safety(Config *config, int argc, char **argv, char *output, size_t size)
{
    if (argc != 2 || strcmp(argv[0], "transcription") || strcmp(argv[1], "off")) {
        return 1;
    }
    if (config_set_value(config, "transcription.enabled", "false", output, size)) {
        return 2;
    }
    atomic_fetch_add(&cancelled, 1);
    snprintf(output, size, "recognition consent retired before acknowledgement");
    return 0;
}
static const CastRuntimeHooks runtime_hooks = {.prepare_config = fake_prepare,
                                               .commit_config = fake_commit,
                                               .barrier = fake_barrier,
                                               .safety_command = fake_safety};
static const CastExtensionCapability capabilities[] = {{.id = "cinematic_zoom",
                                                        .implemented = true,
                                                        .compiled = true,
                                                        .platform_supported = true,
                                                        .dependency_ready = fake_ready},
                                                       {.id = "transcription_subtitles",
                                                        .implemented = true,
                                                        .compiled = true,
                                                        .platform_supported = true,
                                                        .dependency_ready = fake_ready},
                                                       {.id = "speech_teleprompter",
                                                        .implemented = true,
                                                        .compiled = true,
                                                        .platform_supported = true,
                                                        .dependency_ready = fake_ready}};
static CastProExtension provider = {.api_version = CAST_PRO_EXTENSION_API,
                                    .struct_size = sizeof provider,
                                    .provider_id = "cast-pro",
                                    .verify_license = fake_verify,
                                    .verify_update = fake_update,
                                    .capabilities = capabilities,
                                    .capability_count = 3,
                                    .runtime = &runtime_hooks};
const CastProExtension *cast_pro_extension_v2(void)
{
    provider.settings = cast_shared_extension_schema(&provider.settings_count);
    return &provider;
}
typedef struct {
    unsigned pairs;
    const char *keys[3], *values[3];
    atomic_bool finished;
    int result;
    char output[256];
} WorkerRequest;
static void *worker_settings(void *context)
{
    WorkerRequest *request = context;
    const CastRuntimeHost *host = cast_runtime_host();
    request->result = host->settings(host->context, request->pairs, request->keys, request->values,
                                     request->output, sizeof request->output);
    atomic_store(&request->finished, true);
    return NULL;
}
static pthread_t begin_request(WorkerRequest *request, bool held)
{
    pthread_mutex_lock(&preparation_lock);
    hold_preparation = held;
    preparation_entered = release_preparation = false;
    pthread_mutex_unlock(&preparation_lock);
    atomic_store(&request->finished, false);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, worker_settings, request));
    return worker;
}
static void finish_request(App *app, WorkerRequest *request, pthread_t worker)
{
    for (unsigned i = 0; i < 3000 && !atomic_load(&request->finished); ++i) {
        app_runtime_sync(app);
        app_runtime_drain(app);
        usleep(1000);
    }
    assert(atomic_load(&request->finished));
    pthread_join(worker, NULL);
}
int main(void)
{
    char directory[] = "/tmp/cast-workflow-runtime-XXXXXX", error[CAST_ERR];
    assert(mkdtemp(directory));
    App *app = calloc(1, sizeof *app);
    assert(app);
    config_defaults(&app->config);
    strcpy(app->config.backend, "synthetic");
    strcpy(app->config.output_device, "none");
    app->config.camera_enabled = app->config.virtual_enabled = false;
    app->config.width = 320;
    app->config.height = 180;
    snprintf(app->config.record_dir, sizeof app->config.record_dir, "%s", directory);
    snprintf(app->config.licensing_file, sizeof app->config.licensing_file, "%s/license.json",
             directory);
    assert(!config_set_value(&app->config, "transcription.enabled", "true", error, sizeof error));
    app->defaults = app->config;
    app->compositor = compositor_create();
    app->platform = platform_open(&app->config, error, sizeof error);
    app->media = media_open(&app->config, error, sizeof error);
    app->panel = panel_transport_create();
    assert(app->compositor && app->platform && app->media && app->panel);
    assert(!frame_alloc(&app->neutral, 320, 180));
    assert(!compositor_neutral(app->compositor, &app->config, &app->neutral, error, sizeof error));
    assert(!cast_license_store_write(app->config.licensing_file, (const unsigned char *)"accepted",
                                     8, error, sizeof error));
    app->edition = edition_service_open(app->config.licensing_file, error, sizeof error);
    assert(app->edition);
    CastEditionSnapshot entitlement;
    for (unsigned i = 0; i < 3000; ++i) {
        edition_service_snapshot(app->edition, &entitlement);
        if (entitlement.license.state == CAST_LICENSE_VALID) {
            break;
        }
        usleep(1000);
    }
    assert(entitlement.license.state == CAST_LICENSE_VALID);
    assert(!app_runtime_open(app, error, sizeof error));
    State output_states = app->state;
    WorkerRequest request = {.pairs = 1, .keys = {"notes.font_size"}, .values = {"50"}};
    pthread_t worker = begin_request(&request, true);
    pthread_mutex_lock(&preparation_lock);
    while (!preparation_entered) {
        pthread_cond_wait(&preparation_wake, &preparation_lock);
    }
    pthread_mutex_unlock(&preparation_lock);
    assert(!atomic_load(&request.finished));
    assert(!strcmp(config_extension_value(&app->config, "notes", "font_size"), "36"));
    char *off[] = {"transcription", "off"};
    assert(!app_runtime_safety(app, 2, off, error, sizeof error));
    assert(atomic_load(&cancelled) == 1);
    assert(!strcmp(config_extension_value(&app->config, "transcription", "enabled"), "false"));
    pthread_mutex_lock(&preparation_lock);
    release_preparation = true;
    pthread_cond_broadcast(&preparation_wake);
    pthread_mutex_unlock(&preparation_lock);
    finish_request(app, &request, worker);
    assert(request.result && !atomic_load(&commits));
    assert(!strcmp(config_extension_value(&app->config, "notes", "font_size"), "36"));
    worker = begin_request(&request, false);
    finish_request(app, &request, worker);
    assert(!request.result && atomic_load(&commits) == 1);
    assert(!strcmp(config_extension_value(&app->config, "notes", "font_size"), "50"));
    assert(!memcmp(&output_states, &app->state, sizeof output_states));
    Config before = app->config;
    unsigned prepare_count = atomic_load(&preparations);
    request.pairs = 2;
    request.keys[0] = "notes.font_size";
    request.values[0] = "60";
    request.keys[1] = "notes.speed_lines_per_minute";
    request.values[1] = "-1";
    worker = begin_request(&request, false);
    finish_request(app, &request, worker);
    assert(request.result && atomic_load(&preparations) == prepare_count);
    assert(!memcmp(&before, &app->config, sizeof before));
    request.pairs = 1;
    request.keys[0] = "notes.font";
    request.values[0] = "missing-test-font";
    worker = begin_request(&request, false);
    finish_request(app, &request, worker);
    assert(request.result && !memcmp(&before, &app->config, sizeof before));
    unsigned previous_barriers = atomic_load(&barriers);
    app_runtime_barrier(app, CAST_BARRIER_GLOBAL_PRIVACY, (1u << CAST_CONSUMER_COUNT) - 1);
    assert(atomic_load(&barriers) == previous_barriers + 1);
    for (unsigned i = 0; i < CAST_CONSUMER_COUNT; ++i) {
        assert(observed_epochs[i] > 1);
    }
    /* Expiring permission during expensive preparation cannot acknowledge a
     * new live recognizer activation when its main-thread commit arrives. */
    request.pairs = 1;
    request.keys[0] = "transcription.enabled";
    request.values[0] = "true";
    worker = begin_request(&request, true);
    pthread_mutex_lock(&preparation_lock);
    while (!preparation_entered) {
        pthread_cond_wait(&preparation_wake, &preparation_lock);
    }
    pthread_mutex_unlock(&preparation_lock);
    assert(!unlink(app->config.licensing_file));
    for (unsigned i = 0; i < 4000; ++i) {
        edition_service_snapshot(app->edition, &entitlement);
        app_runtime_sync(app);
        if (entitlement.license.state == CAST_LICENSE_MISSING) {
            break;
        }
        usleep(1000);
    }
    assert(entitlement.license.state == CAST_LICENSE_MISSING);
    pthread_mutex_lock(&preparation_lock);
    release_preparation = true;
    pthread_cond_broadcast(&preparation_wake);
    pthread_mutex_unlock(&preparation_lock);
    finish_request(app, &request, worker);
    assert(request.result && atomic_load(&commits) == 1);
    assert(!strcmp(config_extension_value(&app->config, "transcription", "enabled"), "false"));
    app_runtime_stop(app);
    unsigned stopped_barriers = atomic_load(&barriers);
    app_runtime_stop(app);
    assert(atomic_load(&barriers) == stopped_barriers);
    edition_service_close(app->edition);
    app->edition = NULL;
    panel_transport_destroy(app->panel, app);
    media_close(app->media);
    platform_close(app->platform);
    compositor_destroy(app->compositor);
    app_runtime_close(app);
    frame_free(&app->neutral);
    const char *files[] = {".license.json.session"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", directory, files[i]);
        assert(!unlink(path));
    }
    assert(!rmdir(directory));
    free(app);
    puts("workflow runtime: atomic preparation/ACK, stale-write rejection, fast stop and epochs "
         "pass");
    return 0;
}
