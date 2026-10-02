#include "app_internal.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int app_error(char *e, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e, n, fmt, ap);
    va_end(ap);
    return -1;
}
int app_copy_string(char *dst, size_t n, const char *src, char *e, size_t en)
{
    if (strlen(src) >= n) {
        return app_error(e, en, "value is too long");
    }
    strcpy(dst, src);
    return 0;
}
static bool oneof(const char *s, const char *choices)
{
    size_t n = strlen(s);
    while (choices && *choices) {
        const char *p = strchr(choices, ',');
        size_t len = p ? (size_t)(p - choices) : strlen(choices);
        if (len == n && !strncmp(s, choices, n)) {
            return true;
        }
        choices = p ? p + 1 : NULL;
    }
    return false;
}
static int number(const char *s, double min, double max, double *v, bool percent, char *e, size_t n)
{
    char *end;
    errno = 0;
    double d = strtod(s, &end);
    if (errno || end == s || (!percent && *end) || (percent && strcmp(end, "%")) || !isfinite(d) ||
        d < min || d > max) {
        return app_error(e, n, "invalid %s: expected %g..%g%s", s, min, max, percent ? "%" : "");
    }
    *v = d;
    return 0;
}
int app_integer(const char *s, int min, int max, int *v, char *e, size_t n)
{
    double d;
    if (number(s, min, max, &d, false, e, n) || floor(d) != d) {
        return app_error(e, n, "%s must be an integer %d..%d", s, min, max);
    }
    *v = (int)d;
    return 0;
}
static int toggle(bool *v, const char *s, char *e, size_t n)
{
    if (!strcmp(s, "on")) {
        *v = true;
    } else if (!strcmp(s, "off")) {
        *v = false;
    } else if (!strcmp(s, "toggle")) {
        *v = !*v;
    } else {
        return app_error(e, n, "expected on|off|toggle");
    }
    return 0;
}
static void cycle(char *value, size_t n, const char *list)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL, *first = strtok_r(buf, ",", &save), *p = first;
    bool found = false;
    while (p) {
        if (found) {
            snprintf(value, n, "%s", p);
            return;
        }
        if (!strcmp(value, p)) {
            found = true;
        }
        p = strtok_r(NULL, ",", &save);
    }
    if (first) {
        snprintf(value, n, "%s", first);
    }
}

