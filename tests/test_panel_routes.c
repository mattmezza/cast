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
int main(void)
{
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
    app->state.live_paused = true;
    app->defaults = app->config;
    char error[CAST_ERR];
    app->compositor = compositor_create();
    app->platform = platform_open(&app->config, error, sizeof error);
    app->media = media_open(&app->config, error, sizeof error);
    assert(app->compositor && app->platform && app->media);
    assert(!frame_alloc(&app->neutral, app->config.width, app->config.height));
    compositor_neutral(&app->config, &app->neutral);
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
    assert(!strcmp(app->config.shape, "circle") && !strcmp(app->config.aspect, "1:1"));
    assert(!app->config.mirror);
    edit_field(panel, app, "camera.anchor", "top-left");
    assert(!strcmp(app->config.anchor, "top-left"));
    edit_field(panel, app, "camera.anchor", "free");
    assert(!strcmp(app->config.anchor, "free"));

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

    /* Native controls cancel the controller's actual countdown without starting
     * a recording; the independent live pause command does not cancel it. */
    app->config.record_countdown = 5;
    const char *start[] = {"record", "start"};
    assert(!route_command(panel->client, 2, start, error, sizeof error));
    assert(app->countdown && !app->state.recording);
    const char *pause_live[] = {"live", "pause"};
    assert(!route_command(panel->client, 2, pause_live, error, sizeof error));
    assert(app->countdown && app->state.live_paused);
    Widget cancel = {.enabled = true, .action = A_COMMAND, .argc = 2, .arg = {"record", "stop"}};
    activate(panel, &cancel);
    assert(!panel->error[0] && !app->countdown && !app->state.recording);
    assert(!app->countdown_path[0]);
    assert(!route_command(panel->client, 2, start, error, sizeof error));
    assert(app->countdown);
    Widget pause = {.enabled = true, .action = A_COMMAND, .argc = 1, .arg = {"pause"}};
    activate(panel, &pause);
    assert(!panel->error[0] && !app->countdown && !app->state.recording);
    assert(app->state.live_paused && !app->countdown_path[0]);

    media_close(app->media);
    platform_close(app->platform);
    compositor_destroy(app->compositor);
    frame_free(&app->neutral);
    free(panel);
    free(app);
    puts("panel edits: centered resize, free position, camera commands, remembered zoom, advanced "
         "settings, countdown cancellation and privacy pause passed");
    return 0;
}
