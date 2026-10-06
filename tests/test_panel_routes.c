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

static unsigned route_calls;

static int route_command(PanelClient *client, int argc, const char *const *args, char *error,
                         size_t size)
{
    route_calls++;
    char *argv[CAST_MAX_ARGS];
    for (int i = 0; i < argc; i++) {
        argv[i] = (char *)args[i];
    }
    char response[CAST_ERR];
    int result;
    if (argc > 0 && !strcmp(args[0], "license")) {
        result = cast_edition_command(argc, argv, ((App *)client)->config.licensing_file, true,
                                      response, sizeof response, error, size);
    } else {
        result = app_command((App *)client, argc, argv, response, sizeof response);
    }
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
/* The mock controller executes synchronously; explicitly model its matching ack. */
static void route_acknowledge(Panel *panel, const App *app)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (panel->edit[i].pending) {
            acknowledge_edit(&panel->edit[i], false);
        }
    }
    panel->snapshot.command_completed = panel->snapshot.command_queued;
    panel->snapshot.config = app->config;
    panel->snapshot.state = app->state;
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
static void check_license_layout(void)
{
    /* An SDL software surface exercises real Clay layout/fonts without X11,
     * sockets, capture or a running daemon. Mutations use the explicit mock
     * controller above and a temporary private store. */
    assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_VIDEODRIVER", "dummy", true));
    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(TTF_Init());
    Panel *panel = calloc(1, sizeof *panel);
    App *app = calloc(1, sizeof *app);
    assert(panel && app);
    config_defaults(&app->config);
    app->state.virtual_paused = true;
    app->state.stream_paused = true;
    char directory[] = "/tmp/cast-license-layout-XXXXXX";
    assert(mkdtemp(directory));
    snprintf(app->config.licensing_file, sizeof app->config.licensing_file, "%s/license.json",
             directory);
    panel->snapshot.config = app->config;
    panel->snapshot.state = app->state;
    panel->snapshot.connected = true;
    snprintf(panel->snapshot.exclusion, sizeof panel->snapshot.exclusion,
             "Private synthetic fixture: capture exclusion is not required.");
    panel->client = (PanelClient *)app;
    panel->density = panel->input_scale = 1;
    panel->width = 520;
    panel->height = 760;
    panel->view = VIEW_OPERATE;
    panel->open_section = panel->open_lane = -1;
    panel->window =
        SDL_CreateWindow("private synthetic License layout", 520, 760, SDL_WINDOW_HIDDEN);
    assert(panel->window);
    SDL_Surface *surface_image = SDL_CreateSurface(520, 760, SDL_PIXELFORMAT_RGBA32);
    assert(surface_image);
    panel->renderer = SDL_CreateSoftwareRenderer(surface_image);
    assert(panel->renderer);
    assert(SDL_SetRenderDrawBlendMode(panel->renderer, SDL_BLENDMODE_BLEND));
    size_t font_length = (size_t)(cast_panel_font_end - cast_panel_font_data);
    for (unsigned i = 0; i < 5; i++) {
        SDL_IOStream *stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        panel->font[i] = TTF_OpenFontIO(stream, true, (float)font_sizes[i]);
        stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        panel->raster_font[i] = TTF_OpenFontIO(stream, true, (float)font_sizes[i]);
        assert(panel->font[i] && panel->raster_font[i]);
    }
    uint32_t memory_size = Clay_MinMemorySize();
    void *arena = malloc(memory_size);
    assert(arena);
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(memory_size, arena),
                    (Clay_Dimensions){520, 760},
                    (Clay_ErrorHandler){.errorHandlerFunction = clay_error, .userData = panel});
    Clay_SetMeasureTextFunction(measure, panel);
    char error[CAST_ERR];
    assert(!cast_edition_snapshot(app->config.licensing_file, 100, &panel->snapshot.edition, error,
                                  sizeof error));
    unsigned calls_before = route_calls;
    Widget open = {.id = 9200, .enabled = true, .action = A_LICENSE};
    activate(panel, &open);
    assert(panel->view == VIEW_LICENSE && route_calls == calls_before);
    for (unsigned width = 360; width <= 520; width += 160) {
        panel->width = (float)width;
        Clay_SetLayoutDimensions((Clay_Dimensions){panel->width, panel->height});
        Clay_RenderCommandArray commands = layout(panel);
        render(panel, commands);
        assert(SDL_FlushRenderer(panel->renderer));
        assert(!panel->error[0]);
        Widget *badge = find_widget(panel, 9200);
        assert(badge && badge->enabled && badge->box.x >= 0 &&
               badge->box.x + badge->box.width <= width + 1);
        assert(!widget_in_scroll(panel, badge));
        Widget *scratch = find_widget(panel, 1000 + (uint32_t)FIELD_COUNT * 3);
        assert(scratch && scratch->enabled && scratch->index == (int)FIELD_COUNT);
        assert(!find_widget(panel, 9201)->enabled && !find_widget(panel, 9207));
        for (int i = 0; i < panel->widget_count; i++) {
            const Widget *w = &panel->widgets[i];
            assert(w->box.x >= 0 && w->box.x + w->box.width <= width + 1);
        }
        const char *evidence = getenv("CAST_LICENSE_LAYOUT_EVIDENCE");
        if (evidence) {
            char path[4096];
            snprintf(path, sizeof path, "%s/license-%u.bmp", evidence, width);
            assert(SDL_SaveBMP(surface_image, path));
        }
    }
    panel->width = 520;
    Clay_SetLayoutDimensions((Clay_Dimensions){520, 760});
    layout(panel);
    set_focus(panel, find_widget(panel, 1000 + (uint32_t)FIELD_COUNT * 3));
    insert_text(panel, "/tmp/explicit-license-file.json");
    assert(!strcmp(panel->edit[FIELD_COUNT].value, "/tmp/explicit-license-file.json"));
    SDL_KeyboardEvent enter = {.key = SDLK_RETURN};
    key_event(panel, &enter);
    assert(route_calls == calls_before && !panel->active_text); /* Enter never imports. */
    layout(panel);
    assert(find_widget(panel, 9201)->enabled);
    activate(panel, find_widget(panel, 9206));
    char *clipboard = SDL_GetClipboardText();
    assert(clipboard && strstr(clipboard, "cinematic_zoom=") &&
           strstr(clipboard, "speech_teleprompter=") && !strstr(clipboard, "payload") &&
           !strstr(clipboard, "planned/"));
    SDL_free(clipboard);
    State state_before = app->state;
    Config config_before = app->config;
    activate(panel, find_widget(panel, 9203));
    assert(panel->license_remove_confirmation && route_calls == calls_before);
    layout(panel);
    activate(panel, find_widget(panel, 9204));
    assert(!panel->license_remove_confirmation && route_calls == calls_before);
    activate(panel, find_widget(panel, 9203));
    layout(panel);
    activate(panel, find_widget(panel, 9205));
    assert(route_calls == calls_before + 1);
    assert(!memcmp(&state_before, &app->state, sizeof state_before));
    assert(!memcmp(&config_before, &app->config, sizeof config_before));
    route_acknowledge(panel, app);
    layout(panel);
    SDL_KeyboardEvent escape = {.key = SDLK_ESCAPE};
    key_event(panel, &escape);
    assert(panel->view == VIEW_OPERATE);
    Widget setup = {.enabled = true, .action = A_SETUP, .index = 1};
    activate(panel, &setup);
    assert(panel->view == VIEW_SETUP);
    activate(panel, &open);
    assert(panel->view == VIEW_LICENSE);
    key_event(panel, &escape);
    assert(panel->view == VIEW_SETUP);
    key_event(panel, &escape);
    assert(panel->view == VIEW_OPERATE);
    char lock_path[4096];
    snprintf(lock_path, sizeof lock_path, "%s/.license.json.lock", directory);
    unlink(lock_path);
    snprintf(lock_path, sizeof lock_path, "%s/.license.json.session", directory);
    unlink(lock_path);
    assert(!rmdir(directory));
    /* Cleanup owns fonts/render surfaces; no worker client exists in this mock. */
    for (unsigned i = 0; i < TEXT_CACHE_MAX; i++) {
        SDL_DestroyTexture(panel->cache[i].texture);
        free(panel->cache[i].text);
    }
    for (unsigned i = 0; i < 5; i++) {
        TTF_CloseFont(panel->font[i]);
        TTF_CloseFont(panel->raster_font[i]);
    }
    SDL_DestroyRenderer(panel->renderer);
    SDL_DestroySurface(surface_image);
    SDL_DestroyWindow(panel->window);
    free(arena);
    free(panel);
    free(app);
    TTF_Quit();
    SDL_Quit();
    puts("SDL dummy License/About layout 360/520, keyboard scratch, safe clipboard, confirmed "
         "removal, privacy and navigation tests passed");
}

