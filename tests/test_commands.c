/* Exercise the production controller without binding a control socket. */
#define main cast_cli_entry
int cast_cli_entry(int, char **);
#include "../src/main.c"
#undef main
#include <assert.h>
#include <libavutil/log.h>
#ifdef WITH_PANEL
static bool expect_application, application_auto_start;
static Config application_config;
int panel_run(const Config *config, char *error, size_t n)
{
    (void)config;
    (void)error;
    (void)n;
    assert(!"command tests must not open a panel");
    return -1;
}
int panel_run_application(const Config *config, int argc, const char *const *argv, bool auto_start,
                          char *error, size_t n)
{
    if (expect_application) {
        assert(argc > 0 && argv);
        application_config = *config;
        application_auto_start = auto_start;
        return 0;
    }
    return panel_run(config, error, n);
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
    app->state.virtual_paused = true;
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
    frame_free(&app->virtual);
    frame_free(&app->record);
    frame_free(&app->neutral);
    frame_free(&app->frozen);
    frame_free(&app->record_frozen);
    frame_free(&app->virtual_raw);
    frame_free(&app->record_raw);
    frame_free(&app->stream);
    frame_free(&app->stream_raw);
    frame_free(&app->stream_frozen);
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
static void presentation_controls(App *app, const char *configuration)
{
    char error[CAST_ERR];
    State initial = app->state;
    COMMAND(app, true, "virtual", "footer", "café — {date:%Y}");
    assert(!strcmp(app->config.pause_footer, "café — {date:%Y}"));
    COMMAND(app, true, "record", "footer", "Shared footer");
    assert(!strcmp(app->config.pause_footer, "Shared footer"));
    COMMAND(app, true, "settings", "output.blur_footer", "Blur {time:%H:%M}",
            "output.pause_footer_size", "18", "output.blur_footer_size", "20",
            "output.pause_text_gap", "32", "output.blur_text_gap", "0");
    assert(app->config.pause_footer_size == 18 && app->config.blur_footer_size == 20 &&
           app->config.pause_text_gap == 32 && app->config.blur_text_gap == 0);
    Config before = app->config;
    char long_footer[sizeof app->config.pause_footer + 1];
    memset(long_footer, 'a', sizeof long_footer - 1);
    long_footer[sizeof long_footer - 1] = 0;
    COMMAND(app, false, "virtual", "footer", long_footer);
    COMMAND(app, false, "virtual", "footer", "bad \xff");
    COMMAND(app, false, "record", "footer", "{unknown}");
    COMMAND(app, false, "record", "footer", "control\x01");
    COMMAND(app, false, "virtual", "footer", "text", "extra");
    COMMAND(app, false, "settings", "output.pause_footer_size", "7");
    COMMAND(app, false, "settings", "output.blur_footer_size", "257");
    COMMAND(app, false, "settings", "output.pause_text_gap", "-1");
    COMMAND(app, false, "settings", "output.blur_text_gap", "513");
    COMMAND(app, false, "settings", "output.blur_footer", "{time:%999999Y}");
    assert(!memcmp(&before, &app->config, sizeof before));
    /* IPC uses NUL as an argument boundary; an injected extra field fails arity. */
    char empty[] = "CAST1\0virtual\0footer\0\0";
    char injected[] = "CAST1\0record\0footer\0first\0second\0";
    char invalid[] = "CAST1\0virtual\0\0text\0";
    char *argv[CAST_MAX_ARGS];
    int argc;
    assert(decode_packet(empty, sizeof empty - 1, &argc, argv, error, sizeof error) == 0);
    assert(argc == 3 && !argv[2][0]);
    assert(app_command(app, argc, argv, response, sizeof response) == 0);
    assert(!app->config.pause_footer[0]);
    assert(decode_packet(injected, sizeof injected - 1, &argc, argv, error, sizeof error) == 0);
    assert(app_command(app, argc, argv, response, sizeof response) < 0);
    assert(!app->config.pause_footer[0]);
    assert(decode_packet(invalid, sizeof invalid - 1, &argc, argv, error, sizeof error) < 0);
    COMMAND(app, true, "record", "footer", "");
    COMMAND(app, true, "settings", "output.blur_footer", "");
    assert(!memcmp(&initial, &app->state, sizeof initial));
    /* Both families must affect the rendered result after a real config reload. */
    const char *style = "[camera]\nwidth_percent=25\n[output]\n"
                        "pause_title=WWW iii café\npause_subtitle=Same subtitle\n"
                        "pause_footer=Footer\nblur_title=WWW iii café\n"
                        "blur_subtitle=Same subtitle\nblur_footer=Footer\n";
    char config_text[2048];
    snprintf(config_text, sizeof config_text, "%spause_font=monospace\nblur_font=monospace\n",
             style);
    write_config(configuration, config_text);
    COMMAND(app, true, "config", "reload");
    Frame neutral = {0}, blurred = {0}, comparison = {0};
    assert(frame_copy(&neutral, &app->neutral) == 0);
    assert(frame_copy(&blurred, &app->screen) == 0);
    assert(compositor_blur(app->compositor, &app->config, &blurred, error, sizeof error) == 0);
    snprintf(config_text, sizeof config_text, "%spause_font=serif\nblur_font=serif\n", style);
    write_config(configuration, config_text);
    COMMAND(app, true, "config", "reload");
    assert(!strcmp(app->config.pause_font, "serif") && !strcmp(app->config.blur_font, "serif"));
    assert(!same_pixels(&neutral, &app->neutral));
    assert(frame_copy(&comparison, &app->screen) == 0);
    assert(compositor_blur(app->compositor, &app->config, &comparison, error, sizeof error) == 0);
    assert(!same_pixels(&blurred, &comparison));
    before = app->config;
    COMMAND(app, false, "settings", "output.pause_font", "monospace");
    assert(strstr(response, "config reload"));
    COMMAND(app, false, "settings", "output.blur_font", "monospace");
    assert(strstr(response, "config reload"));
    assert(!memcmp(&before, &app->config, sizeof before));
    assert(!memcmp(&initial, &app->state, sizeof initial));
    frame_free(&neutral);
    frame_free(&blurred);
    frame_free(&comparison);
    write_config(configuration, "[camera]\nwidth_percent=25\n");
    COMMAND(app, true, "config", "reload");
}
static void composition_controls(App *app, const char *configuration, const char *directory)
{
    char error[CAST_ERR], logo_path[PATH_MAX], bad_path[PATH_MAX];
    snprintf(logo_path, sizeof logo_path, "%s/logo.png", directory);
    snprintf(bad_path, sizeof bad_path, "%s/broken.png", directory);
    static const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc4,
        0x0f, 0xbe, 0x8b, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
        0xcf, 0xc0, 0xd0, 0x80, 0x0f, 0x33, 0x8c, 0x0c, 0x05, 0x00, 0x92, 0x87, 0x5f, 0xc1, 0x49,
        0x57, 0x44, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    FILE *image = fopen(logo_path, "wb");
    assert(image && fwrite(png, 1, sizeof png, image) == sizeof png && fclose(image) == 0);
    write_config(bad_path, "\x89PNG\r\n\x1a\ntruncated");
    State initial = app->state;
    write_config(configuration, "[camera]\nwidth_percent=25\ncorner_order=bottom,left,top\n"
                                "[composition]\nlayout_order=screen,stage,camera\n"
                                "preset_order=conversation,coding,demo\n");
    COMMAND(app, true, "config", "reload");
    COMMAND(app, true, "layout", "prev"); /* Missing current value wraps from the end. */
    assert(!strcmp(app->config.layout, "camera"));
    COMMAND(app, true, "layout", "next");
    assert(!strcmp(app->config.layout, "screen"));
    COMMAND(app, true, "layout", "prev");
    assert(!strcmp(app->config.layout, "camera"));
    COMMAND(app, true, "layout", "stage");
    COMMAND(app, true, "layout", "next");
    assert(!strcmp(app->config.layout, "camera"));
    COMMAND(app, true, "layout", "prev");
    assert(!strcmp(app->config.layout, "stage"));
    COMMAND(app, true, "camera", "anchor", "prev");
    assert(!strcmp(app->config.anchor, "top"));
    COMMAND(app, true, "camera", "anchor", "next");
    assert(!strcmp(app->config.anchor, "bottom"));
    COMMAND(app, true, "camera", "anchor", "next");
    assert(!strcmp(app->config.anchor, "left"));
    COMMAND(app, true, "camera", "anchor", "prev");
    assert(!strcmp(app->config.anchor, "bottom"));
    COMMAND(app, true, "camera", "shape", "rectangle");
    COMMAND(app, true, "camera", "shape", "prev");
    assert(!strcmp(app->config.shape, "circle"));
    COMMAND(app, true, "camera", "shape", "next");
    assert(!strcmp(app->config.shape, "rectangle"));
    COMMAND(app, true, "camera", "aspect", "native");
    COMMAND(app, true, "camera", "aspect", "prev");
    assert(!strcmp(app->config.aspect, "1:1"));
    COMMAND(app, true, "camera", "aspect", "next");
    assert(!strcmp(app->config.aspect, "native"));
    COMMAND(app, true, "preset", "prev");
    assert(!strcmp(app->current_preset, "demo"));
    COMMAND(app, true, "preset", "next");
    assert(!strcmp(app->current_preset, "conversation"));
    COMMAND(app, true, "preset", "prev");
    assert(!strcmp(app->current_preset, "demo"));
    COMMAND(app, true, "preset", "prev");
    assert(!strcmp(app->current_preset, "coding"));
    COMMAND(app, true, "screen", "size", "83%");
    COMMAND(app, true, "screen", "size", "-5%");
    assert(app->config.screen_width_percent == 78);
    COMMAND(app, true, "screen", "margin", "20");
    COMMAND(app, true, "screen", "radius", "18");
    COMMAND(app, true, "screen", "border", "width", "3");
    COMMAND(app, true, "screen", "border", "color", "#abcdef");
    COMMAND(app, true, "screen", "background", "gradient");
    assert(app->config.screen_margin == 20 && app->config.screen_radius == 18 &&
           app->config.screen_border_width == 3 && app->config.screen_border_color == 0xabcdef &&
           !strcmp(app->config.screen_background, "gradient"));
    COMMAND(app, true, "settings", "background.source", "camera", "background.gradient_from",
            "#123456", "background.gradient_via", "#654321", "background.gradient_to", "#abcdef",
            "background.gradient_via_enabled", "false", "background.gradient_angle", "270");
    COMMAND(app, true, "settings", "background.gradient_waypoint", "25", "screen.background_color",
            "#332211", "screen.background_blur_radius", "4", "screen.background_brightness", "0.4");
    COMMAND(app, true, "logo", "path", logo_path);
    assert(!app->config.logo_enabled && !strcmp(app->config.logo_path, logo_path));
    COMMAND(app, true, "logo", "on");
    COMMAND(app, true, "logo", "size", "20%");
    COMMAND(app, true, "logo", "size", "+5%");
    COMMAND(app, true, "logo", "anchor", "top");
    COMMAND(app, true, "logo", "margin", "30", "40");
    COMMAND(app, true, "logo", "opacity", "40%");
    assert(app->config.logo_enabled && app->config.logo_width_percent == 25 &&
           !strcmp(app->config.logo_anchor, "top") && app->config.logo_margin_x == 30 &&
           app->config.logo_margin_y == 40 && app->config.logo_opacity == .4);
    COMMAND(app, true, "text", "set", "Static café {literal}\nSecond line");
    assert(!app->config.text_enabled);
    COMMAND(app, true, "text", "on");
    COMMAND(app, true, "text", "size", "32");
    COMMAND(app, true, "text", "color", "#fedcba");
    COMMAND(app, true, "text", "anchor", "right");
    COMMAND(app, true, "text", "margin", "10", "12");
    COMMAND(app, true, "text", "opacity", "0.6");
    assert(app->config.text_enabled && app->config.text_size == 32 &&
           app->config.text_color == 0xfedcba && !strcmp(app->config.text_anchor, "right") &&
           app->config.text_margin_x == 10 && app->config.text_margin_y == 12 &&
           app->config.text_opacity == .6);
    COMMAND(app, true, "text", "font", "monospace");
    Frame mono = {0}, serif = {0};
    assert(compositor_render(app->compositor, &app->config, &app->screen, NULL, NULL, true, &mono,
                             error, sizeof error) == 0);
    COMMAND(app, true, "text", "font", "serif");
    assert(!strcmp(app->config.text_font, "serif"));
    assert(compositor_render(app->compositor, &app->config, &app->screen, NULL, NULL, true, &serif,
                             error, sizeof error) == 0);
    assert(!same_pixels(&mono, &serif));
    Config before = app->config, defaults_before = app->defaults;
    COMMAND(app, false, "logo", "path", "/tmp/cast-no-such-logo-file-42.png");
    assert(strstr(response, "logo"));
    COMMAND(app, false, "logo", "path", bad_path);
    assert(strstr(response, "decode"));
    COMMAND(app, false, "text", "font", ":file=/tmp/cast-no-such-font-file-42.ttf");
    assert(strstr(response, "font"));
    COMMAND(app, false, "text", "font", "");
    COMMAND(app, false, "text", "set", "bad \xff");
    COMMAND(app, false, "text", "set", "control\x01");
    COMMAND(app, false, "text", "set", "value", "extra");
    COMMAND(app, false, "text", "margin", "20", "-1");
    COMMAND(app, false, "logo", "anchor", "free");
    COMMAND(app, false, "logo", "size", "101%");
    COMMAND(app, false, "logo", "opacity", "101%");
    COMMAND(app, false, "text", "opacity", "nan");
    COMMAND(app, false, "screen", "size", "10");
    COMMAND(app, false, "screen", "border", "color", "red");
    COMMAND(app, false, "screen", "margin", "4097");
    COMMAND(app, false, "layout", "previous");
    COMMAND(app, false, "camera", "shape", "prev", "extra");
    assert(!memcmp(&before, &app->config, sizeof before));
    assert(compositor_render(app->compositor, &app->config, &app->screen, NULL, NULL, true, &mono,
                             error, sizeof error) == 0);
    assert(same_pixels(&mono, &serif)); /* Failed font/image changes retain prepared resources. */
    write_config(configuration, "[text]\nfont=:file=/tmp/cast-no-such-font-file-42.ttf\n");
    COMMAND(app, false, "config", "reload");
    assert(!memcmp(&before, &app->config, sizeof before));
    assert(!memcmp(&defaults_before, &app->defaults, sizeof defaults_before));
    write_config(configuration, "[logo]\npath=/tmp/cast-no-such-logo-file-42.png\n");
    COMMAND(app, false, "config", "reload");
    assert(!memcmp(&before, &app->config, sizeof before));
    assert(!memcmp(&defaults_before, &app->defaults, sizeof defaults_before));
    char empty[] = "CAST1\0text\0set\0\0";
    char injected[] = "CAST1\0text\0set\0first\0second\0";
    char *argv[CAST_MAX_ARGS];
    int argc;
    assert(decode_packet(empty, sizeof empty - 1, &argc, argv, error, sizeof error) == 0);
    assert(app_command(app, argc, argv, response, sizeof response) == 0);
    assert(!app->config.text_content[0] && app->config.text_enabled);
    assert(decode_packet(injected, sizeof injected - 1, &argc, argv, error, sizeof error) == 0);
    assert(app_command(app, argc, argv, response, sizeof response) < 0);
    COMMAND(app, true, "logo", "toggle");
    assert(!app->config.logo_enabled);
    char clear_logo[] = "CAST1\0logo\0path\0\0";
    assert(decode_packet(clear_logo, sizeof clear_logo - 1, &argc, argv, error, sizeof error) == 0);
    assert(app_command(app, argc, argv, response, sizeof response) == 0);
    assert(!app->config.logo_path[0]);
    COMMAND(app, false, "logo", "on");
    COMMAND(app, true, "reset");
    assert(
        !app->config.logo_enabled && !app->config.text_enabled && !app->config.logo_path[0] &&
        !app->config.text_content[0] && !strcmp(app->config.text_font, app->defaults.text_font) &&
        !strcmp(app->config.background_source, app->defaults.background_source) &&
        app->config.gradient_from == app->defaults.gradient_from &&
        app->config.gradient_via == app->defaults.gradient_via &&
        app->config.gradient_to == app->defaults.gradient_to &&
        app->config.gradient_via_enabled == app->defaults.gradient_via_enabled &&
        app->config.gradient_angle == app->defaults.gradient_angle &&
        app->config.gradient_waypoint == app->defaults.gradient_waypoint &&
        app->config.screen_margin == app->defaults.screen_margin &&
        app->config.screen_radius == app->defaults.screen_radius &&
        app->config.screen_border_width == app->defaults.screen_border_width &&
        app->config.screen_border_color == app->defaults.screen_border_color &&
        !strcmp(app->config.screen_background, app->defaults.screen_background) &&
        app->config.screen_background_blur_radius == app->defaults.screen_background_blur_radius &&
        app->config.screen_background_brightness == app->defaults.screen_background_brightness &&
        app->config.logo_width_percent == app->defaults.logo_width_percent &&
        app->config.logo_opacity == app->defaults.logo_opacity &&
        app->config.text_size == app->defaults.text_size &&
        app->config.text_color == app->defaults.text_color &&
        app->config.text_opacity == app->defaults.text_opacity);
    assert(!memcmp(&initial, &app->state, sizeof initial));
    frame_free(&mono);
    frame_free(&serif);
    unlink(logo_path);
    unlink(bad_path);
    write_config(configuration, "[camera]\nwidth_percent=25\n");
    COMMAND(app, true, "config", "reload");
}

static void output_modes(App *app)
{
    char error[CAST_ERR];
    COMMAND(app, true, "settings", "output.blur_title", "", "output.blur_subtitle", "",
            "output.blur_radius", "4");
    COMMAND(app, false, "settings", "output.pause_font", "sans");
    assert(strstr(response, "config reload"));
    COMMAND(app, true, "virtual", "subtitle", "");
    COMMAND(app, true, "camera", "anchor", "top");
    COMMAND(app, true, "camera", "anchor", "left");
    tick(app);
    app->config.virtual_enabled = false;
    COMMAND(app, true, "virtual", "resume");
    media_audio_status(app->media, error, sizeof error);
    assert(strstr(error, "\"virtual_silent\":true"));
    app->config.virtual_enabled = true;
    COMMAND(app, true, "virtual", "resume");
    COMMAND(app, true, "virtual", "freeze");
    COMMAND(app, true, "virtual", "blur", "on");
    assert(app->state.virtual_frozen && app->state.virtual_blurred && !app->state.record_frozen &&
           !app->state.record_blurred);
    Frame snapshot = {0}, expected = {0};
    assert(frame_copy(&snapshot, &app->frozen) == 0);
    assert(frame_copy(&expected, &snapshot) == 0);
    assert(compositor_blur(app->compositor, &app->config, &expected, error, sizeof error) == 0);
    assert(same_pixels(&expected, &app->virtual));
    assert(app_output_frames(app, error, sizeof error) == 0);
    assert(same_pixels(&expected, &app->virtual) && same_pixels(&snapshot, &app->frozen));
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
           app->state.record_blurred && app->state.virtual_frozen && app->state.virtual_blurred);
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
    COMMAND(app, true, "virtual", "pause");
    assert(same_pixels(&app->frozen, &app->neutral));
    COMMAND(app, true, "virtual", "unfreeze");
    COMMAND(app, true, "virtual", "blur", "off");
    COMMAND(app, true, "virtual", "resume");
    COMMAND(app, true, "settings", "record.countdown", "0");
    tick(app);
    frame_free(&snapshot);
    frame_free(&expected);
}