static size_t utf8_size(const unsigned char *s)
{
    size_t size = s[0] >= 0xc2 && s[0] <= 0xdf   ? 2
                  : s[0] >= 0xe0 && s[0] <= 0xef ? 3
                  : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4
                                                 : 0;
    for (size_t i = 1; i < size; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) {
        return 0;
    }
    return size;
}
static void json_string(char *dst, size_t n, const char *s)
{
    size_t pos = 0;
    if (n < 3) {
        return;
    }
    dst[pos++] = '"';
    for (; *s && pos + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c >= 128) {
            size_t count = utf8_size((const unsigned char *)s);
            if (count) {
                memcpy(dst + pos, s, count);
                pos += count;
                s += count - 1;
            } else {
                pos += (size_t)snprintf(dst + pos, n - pos, "\\u%04x", c);
            }
        } else if (c < 32) {
            int used = snprintf(dst + pos, n - pos, "\\u%04x", c);
            pos += (size_t)used;
        } else {
            dst[pos++] = (char)c;
        }
    }
    dst[pos++] = '"';
    dst[pos] = 0;
}
static int status(App *a, bool json, char *out, size_t n)
{
    State *s = &a->state;
    Config *c = &a->config;
    Capabilities cap = platform_capabilities(a->platform);
    char path[PATH_MAX * 6 + 3], err[CAST_ERR * 6 + 3];
    char source[sizeof c->monitor * 6 + 3], audio[4096], backend[128];
    char mic_source[sizeof c->mic_source * 6 + 3];
    char desktop_source[sizeof c->desktop_source * 6 + 3];
    char virtual_name[sizeof c->virtual_name * 6 + 3];
    char pause_text[sizeof c->pause_text * 6 + 3];
    int length;
    json_string(path, sizeof path, s->record_path);
    json_string(err, sizeof err, s->last_error);
    json_string(source, sizeof source, c->monitor);
    json_string(backend, sizeof backend, c->backend);
    json_string(mic_source, sizeof mic_source, c->mic_source);
    json_string(desktop_source, sizeof desktop_source, c->desktop_source);
    json_string(virtual_name, sizeof virtual_name, c->virtual_name);
    json_string(pause_text, sizeof pause_text, c->pause_text);
    media_audio_status(a->media, audio, sizeof audio);
    double seconds = media_record_duration(a->media) / 1e9;
    if (json) {
        length = snprintf(
            out, n,
            "{\"version\":\"%s\",\"backend\":%s,\"capabilities\":{\"capture\":%s,\"cursor_"
            "metadata\":%s,\"embedded_cursor\":%s,\"input\":%s,\"region_selection\":%s,"
            "\"window_selection\":%s,\"preview\":%s},\"source\":{\"monitor\":%s,\"kind\":\"%"
            "s\",\"region\":[%d,%d,%d,%d]},\"live\":{\"enabled\":%s,\"state\":\"%s\",\"message\":%"
            "s},"
            "\"record\":{\"state\":\"%s\",\"path\":%s,\"duration\":%.3f,\"countdown\":%s,"
            "\"finalizing\":%s},"
            "\"group_paused\":%s,\"layout\":\"%s\",\"zoom\":%.3f,\"camera_visible\":%s,"
            "\"audio\":{\"mic\":{\"enabled\":%s,\"source\":%s,\"gain\":%.3f},"
            "\"desktop\":{\"enabled\":%s,\"source\":%s,\"gain\":%.3f},"
            "\"virtual\":{\"enabled\":%s,\"name\":%s},\"readiness\":%s},"
            "\"last_error\":%s,\"dropped_frames\":%llu}",
            CAST_VERSION, backend, cap.capture ? "true" : "false",
            cap.cursor_metadata ? "true" : "false", cap.embedded_cursor ? "true" : "false",
            cap.input ? "true" : "false", cap.region_selection ? "true" : "false",
            cap.window_selection ? "true" : "false", cap.preview ? "true" : "false", source,
            c->capture_kind, c->region_x, c->region_y, c->region_w, c->region_h,
            c->live_enabled ? "true" : "false",
            s->live_paused   ? "paused"
            : s->live_frozen ? "frozen"
                             : "live",
            pause_text, s->recording ? (s->record_paused ? "paused" : "recording") : "stopped",
            path, seconds, a->countdown ? "true" : "false",
            media_record_finalizing(a->media) ? "true" : "false",
            s->group_paused ? "true" : "false", c->layout, c->zoom_factor,
            c->camera_visible ? "true" : "false", c->mic ? "true" : "false", mic_source,
            c->mic_gain, c->desktop ? "true" : "false", desktop_source, c->desktop_gain,
            c->virtual_audio ? "true" : "false", virtual_name, audio[0] ? audio : "{}", err,
            (unsigned long long)s->dropped_frames);
    } else {
        length =
            snprintf(out, n,
                     "backend=%s source=%s:%s layout=%s camera=%s zoom=%.2f\nlive=%s recording=%s "
                     "duration=%.3fs path=%s "
                     "countdown=%s\nlive_message=%s\naudio=%s\ndropped_frames=%llu error=%s",
                     c->backend, c->capture_kind, c->monitor[0] ? c->monitor : "selected",
                     c->layout, c->camera_visible ? "visible" : "hidden", c->zoom_factor,
                     s->live_paused   ? "PAUSED"
                     : s->live_frozen ? "FROZEN"
                                      : "LIVE",
                     s->recording ? (s->record_paused ? "RECORDING-PAUSED" : "RECORDING")
                     : media_record_finalizing(a->media) ? "finalizing"
                                                         : "stopped",
                     seconds, s->record_path, a->countdown ? "pending" : "off", c->pause_text,
                     audio, (unsigned long long)s->dropped_frames, s->last_error);
    }
    if (length < 0 || (size_t)length >= n) {
        return app_error(out, n, "status exceeds IPC limit; use shorter paths/source names");
    }
    return 0;
}
static int barrier(App *a, State *candidate, char *e, size_t n)
{
    int result = 0;
    bool record_transition =
        a->state.recording && candidate->record_paused != a->state.record_paused;
    bool was_frozen = a->state.live_frozen;
    a->state = *candidate;
    platform_events(a->platform, a->compositor, &a->config, true);
    compositor_clear(a->compositor);
    if (candidate->live_paused) {
        frame_free(&a->live);
        frame_free(&a->frozen);
        if (frame_copy(&a->frozen, &a->neutral)) {
            result = app_error(e, n,
                               "cannot allocate neutral freeze frame; live remains privacy-paused");
        }
        if (a->config.live_enabled && media_live(a->media, &a->neutral, true, e, n)) {
            result = -1;
        }
    } else if (candidate->live_frozen && !was_frozen) {
        if (frame_copy(&a->frozen, a->live.data ? &a->live : &a->neutral)) {
            result = app_error(e, n, "cannot allocate freeze frame");
        }
    }
    if (candidate->record_paused) {
        frame_free(&a->record);
    }
    panel_transport_barrier(a->panel, a, false);
    /* Neutral video is published before recorder barriers wait for in-flight work.
       media_privacy gates live audio before applying the recording transition. */
    media_privacy(a->media, candidate->live_paused, candidate->live_frozen,
                  candidate->record_paused);
    /* Surface a raced encoder failure even though the privacy gate has already applied. */
    if (record_transition && media_record_pause(a->media, candidate->record_paused, e, n)) {
        result = -1;
    }
    if (platform_capabilities(a->platform).preview) {
        const Frame *target = &a->neutral;
        if (!strcmp(a->config.preview_target, "record")) {
            if (candidate->recording && !candidate->record_paused && a->record.data) {
                target = &a->record;
            }
        } else if (!candidate->live_paused) {
            if (candidate->live_frozen && a->frozen.data) {
                target = &a->frozen;
            } else if (a->live.data) {
                target = &a->live;
            }
        }
        if (platform_preview(a->platform, target, candidate, &a->config, e, n)) {
            result = -1;
        }
    }
    return result;
}
int app_recording_start(App *a, const char *path, char *e, size_t n)
{
    if (media_record_start(a->media, &a->config, path, e, n)) {
        return -1;
    }
    a->state.recording = true;
    a->state.record_paused = false;
    a->state.group_record_restore = false;
    a->state.record_started_ns = cast_now_ns();
    media_record_path(a->media, a->state.record_path, sizeof a->state.record_path);
    if (a->state.group_paused) {
        if (media_record_pause(a->media, true, e, n)) {
            media_record_stop(a->media, e, n);
            a->state.recording = false;
            return -1;
        }
        a->state.record_paused = true;
    }
    media_privacy(a->media, a->state.live_paused, a->state.live_frozen, a->state.record_paused);
    return 0;
}
static int restart_reasons(const Config *old, const Config *c, bool recording, char *e, size_t n)
{
    e[0] = 0;
#define RESTART(condition, name)                                                                   \
    do {                                                                                           \
        if (condition) {                                                                           \
            size_t len = strlen(e);                                                                \
            snprintf(e + len, n - len, "%s%s", len ? ", " : "restart required: ", name);           \
        }                                                                                          \
    } while (0)
    RESTART(strcmp(old->backend, c->backend), "backend");
    RESTART(strcmp(old->socket_path, c->socket_path), "socket");
    RESTART(strcmp(old->output_device, c->output_device), "output device");
    RESTART(old->width != c->width || old->height != c->height || old->fps != c->fps,
            "output dimensions/fps");
    RESTART(old->live_enabled != c->live_enabled, "output enabled");
    RESTART(strcmp(old->virtual_name, c->virtual_name), "virtual microphone name");
    if (recording) {
        RESTART(
            strcmp(old->video_codec, c->video_codec) || strcmp(old->audio_codec, c->audio_codec) ||
                strcmp(old->record_container, c->record_container) ||
                old->record_crf != c->record_crf || strcmp(old->record_preset, c->record_preset) ||
                old->record_queue != c->record_queue,
            "recording encoder/container/queue while recording");
    }
#undef RESTART
    return e[0] ? -1 : 0;
}
static int apply_candidate(App *a, Config *c, char *e, size_t n)
{
    if (config_validate(c, e, n)) {
        return -1;
    }
    Capabilities cap = platform_capabilities(a->platform);
    if ((c->keys || c->clicks) && !cap.input) {
        return app_error(e, n, "backend does not support global keys/click observation");
    }
    if (c->zoom_follow && !cap.cursor_metadata) {
        return app_error(e, n,
                         "backend has no separate cursor metadata; zoom follow is unsupported");
    }
    if ((c->cursor != a->config.cursor && !cap.cursor_metadata) ||
        (c->cursor_highlight != a->config.cursor_highlight && !cap.cursor_metadata)) {
        return app_error(e, n, "backend does not support changing cursor visibility/highlight");
    }
    if (c->preview && !cap.preview) {
        return app_error(e, n, "backend preview unsupported");
    }
    bool recording = a->state.recording || media_record_finalizing(a->media);
    if (restart_reasons(&a->config, c, recording, e, n)) {
        return -1;
    }
    /* Stage fallible platform acquisition first. Roll back if media acquisition fails. */
    if (platform_reconfigure(a->platform, c, e, n)) {
        return -1;
    }
    if (media_reconfigure(a->media, c, recording, e, n)) {
        char rollback[CAST_ERR];
        if (platform_reconfigure(a->platform, &a->config, rollback, sizeof rollback)) {
            snprintf(a->state.last_error, sizeof a->state.last_error, "reload rollback: %.900s",
                     rollback);
        } else {
            /* A successfully rolled-back source must retain its prior zoom/annotations. */
            a->source_generation = platform_source_generation(a->platform);
        }
        return -1;
    }
    a->config = *c;
    app_sync_source(a);
    compositor_clear(a->compositor);
    compositor_neutral(c, &a->neutral);
    panel_transport_barrier(a->panel, a, false);
    return 0;
}

