/* Exercise the production controller without binding a control socket. */
#define main cast_cli_entry
int cast_cli_entry(int, char **);
#include "../src/main.c"
#undef main
#include <assert.h>
#include <libavutil/log.h>
#ifdef WITH_PANEL
int panel_run(const Config *config, char *error, size_t n)
{
    (void)config;
    (void)error;
    (void)n;
    assert(!"command tests must not open a panel");
    return -1;
}
#endif

static char response[CAST_IPC_MAX - 16];
static void expect_command(App *app, bool success, int argc, const char **args)
{
    char *argv[CAST_MAX_ARGS];
    assert(argc <= CAST_MAX_ARGS);
    for (int i = 0; i < argc; i++) {
        argv[i] = (char *)args[i];
    }
    int result = app_command(app, argc, argv, response, sizeof response);
    if ((result == 0) != success) {
        fprintf(stderr, "unexpected command result for %s: %s\n", args[0], response);
    }
    assert((result == 0) == success);
}
#define COMMAND(app, success, ...)                                                                 \
    do {                                                                                           \
        const char *args[] = {__VA_ARGS__};                                                        \
        expect_command(app, success, (int)(sizeof args / sizeof args[0]), args);                   \
    } while (0)

static void write_config(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}
static void startup_override(Startup *startup, const char *flag, const char *value)
{
    for (size_t i = 0; i < NOPT; i++) {
        if (!strcmp(options[i].flag, flag)) {
            startup->override[startup->override_count] = &options[i];
            startup->values[startup->override_count++] = value;
            return;
        }
    }
    assert(!"unknown test startup override");
}
static App *new_app(const char *config_pathname, const char *socket_pathname)
{
    App *app = calloc(1, sizeof *app);
    assert(app);
    app->startup.explicit_config = true;
    snprintf(app->startup.config_path, sizeof app->startup.config_path, "%s", config_pathname);
    startup_override(&app->startup, "--backend", "synthetic");
    startup_override(&app->startup, "--camera-device", "synthetic");
    startup_override(&app->startup, "--output-device", "none");
    startup_override(&app->startup, "--socket", socket_pathname);
    startup_override(&app->startup, "--width", "320");
    startup_override(&app->startup, "--height", "240");
    startup_override(&app->startup, "--fps", "20");
    char error[CAST_ERR];
    assert(config_load(&app->config, config_pathname, true, error, sizeof error) == 0);
    assert(app_apply_overrides(&app->config, &app->startup, error, sizeof error) == 0);
    app->defaults = app->config;
    app->state.live_paused = true;
    app->zoom_last = app->config.zoom_factor;
    app->config.zoom_factor = 1;
    app->compositor = compositor_create();
    app->platform = platform_open(&app->config, error, sizeof error);
    app->media = media_open(&app->config, error, sizeof error);
    assert(app->compositor && app->platform && app->media);
    assert(frame_alloc(&app->neutral, app->config.width, app->config.height) == 0);
    assert(compositor_neutral(app->compositor, &app->config, &app->neutral, error, sizeof error) ==
           0);
    assert(media_privacy(app->media, true, false, false, error, sizeof error) == 0);
    app->source_generation = platform_source_generation(app->platform);
    tick(app);
    return app;
}
static void free_app(App *app)
{
    media_close(app->media);
    platform_close(app->platform);
    compositor_destroy(app->compositor);
    frame_free(&app->screen);
    frame_free(&app->camera);
    frame_free(&app->live);
    frame_free(&app->record);
    frame_free(&app->neutral);
    frame_free(&app->frozen);
    frame_free(&app->record_frozen);
    frame_free(&app->live_raw);
    frame_free(&app->record_raw);
    free(app);
}
static void wait_finalization(App *app)
{
    uint64_t deadline = cast_now_ns() + UINT64_C(5000000000);
    while (media_record_finalizing(app->media) && cast_now_ns() < deadline) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(!media_record_finalizing(app->media));
    char error[CAST_ERR];
    media_record_error(app->media, error, sizeof error);
    assert(!error[0]);
    tick(app);
}
static bool same_pixels(const Frame *left, const Frame *right)
{
    return left->data && right->data && left->width == right->width &&
           left->height == right->height && left->stride == right->stride &&
           !memcmp(left->data, right->data, (size_t)left->stride * left->height);
}
static void output_modes(App *app)
{
    char error[CAST_ERR];
    COMMAND(app, true, "settings", "output.blur_title", "", "output.blur_subtitle", "",
            "output.blur_radius", "4");
    COMMAND(app, false, "settings", "output.pause_font", "sans");
    assert(strstr(response, "config reload"));
    COMMAND(app, true, "live", "subtitle", "");
    COMMAND(app, true, "camera", "anchor", "top");
    COMMAND(app, true, "camera", "anchor", "left");
    tick(app);
    COMMAND(app, true, "live", "freeze");
    COMMAND(app, true, "live", "blur", "on");
    assert(app->state.live_frozen && app->state.live_blurred && !app->state.record_frozen &&
           !app->state.record_blurred);
    Frame snapshot = {0}, expected = {0};
    assert(frame_copy(&snapshot, &app->frozen) == 0);
    assert(frame_copy(&expected, &snapshot) == 0);
    assert(compositor_blur(app->compositor, &app->config, &expected, error, sizeof error) == 0);
    assert(same_pixels(&expected, &app->live));
    assert(app_output_frames(app, error, sizeof error) == 0);
    assert(same_pixels(&expected, &app->live) && same_pixels(&snapshot, &app->frozen));
    COMMAND(app, true, "record", "freeze");
    COMMAND(app, true, "record", "blur", "on");
    assert(app->record_frozen.data != app->frozen.data);
    assert(frame_copy(&snapshot, &app->record_frozen) == 0);
    assert(frame_copy(&expected, &snapshot) == 0);
    assert(compositor_blur(app->compositor, &app->config, &expected, error, sizeof error) == 0);
    memset(app->screen.data, 0x3f, (size_t)app->screen.stride * app->screen.height);
    memset(app->camera.data, 0x7f, (size_t)app->camera.stride * app->camera.height);
    assert(app_output_frames(app, error, sizeof error) == 0);
    assert(same_pixels(&expected, &app->record) && same_pixels(&snapshot, &app->record_frozen));
    COMMAND(app, true, "record", "pause");
    assert(same_pixels(&app->record, &app->neutral));
    assert(same_pixels(&app->record_frozen, &app->neutral));
    COMMAND(app, true, "settings", "record.countdown", "1");
    COMMAND(app, true, "record", "cut");
    assert(app->state.record_cut && app->state.record_paused && app->state.record_frozen &&
           app->state.record_blurred && app->state.live_frozen && app->state.live_blurred);
    char path[PATH_MAX];
    strcpy(path, app->state.record_path);
    uint64_t cut_duration = media_record_duration(app->media);
    COMMAND(app, true, "record", "resume");
    assert(app->countdown && app->countdown_resume && app->state.record_cut);
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\"countdown_kind\":\"resume\""));
    COMMAND(app, true, "record", "cut");
    assert(!app->countdown && app->state.record_cut && !strcmp(path, app->state.record_path));
    app->countdown_deadline = 0;
    tick(app);
    assert(app->state.record_cut && media_record_duration(app->media) == cut_duration);
    COMMAND(app, true, "record", "resume");
    COMMAND(app, true, "record", "pause");
    assert(!app->countdown && app->state.record_cut);
    COMMAND(app, true, "record", "toggle");
    assert(!app->state.record_paused && app->state.record_cut);
    COMMAND(app, true, "record", "resume");
    COMMAND(app, true, "record", "toggle");
    assert(!app->countdown && app->state.record_paused && app->state.record_cut);
    COMMAND(app, true, "record", "resume");
    app->countdown_deadline = 0;
    tick(app);
    assert(!app->countdown && !app->state.record_cut && app->state.recording &&
           app->state.record_paused && app->state.record_frozen && app->state.record_blurred &&
           !strcmp(path, app->state.record_path));
    COMMAND(app, true, "record", "resume");
    COMMAND(app, true, "record", "unfreeze");
    COMMAND(app, true, "record", "blur", "off");
    COMMAND(app, true, "live", "pause");
    assert(same_pixels(&app->frozen, &app->neutral));
    COMMAND(app, true, "live", "unfreeze");
    COMMAND(app, true, "live", "blur", "off");
    COMMAND(app, true, "live", "resume");
    COMMAND(app, true, "settings", "record.countdown", "0");
    tick(app);
    frame_free(&snapshot);
    frame_free(&expected);
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    char directory[] = "/tmp/cast-commands-test-XXXXXX";
    assert(mkdtemp(directory));
    char configuration[PATH_MAX], socket_pathname[PATH_MAX], recording_path[PATH_MAX];
    snprintf(configuration, sizeof configuration, "%s/cast.conf", directory);
    snprintf(socket_pathname, sizeof socket_pathname, "%s/cast.sock", directory);
    snprintf(recording_path, sizeof recording_path, "%s/control.mkv", directory);
    write_config(configuration, "[camera]\nwidth_percent=25\n");
    App *app = new_app(configuration, socket_pathname);
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\"state\":\"paused\""));
    State message_state = app->state;
    Frame neutral_before = {0};
    assert(frame_copy(&neutral_before, &app->neutral) == 0);
    COMMAND(app, true, "live", "message", "Screen sharing paused");
    assert(!strcmp(app->config.pause_text, "Screen sharing paused"));
    assert(memcmp(&message_state, &app->state, sizeof message_state) == 0);
    assert(memcmp(neutral_before.data, app->neutral.data,
                  (size_t)app->neutral.stride * app->neutral.height));
    Config message_config = app->config;
    char too_long[sizeof app->config.pause_text + 1];
    memset(too_long, 'x', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    COMMAND(app, false, "live", "message", too_long);
    COMMAND(app, false, "live", "message", "invalid \xff");
    COMMAND(app, false, "live", "message", "extra", "argument");
    assert(memcmp(&message_config, &app->config, sizeof message_config) == 0);
    COMMAND(app, true, "live", "message", "");
    assert(!app->config.pause_text[0] && app->state.live_paused);
    COMMAND(app, true, "live", "message", "café paused");
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\"message\":\"café paused\""));
    frame_free(&neutral_before);
    Config settings_before = app->config;
    COMMAND(app, false, "settings", "camera.radius", "36", "camera.border_width", "999");
    assert(memcmp(&settings_before, &app->config, sizeof settings_before) == 0);
    COMMAND(app, false, "settings", "output.width", "640", "camera.radius", "36");
    assert(memcmp(&settings_before, &app->config, sizeof settings_before) == 0);
    COMMAND(app, false, "settings", "keys.enabled", "true", "camera.radius", "36");
    assert(memcmp(&settings_before, &app->config, sizeof settings_before) == 0);
    COMMAND(app, true, "settings", "camera.radius", "36", "camera.border_width", "4");
    assert(app->config.radius == 36 && app->config.border_width == 4);
    assert(memcmp(&message_state, &app->state, sizeof message_state) == 0);
    COMMAND(app, false, "layout", "screen", "extra");
    COMMAND(app, false, "camera", "size", "95%");
    COMMAND(app, false, "zoom", "set", "nan");
    COMMAND(app, false, "record", "toggle");
    COMMAND(app, true, "camera", "size", "+5%");
    assert(app->config.camera_width_percent == 30);
    COMMAND(app, true, "zoom", "set", "2.5");
    COMMAND(app, true, "zoom", "toggle");
    assert(app->config.zoom_factor == 1);
    COMMAND(app, true, "zoom", "toggle");
    assert(app->config.zoom_factor == 2.5);
    COMMAND(app, true, "live", "resume");
    tick(app);
    COMMAND(app, true, "live", "freeze");
    assert(app->state.live_frozen && app->frozen.data);
    Frame freeze_before = {0};
    assert(frame_copy(&freeze_before, &app->frozen) == 0);
    COMMAND(app, true, "live", "message", "New paused label");
    assert(app->state.live_frozen && !app->state.live_paused);
    assert(!memcmp(freeze_before.data, app->frozen.data,
                   (size_t)app->frozen.stride * app->frozen.height));
    frame_free(&freeze_before);
    COMMAND(app, true, "live", "pause");
    COMMAND(app, true, "live", "unfreeze");
    assert(app->state.live_paused);
    COMMAND(app, true, "resume");
    assert(app->state.live_paused); /* Group resume cannot undo an independent pause. */
    COMMAND(app, true, "live", "resume");
    COMMAND(app, true, "record", "start", recording_path);
    tick(app);
    output_modes(app);
    COMMAND(app, true, "pause");
    COMMAND(app, true, "pause");
    assert(app->state.live_paused && app->state.record_paused);
    COMMAND(app, true, "record", "pause");
    COMMAND(app, true, "resume");
    assert(!app->state.live_paused && app->state.record_paused);
    COMMAND(app, true, "record", "resume");
    assert(app->state.recording && !app->state.record_paused);
    COMMAND(app, true, "live", "pause");
    COMMAND(app, true, "reset");
    assert(app->state.live_paused && app->state.recording);
    assert(app->config.camera_width_percent == 25);

    write_config(configuration, "[composition]\nlayout=screen\n[output]\nwidth=640\n");
    COMMAND(app, true, "config", "reload");
    assert(!strcmp(app->config.layout, "screen") && app->config.width == 320);
    assert(app->state.live_paused && app->state.recording);
    Config before = app->config, defaults_before = app->defaults;
    write_config(configuration, "[composition]\nlayout=camera\n[output]\nbackend=invalid\n");
    COMMAND(app, false, "config", "reload");
    assert(memcmp(&app->config, &before, sizeof before) == 0);
    assert(memcmp(&app->defaults, &defaults_before, sizeof defaults_before) == 0);
    write_config(configuration, "[composition]\nlayout=camera\n[record]\nvideo_codec=ffv1\n");
    COMMAND(app, false, "config", "reload");
    assert(memcmp(&app->config, &before, sizeof before) == 0);
    assert(app->state.recording && app->state.live_paused);
    COMMAND(app, true, "record", "stop");
    assert(strstr(response, recording_path) && !app->state.recording);
    if (media_record_finalizing(app->media)) {
        COMMAND(app, false, "record", "start", recording_path);
    }
    wait_finalization(app);
    COMMAND(app, false, "record", "start", recording_path);
    COMMAND(app, false, "record", "toggle");

    write_config(configuration, "[record]\ncountdown=1\n");
    COMMAND(app, true, "config", "reload");
    COMMAND(app, true, "record", "start", "/tmp/never-created-by-cancelled-cast-test.mkv");
    assert(app->countdown);
    COMMAND(app, true, "pause");
    assert(!app->countdown);
    app->countdown_deadline = 0;
    tick(app);
    assert(!app->state.recording);
    COMMAND(app, true, "record", "start", "/tmp/never-created-by-cancelled-cast-test.mkv");
    COMMAND(app, true, "record", "stop");
    assert(!app->countdown && !app->state.recording);
    COMMAND(app, true, "record", "start", "/tmp/never-created-by-cancelled-cast-test.mkv");
    COMMAND(app, true, "record", "pause");
    assert(!app->countdown && !app->state.recording);

    /* Valid UTF-8 remains intact; arbitrary filename bytes remain valid JSON. */
    strcpy(app->config.mic_source, "mic \"quoted\" \\ route\n");
    strcpy(app->config.desktop_source, "desktop café");
    strcpy(app->config.virtual_name, "cast\tvirtual");
    strcpy(app->state.record_path, "capture \"é\" \\ newline\n byte\xff");
    strcpy(app->state.last_error, "diagnostic\twith\rcontrols");
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\\u00ff") && strstr(response, "\\u000a") &&
           strstr(response, "desktop café"));
    puts(response); /* check-unit parses this artifact with the standard JSON parser. */
    memset(app->state.record_path, 1, sizeof app->state.record_path - 1);
    app->state.record_path[sizeof app->state.record_path - 1] = 0;
    COMMAND(app, false, "status", "--json");
    assert(strstr(response, "IPC limit"));
    COMMAND(app, true, "live", "resume");
    COMMAND(app, true, "quit");
    assert(app->state.live_paused && !app->state.live_frozen && !app->state.recording);
    free_app(app);
    unlink(recording_path);
    unlink(configuration);
    rmdir(directory);
    fputs("production command, privacy, toggle, reload, countdown and status tests passed\n",
          stderr);
    return 0;
}
