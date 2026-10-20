#include "daemon_bridge.h"
#include "../src/panel_transport.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DaemonBridge {
    PanelClient *client;
    Frame frame;
    int target;
    uint64_t generation, privacy_epoch, revision;
    bool pending;
    uint64_t edit_generation, edit_ack;
    char edit_error[256];
};

static void clear_frame(DaemonBridge *bridge)
{
    frame_free(&bridge->frame);
}

DaemonBridge *bridge_open(const Config *config, uint64_t window, char *error, size_t size)
{
    if (!config) {
        if (error && size) snprintf(error, size, "workspace configuration unavailable");
        return NULL;
    }
    DaemonBridge *bridge = calloc(1, sizeof *bridge);
    if (!bridge) {
        if (error && size) snprintf(error, size, "cannot allocate workspace connection");
        return NULL;
    }
    bridge->target = -1;
    bridge->client = panel_client_open(config, window, error, size);
    if (!bridge->client) {
        free(bridge);
        return NULL;
    }
    return bridge;
}

void bridge_close(DaemonBridge *bridge)
{
    if (!bridge) return;
    panel_client_close(bridge->client);
    clear_frame(bridge);
    free(bridge);
}

static int refuse_edit(DaemonBridge *bridge, char *error, size_t size, const char *message)
{
    if (bridge) snprintf(bridge->edit_error, sizeof bridge->edit_error, "%s", message);
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static bool listed(const char *value, const char *const *allowed, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(value, allowed[i])) return true;
    return false;
}

int bridge_edit(DaemonBridge *bridge, AppEdit edit, const char *value, char *error, size_t size)
{
    static const char *const layouts[] = {"overlay", "split", "screen", "camera", "stage"};
    static const char *const fits[] = {"contain", "cover"};
    static const char *const anchors[] = {"top-left", "top", "top-right", "left", "right",
                                         "bottom-left", "bottom", "bottom-right"};
    const char *key = NULL;
    bool valid = false;
    if (!bridge || !value || strlen(value) > 31)
        return refuse_edit(bridge, error, size, "Invalid composition edit");
    if (bridge->pending)
        return refuse_edit(bridge, error, size, "Wait for the pending composition edit");
    switch (edit) {
    case APP_EDIT_LAYOUT:
        key = "composition.layout";
        valid = listed(value, layouts, sizeof layouts / sizeof *layouts);
        break;
    case APP_EDIT_FIT:
        key = "composition.fit";
        valid = listed(value, fits, sizeof fits / sizeof *fits);
        break;
    case APP_EDIT_ANCHOR:
        key = "camera.anchor";
        valid = listed(value, anchors, sizeof anchors / sizeof *anchors);
        break;
    case APP_EDIT_SIZE: {
        key = "camera.width_percent";
        bool digit = false, dot = false;
        valid = true;
        for (const char *p = value; *p; ++p) {
            if (*p >= '0' && *p <= '9') digit = true;
            else if (*p == '.' && !dot) dot = true;
            else valid = false;
        }
        double number = strtod(value, NULL);
        valid = valid && digit && isfinite(number) && number >= 1 && number <= 100;
        break;
    }
    default: break;
    }
    if (!valid) return refuse_edit(bridge, error, size, "Unsupported composition value");
    PanelSnapshot snapshot;
    if (!panel_client_snapshot(bridge->client, &snapshot))
        return refuse_edit(bridge, error, size, "Daemon is busy; try the edit again");
    if (!snapshot.connected)
        return refuse_edit(bridge, error, size, "Daemon disconnected; edit was not queued");
    if (snapshot.command_queued != snapshot.command_completed || snapshot.command_queued == UINT64_MAX)
        return refuse_edit(bridge, error, size, "Daemon command queue is busy");
    char transport_error[CAST_ERR];
    if (panel_client_setting(bridge->client, key, value, transport_error, sizeof transport_error))
        return refuse_edit(bridge, error, size, "Composition edit could not be queued");
    bridge->edit_generation = snapshot.daemon_generation;
    bridge->edit_ack = snapshot.command_queued + 1;
    bridge->pending = true;
    bridge->edit_error[0] = 0;
    if (error && size) error[0] = 0;
    return 0;
}

static void edit_snapshot(DaemonBridge *bridge, const PanelSnapshot *snapshot, AppLive *out)
{
    if (snapshot && bridge->pending) {
        if (!snapshot->connected || snapshot->daemon_generation != bridge->edit_generation) {
            bridge->pending = false;
            snprintf(bridge->edit_error, sizeof bridge->edit_error,
                     "Daemon connection changed; edit canceled. Inspect the current composition.");
        } else if (snapshot->command_completed >= bridge->edit_ack) {
            bridge->pending = false;
            if (snapshot->command_failed)
                snprintf(bridge->edit_error, sizeof bridge->edit_error,
                         "Daemon rejected the composition edit. The current composition is unchanged.");
            else bridge->edit_error[0] = 0;
        }
    }
    out->pending = bridge->pending;
    snprintf(out->edit_error, sizeof out->edit_error, "%s", bridge->edit_error);
}