int main(void)
{
    initialize_fields();
    check_text_contrast();
    check_license_layout();
    /* A control has one home: connection fields never leak into Compose, and
     * the camera size draft cannot be stranded behind a duplicate field slot. */
    assert(sizeof section_names / sizeof *section_names == 6);
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        assert(field_section(&fields[i]) >= 0 && field_section(&fields[i]) <= 10);
        for (size_t j = i + 1; j < FIELD_COUNT; j++) {
            assert(strcmp(fields[i].key, fields[j].key));
        }
    }
    assert(field_section(&fields[find_field("camera.width_percent")]) == 1);
    assert(field_section(&fields[find_field("audio.mic_source")]) == 6);
    assert(field_section(&fields[find_field("stream.key_file")]) == 7);
    assert(field_section(&fields[find_field("record.countdown")]) == 8);
    assert(field_section(&fields[find_field("licensing.file")]) == 9);
    assert(find_field("record.rate_control") >= 0);
    assert(find_field("record.bitrate_kbps") >= 0);
    assert(find_field("stream.video_encoder") >= 0);
    assert(!license_url_valid("http://example.invalid"));
    assert(!license_url_valid("https://example.invalid\n--command"));
    assert(license_url_valid("https://example.invalid/info"));
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

    route_acknowledge(panel, app);
    /* Immediate controls go through the actual activate/settings route exactly once. */
    const char *ordinary[] = {"camera.anchor",
                              "camera.shape",
                              "camera.mirror",
                              "composition.layout",
                              "annotations.virtual_clicks",
                              "camera.radius",
                              "record.countdown",
                              "output.pause_text",
                              "screen.background_color"};
    const char *batched[] = {"stream.server_url",
                             "capture.x",
                             "capture.width",
                             "zoom.min",
                             "zoom.max",
                             "composition.layout_order",
                             "composition.preset_order",
                             "camera.corner_order",
                             "record.container",
                             "record.video_codec",
                             "record.audio_codec",
                             "record.preset"};
    for (size_t i = 0; i < sizeof ordinary / sizeof *ordinary; i++) {
        assert(field_applies_immediately(&fields[find_field(ordinary[i])]));
    }
    for (size_t i = 0; i < sizeof batched / sizeof *batched; i++) {
        assert(!field_applies_immediately(&fields[find_field(batched[i])]));
    }
    unsigned calls = route_calls;
    int anchor_index = find_field("camera.anchor");
    Widget anchor = {.id = 8400,
                     .type = W_ANCHOR,
                     .action = A_SETTING,
                     .enabled = true,
                     .draft = true,
                     .index = anchor_index};
    snprintf(anchor.value, sizeof anchor.value, "top-left");
    activate(panel, &anchor);
    assert(route_calls == calls + 1 && !strcmp(app->config.anchor, "top-left"));
    assert(panel->edit[anchor_index].pending);
    choose_field(panel, anchor_index, "top-right");
    assert(route_calls == calls + 1 && !strcmp(app->config.anchor, "top-left"));
    assert(panel->edit[anchor_index].auto_apply);
    route_acknowledge(panel, app);
    assert(panel->edit[anchor_index].dirty); /* Older ack cannot discard the next choice. */
    commit_field(panel, anchor_index);
    assert(route_calls == calls + 2 && !strcmp(app->config.anchor, "top-right"));
    route_acknowledge(panel, app);
    assert(!panel->edit[anchor_index].dirty);

    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (strstr(fields[i].label, "(px)")) {
            assert(field_is_pixels(&fields[i]));
        }
        assert(flow_slider_key(fields[i].key) == field_is_numeric(&fields[i]));
        if (field_is_numeric(&fields[i])) {
            double minimum, maximum;
            flow_slider_range(panel, i, &minimum, &maximum);
            assert(isfinite(minimum) && isfinite(maximum) && minimum < maximum);
            assert(minimum >= fields[i].minimum && maximum <= fields[i].maximum);
            assert(flow_slider_step(&fields[i]) > 0);
        }
    }
    assert(field_is_pixels(&fields[find_field("capture.width")]));
    assert(field_is_pixels(&fields[find_field("camera.crop_x")]));
    assert(field_is_pixels(&fields[find_field("output.width")]));
    assert(!field_is_pixels(&fields[find_field("camera.width_percent")]));
    int radius_index = find_field("camera.radius");
    Widget slider = {.id = 1000 + (uint32_t)radius_index * 3,
                     .type = W_SLIDER,
                     .action = A_FIELD,
                     .index = radius_index,
                     .enabled = true,
                     .draft = true,
                     .box = {0, 0, 200, 30}};
    field_value(&fields[radius_index], &app->config, panel->edit[radius_index].value,
                sizeof panel->edit[radius_index].value);
    calls = route_calls;
    int radius_before = app->config.radius;
    flow_slider_update(panel, &slider, 150);
    assert(route_calls == calls && app->config.radius == radius_before);
    int selected_radius = atoi(panel->edit[radius_index].value);
    flow_slider_commit(panel, &slider);
    assert(route_calls == calls + 1 && app->config.radius == selected_radius);
    route_acknowledge(panel, app);
    /* Losing focus ends a held drag just like release, without dropping its value. */
    Widget previous_widget = panel->widgets[0];
    int previous_widget_count = panel->widget_count;
    panel->widget_count = 1;
    panel->widgets[0] = slider;
    panel->active_text = 0;
    panel->slider_drag = slider.id;
    panel->mouse_down = true;
    calls = route_calls;
    flow_slider_update(panel, &panel->widgets[0], 110);
    selected_radius = atoi(panel->edit[radius_index].value);
    assert(route_calls == calls && panel->edit[radius_index].dirty);
    SDL_Event lost_focus = {.type = SDL_EVENT_WINDOW_FOCUS_LOST};
    event(panel, &lost_focus);
    assert(route_calls == calls + 1 && app->config.radius == selected_radius);
    assert(!panel->slider_drag && !panel->mouse_down);
    route_acknowledge(panel, app);
    int region_index = find_field("capture.width");
    bool region_available = panel->snapshot.capabilities.region_selection;
    panel->snapshot.capabilities.region_selection = true;
    panel->widgets[0].index = region_index;
    panel->widgets[0].id = 1000 + (uint32_t)region_index * 3;
    panel->slider_drag = panel->widgets[0].id;
    panel->mouse_down = true;
    calls = route_calls;
    int region_before = app->config.region_w;
    flow_slider_update(panel, &panel->widgets[0], 110);
    event(panel, &lost_focus);
    assert(route_calls == calls && app->config.region_w == region_before);
    assert(panel->edit[region_index].dirty && !panel->slider_drag && !panel->mouse_down);
    field_value(&fields[region_index], &app->config, panel->edit[region_index].value,
                sizeof panel->edit[region_index].value);
    panel->edit[region_index].dirty = false;
    panel->snapshot.capabilities.region_selection = region_available;
    panel->widgets[0] = previous_widget;
    panel->widget_count = previous_widget_count;
    calls = route_calls;
    stage_field(panel, radius_index, "not-a-number");
    commit_field(panel, radius_index);
    assert(route_calls == calls && app->config.radius == selected_radius);
    assert(panel->error[0] && panel->edit[radius_index].dirty);
    stage_field(panel, radius_index, "37");
    commit_field(panel, radius_index);
    assert(route_calls == calls + 1 && app->config.radius == 37);
    route_acknowledge(panel, app);

    /* Ratios drag in useful increments; precise input preserves extra decimals
     * and submits once on Enter. Invalid ratios never reach the controller. */
    int opacity_index = find_field("logo.opacity");
    slider.id = 1000 + (uint32_t)opacity_index * 3;
    slider.index = opacity_index;
    calls = route_calls;
    flow_slider_update(panel, &slider, 77);
    assert(route_calls == calls);
    double opacity = strtod(panel->edit[opacity_index].value, NULL);
    assert(opacity > 0 && opacity < 1);
    assert(fabs(opacity * 100 - round(opacity * 100)) < 1e-9);
    flow_slider_commit(panel, &slider);
    assert(route_calls == calls + 1 && app->config.logo_opacity == opacity);
    route_acknowledge(panel, app);
    panel->widget_count = 1;
    panel->widgets[0] = (Widget){.id = slider.id + 2,
                                 .type = W_FIELD,
                                 .action = A_FIELD,
                                 .enabled = true,
                                 .index = opacity_index};
    panel->focus = panel->active_text = slider.id + 2;
    panel->select_all = true;
    insert_text(panel, "0.375");
    SDL_KeyboardEvent numeric_enter = {.key = SDLK_RETURN};
    key_event(panel, &numeric_enter);
    assert(route_calls == calls + 2 && app->config.logo_opacity == .375);
    assert(!panel->active_text && !panel->edit[opacity_index].auto_apply);
    route_acknowledge(panel, app);
    panel->focus = panel->active_text = slider.id + 2;
    panel->select_all = true;
    insert_text(panel, "1.1");
    key_event(panel, &numeric_enter);
    assert(route_calls == calls + 2 && app->config.logo_opacity == .375);
    assert(panel->error[0] && panel->edit[opacity_index].dirty);
    stage_field(panel, opacity_index, "0.375");
    commit_field(panel, opacity_index);
    route_acknowledge(panel, app);
    panel->error[0] = 0;

    /* A practical drag span must never reduce the precise input's valid range. */
    int step_index = find_field("zoom.step");
    double step_minimum, step_maximum;
    flow_slider_range(panel, (size_t)step_index, &step_minimum, &step_maximum);
    assert(step_maximum < 3);
    calls = route_calls;
    stage_field(panel, step_index, "3");
    commit_field(panel, step_index);
    assert(route_calls == calls + 1 && app->config.zoom_step == 3);
    route_acknowledge(panel, app);
    int fps_index = find_field("output.fps");
    calls = route_calls;
    assert(!flow_enabled(panel, (size_t)fps_index));
    stage_field(panel, fps_index, "90");
    commit_field(panel, fps_index);
    assert(route_calls == calls && !panel->edit[fps_index].dirty);

    assert(!strcmp(flow_slider_unit(&fields[find_field("record.countdown")]), "s"));
    assert(!strcmp(flow_slider_unit(&fields[find_field("keys.timeout_ms")]), "ms"));
    assert(!strcmp(flow_slider_unit(&fields[find_field("composition.split_ratio")]), "%"));
    assert(!strcmp(flow_slider_unit(&fields[find_field("stream.video_bitrate_kbps")]), "kbps"));
    assert(!strcmp(flow_slider_unit(&fields[find_field("output.fps")]), "fps"));
    assert(flow_slider_step(&fields[find_field("logo.opacity")]) == .01);
    assert(flow_slider_step(&fields[find_field("zoom.smoothing")]) == .001);
    assert(flow_slider_step(&fields[find_field("keys.timeout_ms")]) == 10);

    int color_index = find_field("screen.background_color");
    Widget swatch = {.id = 6006,
                     .type = W_BUTTON,
                     .action = A_SWATCH,
                     .enabled = true,
                     .draft = true,
                     .index = color_index};
    snprintf(swatch.value, sizeof swatch.value, "#bbc1ca");
    calls = route_calls;
    activate(panel, &swatch);
    assert(route_calls == calls + 1 && app->config.screen_background_color == 0xbbc1ca);
    route_acknowledge(panel, app);
    calls = route_calls;
    stage_field(panel, color_index, "#zzzzzz");
    commit_field(panel, color_index);
    assert(route_calls == calls && app->config.screen_background_color == 0xbbc1ca);
    assert(panel->error[0] && panel->edit[color_index].dirty);
    stage_field(panel, color_index, "#315c87");
    commit_field(panel, color_index);
    assert(route_calls == calls + 1 && app->config.screen_background_color == 0x315c87);
    route_acknowledge(panel, app);

    /* Coupled ranges stage locally, apply atomically, and retain their page/scroll. */
    int minimum_index = find_field("zoom.min"), maximum_index = find_field("zoom.max");
    double old_minimum = app->config.zoom_min, old_maximum = app->config.zoom_max;
    calls = route_calls;
    choose_field(panel, minimum_index, "2");
    choose_field(panel, maximum_index, "5");
    assert(route_calls == calls && app->config.zoom_min == old_minimum &&
           app->config.zoom_max == old_maximum);
    panel->view = VIEW_SECTION;
    panel->open_section = 0;
    panel->view_scroll[2] = -180;
    apply_section(panel, 0);
    assert(route_calls == calls + 1 && !panel->error[0]);
    assert(panel->view == VIEW_SECTION && panel->open_section == 0 &&
           panel->view_scroll[2] == -180);
    assert(app->config.zoom_min == 2 && app->config.zoom_max == 5);
    route_acknowledge(panel, app);
    assert(!section_dirty(panel, 0));
    stage_field(panel, minimum_index, "2.5");
    Widget revert = {.enabled = true, .action = A_REVERT, .index = 0};
    activate(panel, &revert);
    assert(!section_dirty(panel, 0) && app->config.zoom_min == 2);
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