static void application_entry(const char *configuration, const char *socket_pathname)
{
#ifndef WITH_PANEL
    (void)configuration;
    (void)socket_pathname;
#endif
    char error[CAST_ERR];
    Startup startup;
    char *headless[] = {"cast", "--headless", "--width", "320", "status"};
    assert(startup_parse(5, headless, &startup, error, sizeof error) == 4 && startup.headless &&
           startup.override_count == 1);
#ifdef WITH_PANEL
    char *arguments[] = {"cast",
                         "--config",
                         (char *)configuration,
                         "--socket",
                         (char *)socket_pathname,
                         "--backend",
                         "synthetic",
                         "--width",
                         "320",
                         "--height",
                         "240",
                         "--no-camera",
                         "--no-virtual",
                         "panel"};
    expect_application = true;
    assert(!cast_cli_entry(sizeof arguments / sizeof arguments[0], arguments));
    assert(!application_auto_start && application_config.camera_width_percent == 25 &&
           application_config.width == 320 && application_config.height == 240 &&
           !application_config.camera_enabled && !application_config.virtual_enabled &&
           !strcmp(application_config.socket_path, socket_pathname));
    assert(!cast_cli_entry(sizeof arguments / sizeof arguments[0] - 1, arguments));
    assert(application_auto_start && application_config.camera_width_percent == 25);
    expect_application = false;
#endif
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
    application_entry(configuration, socket_pathname);
    App *app = new_app(configuration, socket_pathname);
    Config exclusion_before = app->config;
    State exclusion_state = app->state;
    COMMAND(app, false, "capture", "exclusion", "transparent");
    COMMAND(app, false, "capture", "mask-color", "#123456");
    COMMAND(app, false, "settings", "capture.exclusion", "transparent");
    COMMAND(app, false, "settings", "batch", "capture.mask_color", "#abcdef",
            "output.pause_background", "#123456");
    assert(!memcmp(&exclusion_before, &app->config, sizeof exclusion_before));
    assert(!memcmp(&exclusion_state, &app->state, sizeof exclusion_state));
    Config initial_config = app->config;
    State initial_state = app->state;
    COMMAND(app, false, "live", "resume");
    COMMAND(app, false, "annotations", "live", "keys", "off");
    COMMAND(app, false, "preview", "target", "live");
    COMMAND(app, false, "settings", "annotations.live_clicks", "false");
    COMMAND(app, false, "stream", "resume");
    COMMAND(app, false, "stream", "toggle");
    COMMAND(app, false, "stream", "freeze");
    COMMAND(app, false, "stream", "blur", "on");
    assert(!memcmp(&initial_config, &app->config, sizeof initial_config));
    assert(!memcmp(&initial_state, &app->state, sizeof initial_state));
    COMMAND(app, true, "stream", "stop");
    assert(!app->state.stream_active && app->state.stream_paused);
    COMMAND(app, true, "stream", "stop");
    COMMAND(app, true, "stream", "status", "--json");
    assert(strstr(response, "\"state\":\"stopped\""));
    COMMAND(app, true, "preview", "target", "stream");
    COMMAND(app, true, "preview", "target", "virtual");
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\"stream\":{") && strstr(response, "\"virtual\":{") &&
           !strstr(response, "\"live\":"));
    presentation_controls(app, configuration);
    composition_controls(app, configuration, directory);
    COMMAND(app, true, "status", "--json");
    assert(strstr(response, "\"state\":\"paused\""));
    State message_state = app->state;
    Frame neutral_before = {0};
    assert(frame_copy(&neutral_before, &app->neutral) == 0);
    COMMAND(app, true, "virtual", "message", "Screen sharing paused");
    assert(!strcmp(app->config.pause_text, "Screen sharing paused"));
    assert(memcmp(&message_state, &app->state, sizeof message_state) == 0);
    assert(memcmp(neutral_before.data, app->neutral.data,
                  (size_t)app->neutral.stride * app->neutral.height));
    Config message_config = app->config;
    char too_long[sizeof app->config.pause_text + 1];
    memset(too_long, 'x', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    COMMAND(app, false, "virtual", "message", too_long);
    COMMAND(app, false, "virtual", "message", "invalid \xff");
    COMMAND(app, false, "virtual", "message", "{unknown}");
    COMMAND(app, false, "virtual", "message", "{time:%999999Y}");
    COMMAND(app, false, "virtual", "message", "control\x01");
    COMMAND(app, false, "virtual", "message", "extra", "argument");
    assert(memcmp(&message_config, &app->config, sizeof message_config) == 0);
    COMMAND(app, true, "virtual", "message", "");
    assert(!app->config.pause_text[0] && app->state.virtual_paused);
    COMMAND(app, true, "virtual", "message", "café paused");
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
    COMMAND(app, true, "virtual", "resume");
    tick(app);
    COMMAND(app, true, "virtual", "freeze");
    assert(app->state.virtual_frozen && app->frozen.data);
    Frame freeze_before = {0};
    assert(frame_copy(&freeze_before, &app->frozen) == 0);
    COMMAND(app, true, "virtual", "message", "New paused label");
    assert(app->state.virtual_frozen && !app->state.virtual_paused);
    assert(!memcmp(freeze_before.data, app->frozen.data,
                   (size_t)app->frozen.stride * app->frozen.height));
    frame_free(&freeze_before);
    COMMAND(app, true, "virtual", "pause");
    COMMAND(app, true, "virtual", "unfreeze");
    assert(app->state.virtual_paused);
    COMMAND(app, true, "resume");
    assert(app->state.virtual_paused); /* Group resume cannot undo an independent pause. */
    COMMAND(app, true, "virtual", "resume");
    COMMAND(app, true, "record", "start", recording_path);
    tick(app);
    output_modes(app);
    COMMAND(app, true, "pause");
    COMMAND(app, true, "pause");
    assert(app->state.virtual_paused && app->state.record_paused);
    COMMAND(app, true, "record", "pause");
    COMMAND(app, true, "resume");
    assert(!app->state.virtual_paused && app->state.record_paused);
    COMMAND(app, true, "record", "resume");
    assert(app->state.recording && !app->state.record_paused);
    COMMAND(app, true, "virtual", "pause");
    COMMAND(app, true, "reset");
    assert(app->state.virtual_paused && app->state.recording);
    assert(app->config.camera_width_percent == 25);

    write_config(configuration, "[composition]\nlayout=screen\n[output]\nwidth=640\n");
    COMMAND(app, true, "config", "reload");
    assert(!strcmp(app->config.layout, "screen") && app->config.width == 320);
    assert(app->state.virtual_paused && app->state.recording);
    Config before = app->config, defaults_before = app->defaults;
    write_config(configuration, "[composition]\nlayout=camera\n[output]\nbackend=invalid\n");
    COMMAND(app, false, "config", "reload");
    assert(memcmp(&app->config, &before, sizeof before) == 0);
    assert(memcmp(&app->defaults, &defaults_before, sizeof defaults_before) == 0);
    write_config(configuration, "[composition]\nlayout=camera\n[record]\nvideo_codec=ffv1\n");
    COMMAND(app, false, "config", "reload");
    assert(memcmp(&app->config, &before, sizeof before) == 0);
    assert(app->state.recording && app->state.virtual_paused);
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
    COMMAND(app, true, "virtual", "resume");
    COMMAND(app, true, "quit");
    assert(app->state.virtual_paused && !app->state.virtual_frozen && !app->state.recording);
    free_app(app);
    unlink(recording_path);
    unlink(configuration);
    rmdir(directory);
    fputs("production command, privacy, toggle, reload, countdown and status tests passed\n",
          stderr);
    return 0;
}