static int command_message(App *a, const char *value, char *out, size_t n)
{
    if (strlen(value) >= sizeof a->config.pause_text) {
        return app_error(out, n, "paused message must be shorter than %zu bytes",
                         sizeof a->config.pause_text);
    }
    for (const unsigned char *p = (const unsigned char *)value; *p;) {
        if (*p < 128) {
            p++;
        } else {
            size_t count = utf8_size(p);
            if (!count) {
                return app_error(out, n, "paused message must be valid UTF-8");
            }
            p += count;
        }
    }
    strcpy(a->config.pause_text, value);
    compositor_neutral(&a->config, &a->neutral);
    int result = 0;
    if (a->state.live_paused && a->config.live_enabled &&
        media_live(a->media, &a->neutral, true, out, n)) {
        result = -1;
    }
    /* An active freeze keeps its original pixels; changing the label never changes state. */
    if (platform_capabilities(a->platform).preview &&
        ((!strcmp(a->config.preview_target, "live") && a->state.live_paused) ||
         (!strcmp(a->config.preview_target, "record") &&
          (!a->state.recording || a->state.record_paused))) &&
        platform_preview(a->platform, &a->neutral, &a->state, &a->config, out, n)) {
        result = -1;
    }
    panel_transport_barrier(a->panel, a, false);
    if (!result) {
        snprintf(out, n, "paused message updated for this session");
    }
    return result;
}

