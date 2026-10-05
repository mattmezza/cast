/* Verify panel edits through the real controller with synthetic media only. */
#define main cast_panel_routes_cli_entry
int cast_panel_routes_cli_entry(int, char **);
#include "../src/main.c"
#undef main
#include <assert.h>
#include <libavutil/log.h>

static int route_command(PanelClient *, int, const char *const *, char *, size_t);
static int route_setting(PanelClient *, const char *, const char *, char *, size_t);
#define panel_client_command route_command
#define panel_client_setting route_setting
#pragma GCC poison panel_client_frame
#include "../src/panel.c"
#undef panel_client_command
#undef panel_client_setting

static int route_command(PanelClient *client, int argc, const char *const *args, char *error,
                         size_t size)
{
    char *argv[CAST_MAX_ARGS];
    for (int i = 0; i < argc; i++) {
        argv[i] = (char *)args[i];
    }
    char response[CAST_ERR];
    int result = app_command((App *)client, argc, argv, response, sizeof response);
    snprintf(error, size, "%s", result ? response : "");
    return result;
}
static int route_setting(PanelClient *client, const char *key, const char *value, char *error,
                         size_t size)
{
    const char *args[] = {"settings", key, value};
    return route_command(client, 3, args, error, size);
}
static void edit_field(Panel *panel, App *app, const char *key, const char *value)
{
    panel->snapshot.config = app->config;
    panel->snapshot.state = app->state;
    panel->snapshot.capabilities = platform_capabilities(app->platform);
    panel->snapshot.connected = true;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (!strcmp(fields[i].key, key)) {
            apply_setting(panel, (int)i, value, false);
            if (panel->error[0]) {
                fprintf(stderr, "%s: %s\n", key, panel->error);
            }
            assert(!panel->error[0]);
            return;
        }
    }
    assert(!"unknown panel field");
}
static void camera_geometry(App *app, int *x, int *y, int *w, int *h)
{
    char error[CAST_ERR];
    assert(!compositor_geometry(&app->config, 640, 480, x, y, w, h, error, sizeof error));
}
static double luminance(Clay_Color c)
{
    double rgb[] = {c.r / 255.0, c.g / 255.0, c.b / 255.0};
    for (size_t i = 0; i < 3; i++) {
        rgb[i] = rgb[i] <= .04045 ? rgb[i] / 12.92 : pow((rgb[i] + .055) / 1.055, 2.4);
    }
    return .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
}
static void check_text_contrast(void)
{
    Clay_Color ink[] = {foreground,           secondary,           muted, accent, danger,
                        {224, 197, 102, 255}, {131, 221, 182, 255}};
    Clay_Color bg[] = {background, surface, control, hovered, {35, 63, 83, 255}};
    for (size_t i = 0; i < sizeof ink / sizeof *ink; i++) {
        for (size_t j = 0; j < sizeof bg / sizeof *bg; j++) {
            /* Muted labels never use hover/selected fills: disabled uses control,
             * hovered chip metadata uses secondary, selected buttons use accent. */
            if (i == 2 && j >= 3) {
                continue;
            }
            assert((luminance(ink[i]) + .05) / (luminance(bg[j]) + .05) >= 4.5);
        }
    }
}
int main(void)
{
    check_text_contrast();
    /* A control has one home: connection fields never leak into Compose, and
     * the camera size draft cannot be stranded behind a duplicate field slot. */
    assert(sizeof section_names / sizeof *section_names == 6);
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        assert(field_section(&fields[i]) >= 0 && field_section(&fields[i]) <= 8);
        for (size_t j = i + 1; j < FIELD_COUNT; j++) {
            assert(strcmp(fields[i].key, fields[j].key));
        }
    }
    assert(field_section(&fields[find_field("camera.width_percent")]) == 1);
    assert(field_section(&fields[find_field("audio.mic_source")]) == 6);
    assert(field_section(&fields[find_field("stream.key_file")]) == 7);
    assert(field_section(&fields[find_field("record.countdown")]) == 8);
    assert(field_section(&fields[find_field("output.pause_text")]) == 5);
    SDL_MouseWheelEvent wheel = {.y = .25f, .direction = SDL_MOUSEWHEEL_NORMAL};
    assert(wheel_delta(&wheel) == 1.5f);
    wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    assert(wheel_delta(&wheel) == -1.5f);
    wheel.y = -1;
    assert(wheel_delta(&wheel) == 6);
    assert(countdown_seconds(0) == 0);
    assert(countdown_seconds(1) == 1);
    assert(countdown_seconds(1000000000ULL) == 1);
    assert(countdown_seconds(1000000001ULL) == 2);
    assert(countdown_seconds(60000000000ULL) == 60);
    /* A delayed Apply acknowledgement must not discard typing done afterward. */
    FieldEdit edit = {.dirty = true, .pending = 1, .revision = 2, .submitted_revision = 1};
    acknowledge_edit(&edit, false);
    assert(edit.dirty && !edit.pending);
    edit.pending = 2;
    edit.submitted_revision = edit.revision;
    acknowledge_edit(&edit, true);
    assert(edit.dirty && !edit.pending);
    edit.pending = 3;
    acknowledge_edit(&edit, false);
    assert(!edit.dirty && !edit.pending);

    av_log_set_level(AV_LOG_ERROR);
    App *app = calloc(1, sizeof *app);
    Panel *panel = calloc(1, sizeof *panel);
    assert(app && panel);
    /* Clipboard availability is asynchronous. Empty paste preserves selection,
     * while a subsequent UTF-8 payload replaces it through the same edit path. */
    panel->widget_count = 1;
    panel->widgets[0] = (Widget){.id = 1000, .type = W_FIELD, .enabled = true, .index = 0};
    panel->active_text = panel->focus = 1000;
    panel->select_all = true;
    panel->caret = 7;
    strcpy(panel->edit[0].value, "Private session");
    panel->edit[0].dirty = true;
    panel->edit[0].revision = 7;
    insert_text(panel, "");
    assert(!strcmp(panel->edit[0].value, "Private session"));
    assert(panel->select_all && panel->caret == 7 && panel->edit[0].dirty);
    assert(panel->edit[0].revision == 7);
    insert_text(panel, "Private café");
    assert(!strcmp(panel->edit[0].value, "Private café"));
    assert(!panel->select_all && panel->caret == strlen("Private café"));
    assert(panel->edit[0].revision == 8);
    memset(panel, 0, sizeof *panel);
    config_defaults(&app->config);
    strcpy(app->config.backend, "synthetic");
    strcpy(app->config.camera_device, "synthetic");
    strcpy(app->config.output_device, "none");
    app->config.width = 320;
    app->config.height = 240;
    app->config.zoom_follow = false;
    app->config.zoom_factor = 1;
    app->zoom_last = 2;
    app->state.virtual_paused = true;
    app->defaults = app->config;
    char error[CAST_ERR];
    app->compositor = compositor_create();
    app->platform = platform_open(&app->config, error, sizeof error);
    app->media = media_open(&app->config, error, sizeof error);
    assert(app->compositor && app->platform && app->media);
    assert(!frame_alloc(&app->neutral, app->config.width, app->config.height));
    assert(!compositor_neutral(app->compositor, &app->config, &app->neutral, error, sizeof error));
    panel->client = (PanelClient *)app;
    State initial_state = app->state;

    strcpy(app->config.anchor, "free");
    app->config.camera_x = 120;
    app->config.camera_y = 70;
    int x, y, width, height, next_x, next_y, next_width, next_height;
    camera_geometry(app, &x, &y, &width, &height);
    edit_field(panel, app, "camera.width_percent", "30");
    camera_geometry(app, &next_x, &next_y, &next_width, &next_height);
    assert(fabs(app->config.camera_width_percent - 30) < 1e-9);
    assert(abs((2 * x + width) - (2 * next_x + next_width)) <= 1);
    assert(abs((2 * y + height) - (2 * next_y + next_height)) <= 1);

    strcpy(app->config.anchor, "bottom-right");
    int retained_y = app->config.camera_y;
    edit_field(panel, app, "camera.x", "45");
    assert(!strcmp(app->config.anchor, "free"));
    assert(app->config.camera_x == 45 && app->config.camera_y == retained_y);
    strcpy(app->config.anchor, "bottom-right");
    edit_field(panel, app, "camera.y", "30");
    assert(!strcmp(app->config.anchor, "free"));
    assert(app->config.camera_x == 45 && app->config.camera_y == 30);
    edit_field(panel, app, "camera.shape", "circle");
    edit_field(panel, app, "camera.aspect", "1:1");
    edit_field(panel, app, "camera.mirror", "false");
    edit_field(panel, app, "camera.background", "gradient");
    edit_field(panel, app, "camera.background_color", "#243040");
    edit_field(panel, app, "camera.background_blur_radius", "80");
    edit_field(panel, app, "camera.background_brightness", "0.3");
    assert(!strcmp(app->config.camera_background, "gradient"));
    assert(app->config.camera_background_color == 0x243040);
    assert(app->config.camera_background_blur_radius == 80);
    assert(app->config.camera_background_brightness == .3);
    assert(!strcmp(app->config.shape, "circle") && !strcmp(app->config.aspect, "1:1"));
    assert(!app->config.mirror);
    edit_field(panel, app, "camera.anchor", "top-left");
    assert(!strcmp(app->config.anchor, "top-left"));
    edit_field(panel, app, "camera.anchor", "free");
    assert(!strcmp(app->config.anchor, "free"));
    const char *edges[] = {"top", "bottom", "left", "right"};
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        edit_field(panel, app, "camera.anchor", edges[i]);
        camera_geometry(app, &x, &y, &width, &height);
        if (i < 2) {
            assert(abs(2 * x + width - app->config.width) <= 1);
        } else {
            assert(abs(2 * y + height - app->config.height) <= 1);
        }
        assert(!strcmp(app->config.anchor, edges[i]));
    }

    edit_field(panel, app, "output.pause_text", "Session {date:%Y-%m-%d}");
    edit_field(panel, app, "output.pause_subtitle", "Returns at {time}");
    edit_field(panel, app, "output.blur_title", "Private {datetime}");
    edit_field(panel, app, "output.blur_subtitle", "");
    edit_field(panel, app, "output.pause_footer", "Recorded {date}");
    edit_field(panel, app, "output.blur_footer", "");
    edit_field(panel, app, "output.pause_footer_size", "20");
    edit_field(panel, app, "output.blur_footer_size", "18");
    edit_field(panel, app, "output.pause_text_gap", "0");
    edit_field(panel, app, "output.blur_text_gap", "512");
    assert(!strcmp(app->config.pause_footer, "Recorded {date}"));
    assert(!app->config.blur_footer[0] && app->config.pause_footer_size == 20);
    assert(app->config.pause_text_gap == 0 && app->config.blur_text_gap == 512);
    edit_field(panel, app, "output.blur_radius", "24");
    edit_field(panel, app, "output.blur_opacity", "0.75");
    assert(!strcmp(app->config.pause_text, "Session {date:%Y-%m-%d}"));
    assert(!strcmp(app->config.pause_subtitle, "Returns at {time}"));
    assert(!app->config.blur_subtitle[0] && app->config.blur_radius == 24);
    assert(app->config.blur_opacity == .75);

    /* New compositing fields remain ordinary daemon settings and preserve state. */
    edit_field(panel, app, "screen.width_percent", "71.5");
    edit_field(panel, app, "screen.margin", "27");
    edit_field(panel, app, "screen.radius", "22");
    edit_field(panel, app, "screen.border_width", "3");
    edit_field(panel, app, "screen.border_color", "#80c9ff");
    edit_field(panel, app, "screen.background", "gradient");
    edit_field(panel, app, "screen.background_color", "#20252b");
    edit_field(panel, app, "screen.background_blur_radius", "90");
    edit_field(panel, app, "screen.background_brightness", "0.3");
    edit_field(panel, app, "background.source", "camera");
    edit_field(panel, app, "background.gradient_from", "#101827");
    edit_field(panel, app, "background.gradient_via", "#26354a");
    edit_field(panel, app, "background.gradient_to", "#080b12");
    edit_field(panel, app, "background.gradient_via_enabled", "false");
    edit_field(panel, app, "background.gradient_angle", "42.5");
    edit_field(panel, app, "background.gradient_waypoint", "62.5");
    assert(app->config.screen_width_percent == 71.5 && app->config.screen_margin == 27);
    assert(app->config.screen_radius == 22 && app->config.screen_border_width == 3);
    assert(app->config.screen_border_color == 0x80c9ff);
    assert(!strcmp(app->config.screen_background, "gradient"));
    assert(app->config.screen_background_blur_radius == 90 &&
           app->config.screen_background_brightness == .3);
    assert(!strcmp(app->config.background_source, "camera"));
    assert(!app->config.gradient_via_enabled && app->config.gradient_angle == 42.5 &&
           app->config.gradient_waypoint == 62.5);
    edit_field(panel, app, "logo.enabled", "false");
    edit_field(panel, app, "logo.path", "");
    edit_field(panel, app, "logo.anchor", "top");
    edit_field(panel, app, "logo.width_percent", "15");
    edit_field(panel, app, "logo.margin_x", "20");
    edit_field(panel, app, "logo.margin_y", "30");
    edit_field(panel, app, "logo.opacity", "0.4");
    assert(!app->config.logo_enabled && !app->config.logo_path[0]);
    assert(!strcmp(app->config.logo_anchor, "top") && app->config.logo_width_percent == 15);
    assert(app->config.logo_margin_x == 20 && app->config.logo_margin_y == 30 &&
           app->config.logo_opacity == .4);
    edit_field(panel, app, "text.content", "Static café {date}");
    edit_field(panel, app, "text.font", "Noto Sans");
    edit_field(panel, app, "text.size", "31");
    edit_field(panel, app, "text.color", "#80c9ff");
    edit_field(panel, app, "text.anchor", "bottom");
    edit_field(panel, app, "text.margin_x", "25");
    edit_field(panel, app, "text.margin_y", "35");
    edit_field(panel, app, "text.opacity", "0.6");
    edit_field(panel, app, "text.enabled", "true");
    assert(!strcmp(app->config.text_content, "Static café {date}") && app->config.text_enabled);
    assert(app->config.text_size == 31 && app->config.text_color == 0x80c9ff);
    assert(!strcmp(app->config.text_anchor, "bottom") && app->config.text_margin_x == 25);
    assert(app->config.text_margin_y == 35 && app->config.text_opacity == .6);
    Config before_invalid = app->config;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (!strcmp(fields[i].key, "background.gradient_waypoint")) {
            apply_setting(panel, (int)i, "100", false);
            assert(panel->error[0]);
        }
    }
    assert(!memcmp(&before_invalid, &app->config, sizeof before_invalid));
    panel->error[0] = 0;

    edit_field(panel, app, "zoom.factor", "3.25");
    assert(app->config.zoom_factor == 3.25 && app->zoom_last == 3.25);
    const char *toggle[] = {"zoom", "toggle"};
    assert(!route_command(panel->client, 2, toggle, error, sizeof error));
    assert(app->config.zoom_factor == 1);
    assert(!route_command(panel->client, 2, toggle, error, sizeof error));
    assert(app->config.zoom_factor == 3.25);
    edit_field(panel, app, "camera.radius", "40");
    assert(app->config.radius == 40);
    assert(!memcmp(&initial_state, &app->state, sizeof initial_state));

    /* Drafts stage locally, validate as a batch, then clear only for their own ack. */
    panel->snapshot.config = app->config;
    panel->snapshot.state = app->state;
    int shape_index = find_field("camera.shape");
    int mirror_index = find_field("camera.mirror");
    assert(shape_index >= 0 && mirror_index >= 0);
    stage_field(panel, shape_index, "rounded");
    stage_field(panel, mirror_index, "true");
    assert(!strcmp(app->config.shape, "circle") && !app->config.mirror);
    panel->view = VIEW_SECTION;
    panel->open_section = 1;
    panel->view_scroll[3] = -180;
    apply_section(panel, 1);
    assert(panel->view == VIEW_SECTION && panel->open_section == 1 &&
           panel->view_scroll[3] == -180);
    assert(!panel->error[0] && !strcmp(app->config.shape, "rounded") && app->config.mirror);
    acknowledge_edit(&panel->edit[shape_index], false);
    acknowledge_edit(&panel->edit[mirror_index], false);
    assert(!section_dirty(panel, 1));
    panel->snapshot.config = app->config;
    stage_field(panel, shape_index, "circle");
    Widget revert = {.enabled = true, .action = A_REVERT, .index = 1};
    activate(panel, &revert);
    assert(!section_dirty(panel, 1) && !strcmp(app->config.shape, "rounded"));
    panel->snapshot.state.stream_active = true;
    assert(!writable(panel, &fields[find_field("stream.server_url")]));
    assert(writable(panel, &fields[find_field("output.blur_title")]));
    panel->snapshot.state.stream_active = false;
    /* Successful daemon commands own target selection; navigation never emits one. */
    const char *resume_virtual[] = {"virtual", "resume"};
    assert(!route_command(panel->client, 2, resume_virtual, error, sizeof error));
    assert(!strcmp(app->config.preview_target, "virtual"));
    Widget manual_preview = {.enabled = true, .action = A_PREVIEW, .index = 2};
    activate(panel, &manual_preview);
    assert(!strcmp(app->config.preview_target, "stream"));
    /* Failed actions retain the manually chosen target. */
    const char *invalid_resume[] = {"stream", "resume"};
    assert(route_command(panel->client, 2, invalid_resume, error, sizeof error));
    assert(!strcmp(app->config.preview_target, "stream"));

    panel->snapshot.connected = true;
    panel->snapshot.config.virtual_enabled = true;
    panel->snapshot.state.virtual_paused = false;
    panel->snapshot.state.virtual_frozen = panel->snapshot.state.virtual_blurred = true;
    assert(!strcmp(virtual_status(&panel->snapshot), "Blurred"));
    panel->snapshot.state.virtual_paused = true;
    assert(!strcmp(virtual_status(&panel->snapshot), "Paused"));
    panel->snapshot.state.recording = true;
    panel->snapshot.state.record_frozen = panel->snapshot.state.record_blurred = true;
    assert(!strcmp(record_status(&panel->snapshot), "Blurred"));
    panel->snapshot.state.record_paused = true;
    assert(!strcmp(record_status(&panel->snapshot), "Paused"));
    panel->snapshot.state.record_cut = true;
    assert(!strcmp(record_status(&panel->snapshot), "Cut"));
    panel->snapshot.countdown = true;
    assert(!strcmp(record_status(&panel->snapshot), "Countdown"));

    /* Native controls cancel the controller's actual countdown without starting
     * a recording; the independent virtual pause command does not cancel it. */
    app->config.record_countdown = 5;
    const char *start[] = {"record", "start"};
    assert(!route_command(panel->client, 2, start, error, sizeof error));
    assert(app->countdown && !app->state.recording);
    const char *pause_virtual[] = {"virtual", "pause"};
    assert(!route_command(panel->client, 2, pause_virtual, error, sizeof error));
    assert(app->countdown && app->state.virtual_paused);
    Widget cancel = {.enabled = true, .action = A_COMMAND, .argc = 2, .arg = {"record", "stop"}};
    activate(panel, &cancel);
    assert(!panel->error[0] && !app->countdown && !app->state.recording);
    assert(!app->countdown_path[0]);
    assert(!route_command(panel->client, 2, start, error, sizeof error));
    assert(app->countdown);
    Widget pause = {.enabled = true, .action = A_COMMAND, .argc = 1, .arg = {"pause"}};
    activate(panel, &pause);
    assert(!panel->error[0] && !app->countdown && !app->state.recording);
    assert(app->state.virtual_paused && !app->countdown_path[0]);

    /* Close is a local action, even with a command pending or disconnected. */
    State before_close = app->state;
    Config config_before_close = app->config;
    panel->snapshot.connected = false;
    panel->snapshot.command_queued = panel->snapshot.command_completed + 1;
    Widget close = {.id = 7, .enabled = true, .action = A_CLOSE};
    assert(!widget_in_scroll(panel, &close));
    activate(panel, &close);
    assert(panel->quit);
    assert(!memcmp(&before_close, &app->state, sizeof before_close));
    assert(!memcmp(&config_before_close, &app->config, sizeof config_before_close));

    media_close(app->media);
    platform_close(app->platform);
    compositor_destroy(app->compositor);
    frame_free(&app->neutral);
    free(panel);
    free(app);
    puts("panel edits: camera anchors/backdrops, preview acknowledgements, output mode labels, "
         "presentation settings, countdown cancellation and privacy pause passed");
    return 0;
}