static void copy_config(AppLive *out, const PanelSnapshot *snapshot)
{
    const Config *config = &snapshot->config;
    const State *state = &snapshot->state;
    out->pro = snapshot->edition.identity.pro;
    out->generation = snapshot->daemon_generation;
    out->privacy_epoch = snapshot->privacy_epoch;
    out->width = config->width;
    out->height = config->height;
    out->fps = config->fps;
    snprintf(out->backend, sizeof out->backend, "%.*s", (int)sizeof config->backend, config->backend);
    snprintf(out->record_container, sizeof out->record_container, "%.*s", (int)sizeof config->record_container, config->record_container);
    snprintf(out->record_codec, sizeof out->record_codec, "%.*s", (int)sizeof config->video_codec, config->video_codec);
    snprintf(out->stream_service, sizeof out->stream_service, "%.*s", (int)sizeof config->stream.service, config->stream.service);
    out->camera_width_percent = (float)config->camera_width_percent;
    /* These are validated configuration enums, never endpoints or credentials. */
    snprintf(out->layout, sizeof out->layout, "%.*s", (int)sizeof config->layout, config->layout);
    snprintf(out->fit, sizeof out->fit, "%.*s", (int)sizeof config->fit, config->fit);
    snprintf(out->anchor, sizeof out->anchor, "%.*s", (int)sizeof config->anchor, config->anchor);
    out->virtual_enabled = config->virtual_enabled;
    out->virtual_paused = state->virtual_paused;
    out->virtual_frozen = state->virtual_frozen;
    out->virtual_blurred = state->virtual_blurred;
    out->recording = state->recording;
    out->record_paused = state->record_paused;
    out->record_frozen = state->record_frozen;
    out->record_blurred = state->record_blurred;
    out->stream_active = state->stream_active;
    out->stream_paused = state->stream_paused;
    out->stream_frozen = state->stream_frozen;
    out->stream_blurred = state->stream_blurred;
    out->mic = config->mic;
    out->desktop = config->desktop;
    out->camera_enabled = config->camera_enabled;
    out->camera_visible = config->camera_visible;
    out->mirror = config->mirror;
    /* Generic source labels intentionally avoid exposing paths and user text. */
    snprintf(out->screen_name, sizeof out->screen_name, "Screen capture");
    snprintf(out->camera_name, sizeof out->camera_name, "%s", config->camera_enabled ? "Camera" : "Camera disabled");
    snprintf(out->mic_name, sizeof out->mic_name, "%s", config->mic ? "Microphone" : "Microphone off");
    snprintf(out->desktop_name, sizeof out->desktop_name, "%s", config->desktop ? "Desktop audio" : "Desktop audio off");
    if (snapshot->composition_error[0])
        snprintf(out->error, sizeof out->error, "%s", snapshot->composition_error);
    else if (snapshot->error[0] || snapshot->stream.error[0])
        snprintf(out->error, sizeof out->error, "Daemon reported an error. Inspect cast status for details.");
}

void bridge_poll(DaemonBridge *bridge, int target, AppLive *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = true;
    out->target = target;
    snprintf(out->status, sizeof out->status, "Waiting for daemon");
    if (!bridge) {
        snprintf(out->error, sizeof out->error, "Workspace connection unavailable");
        return;
    }
    edit_snapshot(bridge, NULL, out);
    if (target < 0 || target > 2) {
        clear_frame(bridge);
        bridge->target = -1;
        snprintf(out->error, sizeof out->error, "Invalid preview target");
        return;
    }
    PanelSnapshot before, after;
    /* A missed snapshot cannot prove that retained pixels remain private. */
    bool sampled = panel_client_snapshot(bridge->client, &before);
    edit_snapshot(bridge, sampled ? &before : NULL, out);
    if (!sampled || !before.connected) {
        clear_frame(bridge);
        bridge->generation = 0;
        if (sampled && before.error[0]) {
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, before.error);
            snprintf(out->status, sizeof out->status, "Daemon not connected");
        }
        return;
    }
    if (target != bridge->target || before.daemon_generation != bridge->generation ||
        before.privacy_epoch != bridge->privacy_epoch) clear_frame(bridge);
    bridge->target = target;
    bridge->generation = before.daemon_generation;
    bridge->privacy_epoch = before.privacy_epoch;
    char error[CAST_ERR] = {0};
    int copied = panel_client_frame(bridge->client, PANEL_PREVIEW_COMPOSITION, &bridge->frame, error, sizeof error);
    bool observed = panel_client_snapshot(bridge->client, &after);
    edit_snapshot(bridge, observed ? &after : NULL, out);
    if (copied < 0 || !observed || !after.connected ||
        after.daemon_generation != before.daemon_generation ||
        after.privacy_epoch != before.privacy_epoch) {
        clear_frame(bridge);
        if (copied < 0 && error[0])
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, error);
        else if (observed && !after.connected && after.error[0])
            snprintf(out->error, sizeof out->error, "%.*s", (int)sizeof out->error-1, after.error);
        return;
    }
    copy_config(out, &after);
    out->connected = true;
    snprintf(out->status, sizeof out->status, "Connected");
    if (copied > 0) ++bridge->revision;
    if (bridge->frame.data) {
        out->has_frame = true;
        out->frame = (cui_bitmap){bridge->frame.data, (unsigned)bridge->frame.width,
            (unsigned)bridge->frame.height, (size_t)bridge->frame.stride, bridge->revision};
    }
}