#define IS(i, text) (ac > (i) && !strcmp(av[i], text))
#define ARITY(count)                                                                               \
    do {                                                                                           \
        if (ac != (count))                                                                         \
            return app_error(out, n, "invalid arguments; cast %s --help", av[0]);                  \
    } while (0)
#define ENUM(dst, value, choices)                                                                  \
    do {                                                                                           \
        if (!oneof(value, choices))                                                                \
            return app_error(out, n, "expected %s", choices);                                      \
        snprintf(dst, sizeof dst, "%s", value);                                                    \
    } while (0)

static int command_camera(App *a, Config *candidate, int ac, char **av, char *out, size_t n)
{
    Config c = *candidate;
    char e[CAST_ERR];

    if (ac < 2) {
        return app_error(out, n, "camera subcommand required");
    }
    if (IS(1, "list")) {
        ARITY(2);
        media_camera_list(out, n);
        return 0;
    }
    if (IS(1, "show") || IS(1, "hide") || IS(1, "toggle")) {
        ARITY(2);
        c.camera_visible = IS(1, "show") ? true : IS(1, "hide") ? false : !c.camera_visible;
    } else if (IS(1, "size")) {
        ARITY(3);
        double value;
        if (number(av[2], -100, 100, &value, true, out, n)) {
            return -1;
        }
        if (av[2][0] == '+' || av[2][0] == '-') {
            value += c.camera_width_percent;
        }
        if (value < 1 || value > 100) {
            return app_error(out, n, "camera size must be 1..100 percentage points");
        }
        int x, y, w, h;
        if (compositor_geometry(&c, a->camera.width ? a->camera.width : 640,
                                a->camera.height ? a->camera.height : 480, &x, &y, &w, &h, e,
                                sizeof e)) {
            return app_error(out, n, "%s", e);
        }
        c.camera_width_percent = value;
        if (!strcmp(c.anchor, "free")) {
            int nx, ny, nw, nh;
            if (compositor_geometry(&c, a->camera.width ? a->camera.width : 640,
                                    a->camera.height ? a->camera.height : 480, &nx, &ny, &nw, &nh,
                                    out, n)) {
                return -1;
            }
            c.camera_x = x + (w - nw) / 2;
            c.camera_y = y + (h - nh) / 2;
        }
    } else if (IS(1, "move") || IS(1, "position")) {
        ARITY(4);
        int x, y;
        if (app_integer(av[2], -7680, 7680, &x, out, n) ||
            app_integer(av[3], -4320, 4320, &y, out, n)) {
            return -1;
        }
        if (IS(1, "move")) {
            int w, h, cx, cy;
            if (compositor_geometry(&c, a->camera.width ? a->camera.width : 640,
                                    a->camera.height ? a->camera.height : 480, &cx, &cy, &w, &h,
                                    out, n)) {
                return -1;
            }
            x += cx;
            y += cy;
        }
        strcpy(c.anchor, "free");
        c.camera_x = x;
        c.camera_y = y;
    } else if (IS(1, "anchor")) {
        ARITY(3);
        if (IS(2, "next")) {
            cycle(c.anchor, sizeof c.anchor, c.corner_order);
        } else {
            ENUM(c.anchor, av[2], "top-left,top-right,bottom-left,bottom-right");
        }
    } else if (IS(1, "shape")) {
        ARITY(3);
        if (IS(2, "next")) {
            cycle(c.shape, sizeof c.shape, "rectangle,rounded,circle");
        } else {
            ENUM(c.shape, av[2], "rectangle,rounded,circle");
        }
    } else if (IS(1, "aspect")) {
        ARITY(3);
        ENUM(c.aspect, av[2], "native,16:9,4:3,1:1");
    } else if (IS(1, "crop")) {
        ARITY(5);
        if (!IS(2, "move")) {
            return app_error(out, n, "camera crop move DX DY");
        }
        int x, y;
        if (app_integer(av[3], -16384, 16384, &x, out, n) ||
            app_integer(av[4], -16384, 16384, &y, out, n)) {
            return -1;
        }
        if (abs(c.crop_x + x) > 16384 || abs(c.crop_y + y) > 16384) {
            return app_error(out, n, "camera crop offset exceeds range");
        }
        c.crop_x += x;
        c.crop_y += y;
    } else if (IS(1, "mirror")) {
        ARITY(3);
        if (toggle(&c.mirror, av[2], out, n)) {
            return -1;
        }
    } else if (IS(1, "device")) {
        ARITY(3);
        if (app_copy_string(c.camera_device, sizeof c.camera_device, av[2], out, n)) {
            return -1;
        }
        c.camera_enabled = true;
    } else {
        return app_error(out, n, "unknown camera command");
    }
    int x, y, w, h;
    if (compositor_geometry(&c, a->camera.width ? a->camera.width : 640,
                            a->camera.height ? a->camera.height : 480, &x, &y, &w, &h, out, n)) {
        return -1;
    }
    if (!strcmp(c.anchor, "free")) {
        c.camera_x = x;
        c.camera_y = y;
    }

    *candidate = c;
    return 0;
}

static int command_zoom(App *a, Config *candidate, int ac, char **av, char *out, size_t n)
{
    Config c = *candidate;
    Capabilities cap = platform_capabilities(a->platform);

    if (ac < 2) {
        return app_error(out, n, "zoom subcommand required");
    }
    if (IS(1, "set")) {
        ARITY(3);
        if (number(av[2], c.zoom_min, c.zoom_max, &c.zoom_factor, false, out, n)) {
            return -1;
        }
    } else if (IS(1, "follow")) {
        ARITY(3);
        if (!IS(2, "on") && !IS(2, "off")) {
            return app_error(out, n, "zoom follow on|off");
        }
        if (IS(2, "on") && !cap.cursor_metadata) {
            return app_error(out, n, "cursor-follow unsupported without cursor metadata");
        }
        c.zoom_follow = IS(2, "on");
    } else {
        ARITY(2);
        if (IS(1, "reset")) {
            c.zoom_factor = 1;
        } else if (IS(1, "toggle")) {
            c.zoom_factor = c.zoom_factor > 1 ? 1 : a->zoom_last;
        } else if (IS(1, "in")) {
            c.zoom_factor = fmin(c.zoom_max, c.zoom_factor + c.zoom_step);
        } else if (IS(1, "out")) {
            c.zoom_factor = fmax(1, c.zoom_factor - c.zoom_step);
        } else {
            return app_error(out, n, "zoom toggle|in|out|reset|set FACTOR|follow on|off");
        }
    }
    *candidate = c;
    return 0;
}

static int command_preset(App *a, Config *candidate, int ac, char **av, char *name_out, char *out,
                          size_t n)
{
    Config c = *candidate;

    ARITY(2);
    char name[64];
    if (IS(1, "next")) {
        snprintf(name, sizeof name, "%s", a->current_preset);
        cycle(name, sizeof name, c.preset_order);
    } else if (app_copy_string(name, sizeof name, av[1], out, n)) {
        return -1;
    }
    Preset *p = NULL;
    for (int i = 0; i < c.preset_count; i++) {
        if (!strcmp(c.presets[i].name, name)) {
            p = &c.presets[i];
        }
    }
    if (!p) {
        return app_error(out, n, "unknown preset %s (configured in [preset.NAME])", name);
    }
    if (p->mask & 1) {
        strcpy(c.layout, p->layout);
    }
    if (p->mask & 2) {
        strcpy(c.shape, p->camera_shape);
    }
    if (p->mask & 4) {
        c.camera_width_percent = p->camera_width_percent;
    }
    if (p->mask & 8) {
        c.split_ratio = p->split_ratio;
    }
    if (p->mask & 16) {
        c.camera_visible = p->camera_visible;
    }
    if (p->mask & 32) {
        c.zoom_factor = p->zoom_factor;
    } else {
        c.zoom_factor = 1;
    }
    if (p->mask & 64) {
        strcpy(c.anchor, p->camera_anchor);
    }
    if (p->mask & 128) {
        strcpy(c.aspect, p->camera_aspect);
    }
    snprintf(name_out, sizeof a->current_preset, "%s", name);

    *candidate = c;
    return 0;
}

int app_command(App *a, int ac, char **av, char *out, size_t n)
{
    Config c = a->config;
    State s = a->state;
    Capabilities cap = platform_capabilities(a->platform);
    int result = 0;
    bool change = false;
    char applied_preset[sizeof a->current_preset] = "";
    out[0] = 0;
    if (IS(0, "status")) {
        if (ac != 1 && !(ac == 2 && IS(1, "--json"))) {
            return app_error(out, n, "status [--json]");
        }
        return status(a, ac == 2, out, n);
    }
    if (IS(0, "quit")) {
        ARITY(1);
        int result = app_shutdown_privacy(a, out, n);
        app_request_stop();
        if (!result) {
            snprintf(out, n, "cast shutting down; live output privacy-paused");
        }
        return result;
    }
    if (IS(0, "live") && IS(1, "message")) {
        ARITY(3);
        return command_message(a, av[2], out, n);
    }
    if (IS(0, "settings")) {
        if (ac < 3 || !(ac & 1)) {
            return app_error(out, n, "settings SECTION.KEY VALUE [SECTION.KEY VALUE ...]");
        }
        for (int i = 1; i < ac; i += 2) {
            if (config_set_value(&c, av[i], av[i + 1], out, n)) {
                return -1;
            }
        }
        if (apply_candidate(a, &c, out, n)) {
            return -1;
        }
        if (c.zoom_factor > 1) {
            a->zoom_last = c.zoom_factor;
        }
        snprintf(out, n, "session settings applied atomically; output states preserved");
        return 0;
    }
    if (IS(0, "pause") || IS(0, "resume") || IS(0, "live")) {
        if (IS(0, "live")) {
            ARITY(2);
        } else {
            ARITY(1);
        }
        if (IS(0, "pause") && a->countdown) {
            a->countdown = false;
            a->countdown_path[0] = 0;
        }
        if (state_command(&s, av[0], ac > 1 ? av[1] : "", out, n)) {
            return -1;
        }
        result = barrier(a, &s, out, n);
        if (!result) {
            snprintf(out, n, "outputs updated: live %s, record %s",
                     s.live_paused   ? "paused"
                     : s.live_frozen ? "frozen"
                                     : "live",
                     s.recording ? (s.record_paused ? "paused" : "recording") : "stopped");
        }
        return result;
    }
    if (IS(0, "record")) {
        if (ac < 2) {
            return app_error(out, n, "record start [PATH]|stop|pause|resume|toggle");
        }
        if (IS(1, "start")) {
            if (ac != 2 && ac != 3) {
                return app_error(out, n, "record start [PATH]");
            }
            if (a->state.recording || a->countdown || media_record_finalizing(a->media)) {
                return app_error(out, n, "a recording, countdown, or finalization already exists");
            }
            const char *path = ac == 3 ? av[2] : NULL;
            if (c.record_countdown) {
                if (path &&
                    app_copy_string(a->countdown_path, sizeof a->countdown_path, path, out, n)) {
                    return -1;
                }
                if (!path) {
                    a->countdown_path[0] = 0;
                }
                a->countdown = true;
                a->countdown_deadline =
                    cast_now_ns() + (uint64_t)c.record_countdown * 1000000000ULL;
                snprintf(out, n,
                         "recording countdown: %d seconds; cast record stop or cast pause cancels",
                         c.record_countdown);
                return 0;
            }
            if (app_recording_start(a, path, out, n)) {
                return -1;
            }
            snprintf(out, n, "recording %s%s", a->state.record_path,
                     a->state.record_paused ? " (paused by group pause)" : "");
            return 0;
        }
        ARITY(2);
        if (IS(1, "stop")) {
            if (a->countdown) {
                a->countdown = false;
                snprintf(out, n, "recording countdown cancelled");
                return 0;
            }
            if (!a->state.recording) {
                return app_error(out, n, "no recording to stop");
            }
            int rc = media_record_stop(a->media, out, n);
            a->record_finalizing = media_record_finalizing(a->media);
            a->state.recording = a->state.record_paused = a->state.group_record_restore = false;
            if (!out[0]) {
                snprintf(out, n, "finalized %s", a->state.record_path);
            }
            return rc;
        }
        if (state_command(&s, "record", av[1], out, n)) {
            return -1;
        }
        return barrier(a, &s, out, n);
    }
    if (IS(0, "config")) {
        ARITY(2);
        if (!IS(1, "reload")) {
            return app_error(out, n, "config reload (check/defaults run locally)");
        }
        Config fresh;
        if (config_load(&fresh, a->startup.config_path, a->startup.explicit_config, out, n) ||
            app_apply_overrides(&fresh, &a->startup, out, n) || app_socket_path(&fresh, out, n)) {
            return -1;
        }
        if (!cap.cursor_metadata && fresh.zoom_follow) {
            fresh.zoom_follow = false;
            fprintf(stderr,
                    "cast: backend lacks cursor metadata; reloaded cursor-follow disabled\n");
        }
        Config effective = fresh;
        if (apply_candidate(a, &fresh, out, n)) {
            return -1;
        }
        a->defaults = effective;
        a->zoom_last = effective.zoom_factor;
        snprintf(out, n, "configuration reloaded atomically; output states preserved");
        return 0;
    }
    if (IS(0, "reset")) {
        ARITY(1);
        Config fresh = a->config;
#define RESTORE(field) fresh.field = a->defaults.field
#define RESTORE_STR(field) memcpy(fresh.field, a->defaults.field, sizeof fresh.field)
        RESTORE_STR(layout);
        RESTORE_STR(shape);
        RESTORE_STR(anchor);
        RESTORE_STR(aspect);
        RESTORE_STR(fit);
        RESTORE_STR(split_side);
        RESTORE(camera_width_percent);
        RESTORE(split_ratio);
        RESTORE(margin);
        RESTORE(radius);
        RESTORE(border_width);
        RESTORE(border_color);
        RESTORE(camera_x);
        RESTORE(camera_y);
        RESTORE(crop_x);
        RESTORE(crop_y);
        RESTORE(camera_visible);
        RESTORE(mirror);
        RESTORE(zoom_follow);
        RESTORE(cursor);
        RESTORE(cursor_highlight);
        RESTORE(clicks);
        RESTORE(keys);
        RESTORE_STR(keys_mode);
        RESTORE(annotations_live_keys);
        RESTORE(annotations_live_clicks);
        RESTORE(annotations_record_keys);
        RESTORE(annotations_record_clicks);
        fresh.zoom_factor = 1;
#undef RESTORE
#undef RESTORE_STR
        if (apply_candidate(a, &fresh, out, n)) {
            return -1;
        }
        a->zoom_last = a->defaults.zoom_factor;
        snprintf(out, n, "composition reset; output states preserved");
        return 0;
    }
    if (IS(0, "layout")) {
        ARITY(2);
        if (IS(1, "next")) {
            cycle(c.layout, sizeof c.layout, c.layout_order);
        } else {
            ENUM(c.layout, av[1], "overlay,split,screen,camera");
        }
        change = true;
    } else if (IS(0, "split")) {
        ARITY(3);
        if (IS(1, "side")) {
            ENUM(c.split_side, av[2], "left,right");
        } else if (IS(1, "ratio")) {
            if (number(av[2], 5, 95, &c.split_ratio, true, out, n)) {
                return -1;
            }
        } else {
            return app_error(out, n, "split side left|right; split ratio PERCENT%%");
        }
        change = true;
    } else if (IS(0, "camera")) {
        if (command_camera(a, &c, ac, av, out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "screen") || IS(0, "capture") || IS(0, "preview")) {
        if (IS(0, "preview") && ac == 3 && IS(1, "target")) {
            ENUM(c.preview_target, av[2], "live,record");
            change = true;
        } else {
            if (platform_command(a->platform, &a->config, ac, av, out, n)) {
                return -1;
            }
            if (IS(0, "screen") && IS(1, "list")) {
                return 0;
            }
            app_sync_source(a);
            return 0;
        }
    } else if (IS(0, "zoom")) {
        if (command_zoom(a, &c, ac, av, out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "cursor")) {
        if (ac == 3 && IS(1, "highlight")) {
            if (toggle(&c.cursor_highlight, av[2], out, n)) {
                return -1;
            }
        } else {
            ARITY(2);
            if (cap.embedded_cursor) {
                return app_error(
                    out, n,
                    "cursor is embedded by capture backend; restart portal with cursor "
                    "policy to change visibility");
            }
            if (toggle(&c.cursor, av[1], out, n)) {
                return -1;
            }
        }
        if (!cap.cursor_metadata && !cap.embedded_cursor) {
            return app_error(out, n, "cursor metadata unsupported");
        }
        if (cap.embedded_cursor && ac == 3) {
            return app_error(
                out, n, "cursor highlighting requires metadata; unsupported for embedded cursor");
        }
        change = true;
    } else if (IS(0, "clicks")) {
        ARITY(2);
        if (!cap.input) {
            return app_error(out, n, "global click observation unsupported on this backend");
        }
        if (toggle(&c.clicks, av[1], out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "keys")) {
        if (!cap.input) {
            return app_error(out, n, "global key observation unsupported on this backend");
        }
        if (ac == 2 && IS(1, "clear")) {
            compositor_clear(a->compositor);
            return 0;
        }
        if (ac == 3 && IS(1, "mode")) {
            ENUM(c.keys_mode, av[2], "shortcuts,all");
        } else {
            ARITY(2);
            if (toggle(&c.keys, av[1], out, n)) {
                return -1;
            }
        }
        change = true;
    } else if (IS(0, "annotations")) {
        ARITY(4);
        if (!oneof(av[1], "live,record") || !oneof(av[2], "keys,clicks") ||
            !oneof(av[3], "on,off")) {
            return app_error(out, n, "annotations live|record keys|clicks on|off");
        }
        bool value = IS(3, "on");
        if (IS(1, "live")) {
            if (IS(2, "keys")) {
                c.annotations_live_keys = value;
            } else {
                c.annotations_live_clicks = value;
            }
        } else {
            if (IS(2, "keys")) {
                c.annotations_record_keys = value;
            } else {
                c.annotations_record_clicks = value;
            }
        }
        change = true;
    } else if (IS(0, "audio")) {
        if (media_audio_command(a->media, &c, ac, av, out, n)) {
            return -1;
        }
        if (IS(1, "list")) {
            return 0;
        }
        a->config = c;
        return 0;
    } else if (IS(0, "preset")) {
        if (command_preset(a, &c, ac, av, applied_preset, out, n)) {
            return -1;
        }
        change = true;
    } else {
        return app_error(out, n, "unknown command %s; cast --help", av[0]);
    }
    if (change) {
        if (config_validate(&c, out, n)) {
            return -1;
        }
        if (strcmp(c.camera_device, a->config.camera_device) ||
            c.camera_enabled != a->config.camera_enabled) {
            if (media_reconfigure(a->media, &c, a->state.recording, out, n)) {
                return -1;
            }
        }
        bool annotation_policy_changed = c.keys != a->config.keys || c.clicks != a->config.clicks ||
                                         strcmp(c.keys_mode, a->config.keys_mode);
        a->config = c;
        if (c.zoom_factor > 1) {
            a->zoom_last = c.zoom_factor;
        }
        if (applied_preset[0]) {
            snprintf(a->current_preset, sizeof a->current_preset, "%s", applied_preset);
        }
        if (annotation_policy_changed) {
            compositor_clear(a->compositor);
        }
    }
    if (!out[0]) {
        snprintf(out, n, "ok");
    }
    return 0;
#undef IS
#undef ARITY
#undef ENUM
}
