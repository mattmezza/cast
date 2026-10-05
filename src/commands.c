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
static void cycle(char *value, size_t n, const char *list, bool previous)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    char *items[CAST_MAX_PRESETS], *save = NULL;
    int count = 0, current = -1;
    for (char *p = strtok_r(buf, ",", &save); p && count < CAST_MAX_PRESETS;
         p = strtok_r(NULL, ",", &save)) {
        items[count] = p;
        if (!strcmp(value, p)) {
            current = count;
        }
        count++;
    }
    if (count) {
        int next = current < 0 ? (previous ? count - 1 : 0)
                   : previous  ? (current + count - 1) % count
                               : (current + 1) % count;
        snprintf(value, n, "%s", items[next]);
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
/* Display only the authority; URL paths/query parameters are destination-private. */
static void stream_destination(const StreamConfig *config, char *destination, size_t n)
{
    const char *host = strstr(config->server_url, "://");
    if (!host) {
        snprintf(destination, n, "unconfigured");
        return;
    }
    host += 3;
    size_t length = strcspn(host, "/?#");
    snprintf(destination, n, "%.*s", (int)length, host);
}
static void select_preview(App *app, const char *target)
{
    snprintf(app->config.preview_target, sizeof app->config.preview_target, "%s", target);
    panel_transport_barrier(app->panel, app, false);
}
static int stream_status_reply(App *a, bool json, char *out, size_t n)
{
    StreamSnapshot status;
    media_stream_status(a->media, &status);
    char destination[1024], safe_destination[6147], safe_error[CAST_ERR * 6 + 3];
    stream_destination(&a->config.stream, destination, sizeof destination);
    json_string(safe_destination, sizeof safe_destination, destination);
    json_string(safe_error, sizeof safe_error, status.error);
    uint64_t now = cast_now_ns();
    double session = status.active && status.session_started_ns && now > status.session_started_ns
                         ? (now - status.session_started_ns) / 1e9
                         : 0;
    double connected = status.state == STREAM_STREAMING && status.connected_started_ns &&
                               now > status.connected_started_ns
                           ? (now - status.connected_started_ns) / 1e9
                           : 0;
    double bitrate = status.outgoing_bitrate_kbps;
    double retry = status.next_retry_ns > now ? (status.next_retry_ns - now) / 1e9 : 0;
    int length;
    if (json) {
        length = snprintf(
            out, n,
            "{\"state\":\"%s\",\"active\":%s,\"paused\":%s,\"frozen\":%s,\"blurred\":%s,"
            "\"service\":\"%s\",\"destination\":%s,\"generation\":%llu,"
            "\"session_duration\":%.3f,\"connected_duration\":%.3f,\"outgoing_bitrate_kbps\":%.3f,"
            "\"bytes_written\":%llu,\"video_frames\":%llu,\"audio_samples\":%llu,"
            "\"queue_depth\":%u,\"dropped_frames\":%llu,\"retry_attempt\":%u,"
            "\"next_retry_seconds\":%.3f,\"error\":%s}",
            stream_lifecycle_name(status.state), status.active ? "true" : "false",
            a->state.stream_paused ? "true" : "false", a->state.stream_frozen ? "true" : "false",
            a->state.stream_blurred ? "true" : "false", a->config.stream.service, safe_destination,
            (unsigned long long)status.generation, session, connected, bitrate,
            (unsigned long long)status.bytes_written, (unsigned long long)status.video_frames,
            (unsigned long long)status.audio_samples, status.queue_depth,
            (unsigned long long)status.dropped_frames, status.retry_attempt, retry, safe_error);
    } else {
        length = snprintf(
            out, n,
            "stream=%s presentation=%s service=%s destination=%s session=%.3fs connected=%.3fs "
            "outgoing=%.1fkbps queue=%u dropped=%llu retry=%u next_retry=%.3fs error=%s\n"
            "Network writes describe local transmission; service/viewer delivery is not measured.",
            stream_lifecycle_name(status.state),
            a->state.stream_paused    ? "paused"
            : a->state.stream_blurred ? "blurred"
            : a->state.stream_frozen  ? "frozen"
                                      : "composition",
            a->config.stream.service, destination, session, connected, bitrate, status.queue_depth,
            (unsigned long long)status.dropped_frames, status.retry_attempt, retry, status.error);
    }
    if (length < 0 || (size_t)length >= n) {
        return app_error(out, n, "stream status exceeds IPC limit");
    }
    return 0;
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
    char stream[CAST_ERR * 2];
    if (stream_status_reply(a, json, stream, sizeof stream)) {
        return app_error(out, n, "%s", stream);
    }
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
            "s\",\"region\":[%d,%d,%d,%d],\"mask_color\":\"#%06x\"},"
            "\"virtual\":{\"enabled\":%s,\"state\":\"%s\","
            "\"message\":%"
            "s,\"paused\":%s,\"frozen\":%s,\"blurred\":%s},"
            "\"record\":{\"state\":\"%s\",\"path\":%s,\"duration\":%.3f,\"countdown\":%s,"
            "\"finalizing\":%s,\"paused\":%s,\"frozen\":%s,\"blurred\":%s,\"cut\":%s,"
            "\"countdown_kind\":%s},"
            "\"stream\":%s,\"group_paused\":%s,\"layout\":\"%s\",\"zoom\":%.3f,\"camera_visible\":%"
            "s,"
            "\"audio\":{\"mic\":{\"enabled\":%s,\"source\":%s,\"gain\":%.3f},"
            "\"desktop\":{\"enabled\":%s,\"source\":%s,\"gain\":%.3f},"
            "\"virtual\":{\"enabled\":%s,\"name\":%s},\"readiness\":%s},"
            "\"last_error\":%s,\"dropped_frames\":%llu}",
            CAST_VERSION, backend, cap.capture ? "true" : "false",
            cap.cursor_metadata ? "true" : "false", cap.embedded_cursor ? "true" : "false",
            cap.input ? "true" : "false", cap.region_selection ? "true" : "false",
            cap.window_selection ? "true" : "false", cap.preview ? "true" : "false", source,
            c->capture_kind, c->region_x, c->region_y, c->region_w, c->region_h,
            c->capture_mask_color,
            c->virtual_enabled ? "true" : "false",
            !c->virtual_enabled  ? "stopped"
            : s->virtual_paused  ? "paused"
            : s->virtual_blurred ? "blurred"
            : s->virtual_frozen  ? "frozen"
                                 : "virtual",
            pause_text, s->virtual_paused ? "true" : "false", s->virtual_frozen ? "true" : "false",
            s->virtual_blurred ? "true" : "false",
            !s->recording       ? "stopped"
            : s->record_cut     ? "cut"
            : s->record_paused  ? "paused"
            : s->record_blurred ? "blurred"
            : s->record_frozen  ? "frozen"
                                : "recording",
            path, seconds, a->countdown ? "true" : "false",
            media_record_finalizing(a->media) ? "true" : "false",
            s->record_paused ? "true" : "false", s->record_frozen ? "true" : "false",
            s->record_blurred ? "true" : "false", s->record_cut ? "true" : "false",
            !a->countdown         ? "null"
            : a->countdown_resume ? "\"resume\""
                                  : "\"start\"",
            stream, s->group_paused ? "true" : "false", c->layout, c->zoom_factor,
            c->camera_visible ? "true" : "false", c->mic ? "true" : "false", mic_source,
            c->mic_gain, c->desktop ? "true" : "false", desktop_source, c->desktop_gain,
            c->virtual_audio ? "true" : "false", virtual_name, audio[0] ? audio : "{}", err,
            (unsigned long long)s->dropped_frames);
    } else {
        length = snprintf(
            out, n,
            "backend=%s source=%s:%s layout=%s camera=%s zoom=%.2f\nvirtual=%s recording=%s "
            "duration=%.3fs path=%s "
            "countdown=%s\nvirtual_message=%s\n%s\naudio=%s\ndropped_frames=%llu error=%s",
            c->backend, c->capture_kind, c->monitor[0] ? c->monitor : "selected", c->layout,
            c->camera_visible ? "visible" : "hidden", c->zoom_factor,
            s->virtual_paused    ? "PAUSED"
            : s->virtual_blurred ? "BLURRED"
            : s->virtual_frozen  ? "FROZEN"
                                 : "VIRTUAL",
            s->recording                        ? (s->record_cut       ? "CUT"
                                                   : s->record_paused  ? "RECORDING-PAUSED"
                                                   : s->record_blurred ? "BLURRED"
                                                   : s->record_frozen  ? "FROZEN"
                                                                       : "RECORDING")
            : media_record_finalizing(a->media) ? "finalizing"
                                                : "stopped",
            seconds, s->record_path, a->countdown ? "pending" : "off", c->pause_text, stream, audio,
            (unsigned long long)s->dropped_frames, s->last_error);
    }
    if (length < 0 || (size_t)length >= n) {
        return app_error(out, n, "status exceeds IPC limit; use shorter paths/source names");
    }
    return 0;
}
static int barrier(App *a, State *candidate, char *e, size_t n)
{
    int result = 0;
    State previous = a->state;
    if (candidate->virtual_frozen && !previous.virtual_frozen &&
        app_freeze_frame(a, false, &a->frozen, e, n)) {
        return -1;
    }
    if (candidate->record_frozen && !previous.record_frozen &&
        app_freeze_frame(a, true, &a->record_frozen, e, n)) {
        return -1;
    }
    if (candidate->stream_frozen && !previous.stream_frozen &&
        app_freeze_frame(a, 2, &a->stream_frozen, e, n)) {
        return -1;
    }
    a->state = *candidate;
    platform_events(a->platform, a->compositor, &a->config, true);
    compositor_clear(a->compositor);
    if (candidate->virtual_paused || (previous.virtual_frozen && !candidate->virtual_frozen) ||
        (previous.virtual_blurred && !candidate->virtual_blurred)) {
        frame_free(&a->virtual_raw);
    }
    if (candidate->record_paused || candidate->record_cut ||
        (previous.record_frozen && !candidate->record_frozen) ||
        (previous.record_blurred && !candidate->record_blurred)) {
        frame_free(&a->record_raw);
    }
    bool stream_changed = candidate->stream_paused != previous.stream_paused ||
                          candidate->stream_frozen != previous.stream_frozen ||
                          candidate->stream_blurred != previous.stream_blurred;
    if (stream_changed) {
        frame_free(&a->stream_raw);
        if (candidate->stream_paused && candidate->stream_frozen && !previous.stream_paused) {
            if (frame_copy(&a->stream_frozen, &a->neutral)) {
                frame_free(&a->stream_frozen);
            }
        }
        media_stream_privacy(a->media, candidate->stream_paused || candidate->stream_frozen ||
                                           candidate->stream_blurred);
        media_stream_barrier(a->media);
    }
    if (app_output_frames(a, e, n)) {
        result = -1;
    }
    if (candidate->virtual_paused && !previous.virtual_paused && candidate->virtual_frozen) {
        if (frame_copy(&a->frozen, &a->neutral)) {
            frame_free(&a->frozen);
            result = app_error(e, n, "cannot replace frozen virtual frame with neutral content");
        }
    }
    if (candidate->record_paused && !previous.record_paused && candidate->record_frozen) {
        if (frame_copy(&a->record_frozen, &a->neutral)) {
            frame_free(&a->record_frozen);
            result = app_error(e, n, "cannot replace frozen recording frame with neutral content");
        }
    }
    bool virtual_silent = !a->config.virtual_enabled || candidate->virtual_paused ||
                          candidate->virtual_frozen || candidate->virtual_blurred;
    bool record_silent =
        candidate->record_paused || candidate->record_frozen || candidate->record_blurred;
    if (a->config.virtual_enabled &&
        media_virtual(a->media, a->virtual.data ? &a->virtual : &a->neutral, virtual_silent, e,
                      n)) {
        result = -1;
    }
    if (media_privacy(a->media, virtual_silent, candidate->recording && record_silent,
                      candidate->recording && candidate->record_cut, e, n)) {
        result = -1;
    }
    if (candidate->recording && (candidate->record_paused != previous.record_paused ||
                                 candidate->record_frozen != previous.record_frozen ||
                                 candidate->record_blurred != previous.record_blurred ||
                                 candidate->record_cut != previous.record_cut)) {
        media_record_barrier(a->media);
    }
    if (candidate->recording) {
        bool active, cut;
        uint64_t dropped;
        char recorder_error[CAST_ERR];
        media_status(a->media, &active, &cut, &dropped, recorder_error, sizeof recorder_error);
        if (!active) {
            a->state.recording = false;
            a->state.record_cut = false;
            snprintf(e, n, "%s",
                     recorder_error[0] ? recorder_error : "recording is no longer active");
            result = -1;
        }
    }
    if (a->state.recording && !candidate->record_cut &&
        (candidate->record_paused || (candidate->record_frozen && a->record_frozen.data) ||
         a->record_raw.data)) {
        a->record.ts_ns = cast_now_ns();
        if (media_record_frame(a->media, a->record.data ? &a->record : &a->neutral, e, n)) {
            result = -1;
        }
    }
    if (candidate->stream_active && stream_changed &&
        media_stream_frame(a->media, a->stream.data ? &a->stream : &a->neutral,
                           candidate->stream_paused || candidate->stream_frozen ||
                               candidate->stream_blurred,
                           e, n)) {
        result = -1;
    }
    panel_transport_barrier(a->panel, a, false);
    if (platform_capabilities(a->platform).preview) {
        const Frame *target = !strcmp(a->config.preview_target, "record")   ? &a->record
                              : !strcmp(a->config.preview_target, "stream") ? &a->stream
                                                                            : &a->virtual;
        if (!target->data) {
            target = &a->neutral;
        }
        if (platform_preview(a->platform, target, candidate, &a->config, e, n)) {
            result = -1;
        }
    }
    return result;
}
int app_recording_start(App *a, const char *path, char *e, size_t n)
{
    /* Countdown frames may include its native preview; admit a fresh frame only. */
    frame_free(&a->record_raw);
    if (media_record_start(a->media, &a->config, path, e, n)) {
        return -1;
    }
    a->state.recording = true;
    a->state.record_paused = false;
    a->state.record_cut = a->state.record_frozen = a->state.record_blurred = false;
    frame_free(&a->record_frozen);
    a->state.group_record_restore = false;
    a->state.record_started_ns = cast_now_ns();
    media_record_path(a->media, a->state.record_path, sizeof a->state.record_path);
    a->state.record_paused = a->state.group_paused;
    State candidate = a->state;
    int result = barrier(a, &candidate, e, n);
    if (!result) {
        select_preview(a, "record");
    }
    return result;
}
int app_recording_resume(App *a, char *e, size_t n)
{
    if (!a->state.recording || !a->state.record_cut) {
        return app_error(e, n, "no cut recording to resume");
    }
    State candidate = a->state;
    candidate.record_cut = false;
    frame_free(&a->record_raw);
    int result = barrier(a, &candidate, e, n);
    if (!result) {
        select_preview(a, "record");
    }
    return result;
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
    RESTART(old->virtual_enabled != c->virtual_enabled, "output enabled");
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
    StreamSnapshot stream;
    media_stream_status(a->media, &stream);
    if (stream.active && stream_settings_changed(&a->config.stream, &c->stream)) {
        return app_error(e, n,
                         "stream connection/encoder settings are locked; cast stream stop first");
    }
    Capabilities cap = platform_capabilities(a->platform);
    if (!cap.panel_exclusion && c->capture_mask_color != a->config.capture_mask_color) {
        return app_error(e, n, "capture exclusion settings require the Xorg backend");
    }
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
    if (compositor_prepare(a->compositor, c, e, n)) {
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
    bool mask_color_only =
        a->config.capture_mask_color != c->capture_mask_color &&
        !strcmp(a->config.capture_kind, c->capture_kind) &&
        !strcmp(a->config.monitor, c->monitor) && a->config.region_x == c->region_x &&
        a->config.region_y == c->region_y && a->config.region_w == c->region_w &&
        a->config.region_h == c->region_h;
    a->config = *c;
    app_sync_source(a);
    if (mask_color_only) {
        /* Retire source pixels without changing the user's current zoom. */
        a->config.zoom_factor = c->zoom_factor;
    }
    compositor_clear(a->compositor);
    frame_free(&a->virtual_raw);
    frame_free(&a->record_raw);
    frame_free(&a->stream_raw);
    media_stream_barrier(a->media);
    if (app_output_frames(a, e, n)) {
        return -1;
    }
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
    Config candidate = a->config;
    if (config_set_value(&candidate, "output.pause_title", value, out, n) ||
        compositor_prepare(a->compositor, &candidate, out, n)) {
        return -1;
    }
    a->config = candidate;
    if (app_output_frames(a, out, n)) {
        return -1;
    }
    int result = 0;
    if (a->state.virtual_paused && a->config.virtual_enabled &&
        media_virtual(a->media, &a->neutral, true, out, n)) {
        result = -1;
    }
    /* An active freeze keeps its original pixels; changing the label never changes state. */
    if (platform_capabilities(a->platform).preview &&
        ((!strcmp(a->config.preview_target, "virtual") && a->state.virtual_paused) ||
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
        if (IS(2, "next") || IS(2, "prev")) {
            cycle(c.anchor, sizeof c.anchor, c.corner_order, IS(2, "prev"));
        } else {
            ENUM(c.anchor, av[2],
                 "top-left,top-right,bottom-left,bottom-right,top,bottom,left,right,top-center,"
                 "bottom-center,center-left,center-right");
        }
    } else if (IS(1, "shape")) {
        ARITY(3);
        if (IS(2, "next") || IS(2, "prev")) {
            cycle(c.shape, sizeof c.shape, "rectangle,rounded,circle", IS(2, "prev"));
        } else {
            ENUM(c.shape, av[2], "rectangle,rounded,circle");
        }
    } else if (IS(1, "aspect")) {
        ARITY(3);
        if (IS(2, "next") || IS(2, "prev")) {
            cycle(c.aspect, sizeof c.aspect, "native,16:9,4:3,1:1", IS(2, "prev"));
        } else {
            ENUM(c.aspect, av[2], "native,16:9,4:3,1:1");
        }
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

static int set_size_percent(const char *value, double *size, char *out, size_t n)
{
    double percent;
    if (number(value, -100, 100, &percent, true, out, n)) {
        return -1;
    }
    if (*value == '+' || *value == '-') {
        percent += *size;
    }
    if (percent < 1 || percent > 100) {
        return app_error(out, n, "size must be 1..100 percentage points");
    }
    *size = percent;
    return 0;
}
static int set_opacity(const char *value, double *opacity, char *out, size_t n)
{
    bool percent = strchr(value, '%') != NULL;
    if (number(value, 0, percent ? 100 : 1, opacity, percent, out, n)) {
        return -1;
    }
    if (percent) {
        *opacity /= 100;
    }
    return 0;
}
static int command_screen_style(Config *candidate, int ac, char **av, char *out, size_t n)
{
    if (IS(1, "size")) {
        ARITY(3);
        return set_size_percent(av[2], &candidate->screen_width_percent, out, n);
    }
    const char *key;
    int value;
    if (IS(1, "border")) {
        ARITY(4);
        if (!IS(2, "width") && !IS(2, "color")) {
            return app_error(out, n, "screen border width PIXELS|color #RRGGBB");
        }
        key = IS(2, "width") ? "screen.border_width" : "screen.border_color";
        value = 3;
    } else {
        ARITY(3);
        key = IS(1, "margin")   ? "screen.margin"
              : IS(1, "radius") ? "screen.radius"
                                : "screen.background";
        value = 2;
    }
    return config_set_value(candidate, key, av[value], out, n);
}
static int command_layer(Config *candidate, bool text, int ac, char **av, char *out, size_t n)
{
    if (ac < 2) {
        return app_error(out, n, "%s subcommand required", text ? "text" : "logo");
    }
    if (IS(1, "on") || IS(1, "off") || IS(1, "toggle")) {
        ARITY(2);
        return toggle(text ? &candidate->text_enabled : &candidate->logo_enabled, av[1], out, n);
    }
    if (IS(1, "size") && !text) {
        ARITY(3);
        return set_size_percent(av[2], &candidate->logo_width_percent, out, n);
    }
    if (IS(1, "opacity")) {
        ARITY(3);
        return set_opacity(av[2], text ? &candidate->text_opacity : &candidate->logo_opacity, out,
                           n);
    }
    if (IS(1, "margin")) {
        ARITY(4);
        if (config_set_value(candidate, text ? "text.margin_x" : "logo.margin_x", av[2], out, n)) {
            return -1;
        }
        return config_set_value(candidate, text ? "text.margin_y" : "logo.margin_y", av[3], out, n);
    }
    ARITY(3);
    const char *key = IS(1, "anchor")          ? (text ? "text.anchor" : "logo.anchor")
                      : text && IS(1, "set")   ? "text.content"
                      : text && IS(1, "font")  ? "text.font"
                      : text && IS(1, "size")  ? "text.size"
                      : text && IS(1, "color") ? "text.color"
                      : !text && IS(1, "path") ? "logo.path"
                                               : NULL;
    if (!key) {
        return app_error(out, n, "unknown %s subcommand", text ? "text" : "logo");
    }
    return config_set_value(candidate, key, av[2], out, n);
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
    if (IS(1, "next") || IS(1, "prev")) {
        snprintf(name, sizeof name, "%s", a->current_preset);
        cycle(name, sizeof name, c.preset_order, IS(1, "prev"));
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

int app_countdown_guide(App *a, uint64_t remaining_ns, char *error, size_t n)
{
    /* The native preview owns the visible guide, including with a panel attached. */
    if (remaining_ns) {
        if (!a->record_raw.data && a->screen.data &&
            compositor_render(a->compositor, &a->config, &a->screen,
                              a->config.camera_enabled && a->camera.data ? &a->camera : NULL,
                              &a->cursor, true, &a->record_raw, error, n)) {
            return -1;
        }
        if (app_output_frames(a, error, n) ||
            platform_countdown_frame(a->platform, a->record.data ? &a->record : &a->neutral,
                                     &a->config, error, n)) {
            return -1;
        }
        if (platform_capabilities(a->platform).preview) {
            a->countdown_preview = true;
        }
    }
    int result = platform_countdown(a->platform, remaining_ns, error, n);
    if (!remaining_ns && a->countdown_preview) {
        a->config.preview = false;
        if (!result) {
            a->countdown_preview = false;
        }
    }
    return result;
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
    if (IS(0, "live") || (IS(0, "annotations") && IS(1, "live")) ||
        (IS(0, "preview") && IS(1, "target") && IS(2, "live"))) {
        return app_error(out, n,
                         "cast live was renamed; use cast virtual (annotations virtual; preview "
                         "target virtual)");
    }
    if (IS(0, "stream")) {
        if (IS(1, "status")) {
            if (ac != 2 && !(ac == 3 && IS(2, "--json"))) {
                return app_error(out, n, "stream status [--json]");
            }
            return stream_status_reply(a, ac == 3, out, n);
        }
        if (IS(1, "start")) {
            ARITY(2);
            StreamSnapshot current;
            media_stream_status(a->media, &current);
            if (current.active) {
                return app_error(out, n, "stream session already exists; cast stream stop first");
            }
            if (media_stream_start(a->media, &a->config, out, n)) {
                return -1;
            }
            a->state.stream_active = true;
            a->state.stream_connection_state = STREAM_CONNECTING;
            a->state.stream_paused = true;
            a->state.stream_frozen = a->state.stream_blurred = false;
            a->state.group_stream_restore = false;
            frame_free(&a->stream_raw);
            frame_free(&a->stream_frozen);
            State candidate = a->state;
            if (barrier(a, &candidate, out, n)) {
                return -1;
            }
            select_preview(a, "stream");
            snprintf(out, n,
                     "stream startup accepted in solid pause; inspect cast stream status, then "
                     "cast stream resume");
            return 0;
        }
        if (IS(1, "stop")) {
            ARITY(2);
            if (media_stream_stop(a->media, out, n)) {
                return -1;
            }
            a->state.stream_active = false;
            a->state.stream_connection_state = STREAM_STOPPED;
            a->state.stream_paused = true;
            a->state.stream_frozen = a->state.stream_blurred = false;
            a->state.group_stream_restore = false;
            frame_free(&a->stream_raw);
            frame_free(&a->stream_frozen);
            app_output_frames(a, out, n);
            panel_transport_barrier(a->panel, a, false);
            snprintf(out, n,
                     "stream stopped; service buffering may delay the public broadcast ending");
            return 0;
        }
        if (IS(1, "title") || IS(1, "subtitle") || IS(1, "footer")) {
            ARITY(3);
            const char *key = IS(1, "title")      ? "output.pause_title"
                              : IS(1, "subtitle") ? "output.pause_subtitle"
                                                  : "output.pause_footer";
            if (config_set_value(&c, key, av[2], out, n) || apply_candidate(a, &c, out, n)) {
                return -1;
            }
            snprintf(out, n, "shared output %s updated for this session", av[1]);
            return 0;
        }
        const char *action = ac > 1 ? av[1] : "";
        if (IS(1, "blur") && ac == 3) {
            if (!oneof(av[2], "on,off,toggle")) {
                return app_error(out, n, "stream blur [on|off|toggle]");
            }
            action = IS(2, "on") ? "blur" : IS(2, "off") ? "unblur" : "blur-toggle";
        } else {
            ARITY(2);
        }
        StreamSnapshot current;
        media_stream_status(a->media, &current);
        s.stream_active = current.active;
        if (state_command(&s, "stream", action, out, n)) {
            return -1;
        }
        if (barrier(a, &s, out, n)) {
            return -1;
        }
        if (!strcmp(action, "resume")) {
            select_preview(a, "stream");
        }
        snprintf(out, n, "stream presentation %s; previously transmitted media cannot be retracted",
                 action);
        return 0;
    }
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
            snprintf(out, n, "cast shutting down; virtual output privacy-paused");
        }
        return result;
    }
    if (IS(0, "virtual") && (IS(1, "start") || IS(1, "stop"))) {
        ARITY(2);
        bool enabled = IS(1, "start");
        if (enabled && a->config.virtual_enabled) {
            return app_error(out, n, "virtual camera is already enabled; use cast virtual resume");
        }
        if (media_virtual_enabled(a->media, &a->config, enabled, out, n)) {
            return -1;
        }
        a->config.virtual_enabled = enabled;
        a->state.virtual_paused = true;
        a->state.virtual_frozen = a->state.virtual_blurred = false;
        a->state.group_virtual_restore = false;
        frame_free(&a->virtual_raw);
        frame_free(&a->frozen);
        State candidate = a->state;
        if (barrier(a, &candidate, out, n)) {
            return -1;
        }
        if (enabled) {
            select_preview(a, "virtual");
        }
        snprintf(out, n, "%s",
                 enabled ? "virtual camera started privacy-paused; use cast virtual resume"
                         : "virtual camera stopped");
        return 0;
    }
    if (IS(0, "virtual") && IS(1, "message")) {
        ARITY(3);
        return command_message(a, av[2], out, n);
    }
    if ((IS(0, "virtual") || IS(0, "record")) &&
        (IS(1, "title") || IS(1, "subtitle") || IS(1, "footer"))) {
        ARITY(3);
        const char *key = IS(1, "title")      ? "output.pause_title"
                          : IS(1, "subtitle") ? "output.pause_subtitle"
                                              : "output.pause_footer";
        if (config_set_value(&c, key, av[2], out, n) || apply_candidate(a, &c, out, n)) {
            return -1;
        }
        snprintf(out, n, "shared output %s updated for this session", av[1]);
        return 0;
    }
    if (IS(0, "settings")) {
        if (ac < 3 || !(ac & 1)) {
            return app_error(out, n, "settings SECTION.KEY VALUE [SECTION.KEY VALUE ...]");
        }
        for (int i = 1; i < ac; i += 2) {
            if (!strcmp(av[i], "output.pause_font") || !strcmp(av[i], "output.blur_font") ||
                !strcmp(av[i], "keys.font")) {
                return app_error(out, n,
                                 "font settings require editing cast.conf and cast config reload");
            }
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
    if (IS(0, "pause") || IS(0, "resume") || IS(0, "virtual")) {
        const char *action = ac > 1 ? av[1] : "";
        if (IS(0, "virtual")) {
            if (IS(1, "blur") && ac == 3) {
                if (!oneof(av[2], "on,off,toggle")) {
                    return app_error(out, n, "virtual blur on|off|toggle");
                }
                action = IS(2, "on") ? "blur" : IS(2, "off") ? "unblur" : "blur-toggle";
            } else {
                ARITY(2);
            }
        } else {
            ARITY(1);
        }
        if (IS(0, "pause")) {
            app_countdown_cancel(a);
        }
        if (state_command(&s, av[0], action, out, n)) {
            return -1;
        }
        result = barrier(a, &s, out, n);
        if (!result && IS(0, "virtual") && IS(1, "resume")) {
            select_preview(a, "virtual");
        }
        if (!result) {
            snprintf(out, n, "outputs updated: virtual %s, record %s",
                     s.virtual_paused    ? "paused"
                     : s.virtual_blurred ? "blurred"
                     : s.virtual_frozen  ? "frozen"
                                         : "virtual",
                     !s.recording       ? "stopped"
                     : s.record_cut     ? "cut"
                     : s.record_paused  ? "paused"
                     : s.record_blurred ? "blurred"
                     : s.record_frozen  ? "frozen"
                                        : "recording");
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
                a->countdown_resume = false;
                a->countdown_deadline =
                    cast_now_ns() + (uint64_t)c.record_countdown * 1000000000ULL;
                int guide =
                    app_countdown_guide(a, (uint64_t)c.record_countdown * 1000000000ULL, out, n);
                if (guide != 0) {
                    app_countdown_cancel(a);
                    if (guide > 0) {
                        return app_error(out, n, "recording countdown cancelled");
                    }
                    return -1;
                }
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
        const char *action = av[1];
        if (IS(1, "blur") && ac == 3) {
            if (!oneof(av[2], "on,off,toggle")) {
                return app_error(out, n, "record blur on|off|toggle");
            }
            action = IS(2, "on") ? "blur" : IS(2, "off") ? "unblur" : "blur-toggle";
        } else {
            ARITY(2);
        }
        if (IS(1, "stop")) {
            if (a->countdown) {
                bool resume = a->countdown_resume;
                app_countdown_cancel(a);
                if (!resume) {
                    snprintf(out, n, "recording countdown cancelled");
                    return 0;
                }
            }
            if (!a->state.recording) {
                return app_error(out, n, "no recording to stop");
            }
            int rc = media_record_stop(a->media, out, n);
            a->record_finalizing = media_record_finalizing(a->media);
            a->state.recording = a->state.record_paused = a->state.group_record_restore = false;
            a->state.record_cut = a->state.record_frozen = a->state.record_blurred = false;
            frame_free(&a->record_frozen);
            if (!out[0]) {
                snprintf(out, n, "finalized %s", a->state.record_path);
            }
            return rc;
        }
        if (IS(1, "cancel")) {
            if (!a->countdown) {
                return app_error(out, n, "no recording countdown to cancel");
            }
            app_countdown_cancel(a);
            snprintf(out, n, "recording countdown cancelled");
            return 0;
        }
        if (IS(1, "resume") && s.recording && s.record_cut) {
            if (a->countdown && a->countdown_resume) {
                snprintf(out, n, "recording resume countdown already pending");
                return 0;
            }
            if (c.record_countdown) {
                a->countdown = a->countdown_resume = true;
                a->countdown_path[0] = 0;
                a->countdown_deadline =
                    cast_now_ns() + (uint64_t)c.record_countdown * 1000000000ULL;
                int guide =
                    app_countdown_guide(a, (uint64_t)c.record_countdown * 1000000000ULL, out, n);
                if (guide != 0) {
                    app_countdown_cancel(a);
                    if (guide > 0) {
                        return app_error(
                            out, n, "recording resume countdown cancelled; recording remains cut");
                    }
                    return -1;
                }
                snprintf(out, n, "recording resume countdown: %d seconds; same file %s",
                         c.record_countdown, s.record_path);
                return 0;
            }
            return app_recording_resume(a, out, n);
        }
        if (IS(1, "pause") || IS(1, "cut")) {
            bool initial_countdown = a->countdown && !a->countdown_resume;
            app_countdown_cancel(a);
            if (initial_countdown && !s.recording) {
                snprintf(out, n, "recording countdown cancelled");
                return 0;
            }
        }
        if (state_command(&s, "record", action, out, n)) {
            return -1;
        }
        if (s.record_paused && !a->state.record_paused) {
            app_countdown_cancel(a);
        }
        result = barrier(a, &s, out, n);
        if (!result && IS(1, "resume")) {
            select_preview(a, "record");
        }
        return result;
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
        fresh.virtual_enabled = a->config.virtual_enabled;
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
        RESTORE_STR(camera_background);
        RESTORE(camera_background_color);
        RESTORE(camera_background_blur_radius);
        RESTORE(camera_background_brightness);
        RESTORE(screen_width_percent);
        RESTORE(screen_margin);
        RESTORE(screen_radius);
        RESTORE(screen_border_width);
        RESTORE(screen_border_color);
        RESTORE_STR(screen_background);
        RESTORE(screen_background_color);
        RESTORE(screen_background_blur_radius);
        RESTORE(screen_background_brightness);
        RESTORE_STR(background_source);
        RESTORE(gradient_from);
        RESTORE(gradient_via);
        RESTORE(gradient_to);
        RESTORE(gradient_via_enabled);
        RESTORE(gradient_angle);
        RESTORE(gradient_waypoint);
        RESTORE(logo_enabled);
        RESTORE_STR(logo_path);
        RESTORE_STR(logo_anchor);
        RESTORE(logo_width_percent);
        RESTORE(logo_margin_x);
        RESTORE(logo_margin_y);
        RESTORE(logo_opacity);
        RESTORE(text_enabled);
        RESTORE_STR(text_content);
        RESTORE_STR(text_font);
        RESTORE(text_size);
        RESTORE(text_color);
        RESTORE_STR(text_anchor);
        RESTORE(text_margin_x);
        RESTORE(text_margin_y);
        RESTORE(text_opacity);
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
        RESTORE(annotations_virtual_keys);
        RESTORE(annotations_virtual_clicks);
        RESTORE(annotations_record_keys);
        RESTORE(annotations_record_clicks);
        RESTORE(annotations_stream_keys);
        RESTORE(annotations_stream_clicks);
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
        if (IS(1, "next") || IS(1, "prev")) {
            cycle(c.layout, sizeof c.layout, c.layout_order, IS(1, "prev"));
        } else {
            ENUM(c.layout, av[1], "overlay,stage,split,screen,camera");
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
    } else if (IS(0, "logo") || IS(0, "text")) {
        if (command_layer(&c, IS(0, "text"), ac, av, out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "screen") && ac >= 2 && oneof(av[1], "size,margin,radius,border,background")) {
        if (command_screen_style(&c, ac, av, out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "screen") || IS(0, "capture") || IS(0, "preview")) {
        if (IS(0, "capture") && ac >= 2 && IS(1, "mask-color")) {
            ARITY(3);
            if (!cap.panel_exclusion) {
                return app_error(out, n, "capture exclusion settings require the Xorg backend");
            }
            const char *key = "capture.mask_color";
            if (config_set_value(&c, key, av[2], out, n)) {
                return -1;
            }
            if (apply_candidate(a, &c, out, n)) {
                return -1;
            }
            snprintf(out, n, "%s: %s", key, av[2]);
            return 0;
        } else if (IS(0, "preview") && ac == 3 && IS(1, "target")) {
            ENUM(c.preview_target, av[2], "virtual,record,stream");
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
            frame_free(&a->virtual_raw);
            frame_free(&a->record_raw);
            frame_free(&a->stream_raw);
            media_stream_barrier(a->media);
            return barrier(a, &s, out, n);
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
        if (!oneof(av[1], "virtual,record,stream") || !oneof(av[2], "keys,clicks") ||
            !oneof(av[3], "on,off")) {
            return app_error(out, n, "annotations virtual|record|stream keys|clicks on|off");
        }
        bool value = IS(3, "on");
        if (IS(1, "virtual")) {
            if (IS(2, "keys")) {
                c.annotations_virtual_keys = value;
            } else {
                c.annotations_virtual_clicks = value;
            }
        } else if (IS(1, "stream")) {
            if (IS(2, "keys")) {
                c.annotations_stream_keys = value;
            } else {
                c.annotations_stream_clicks = value;
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
        if (config_validate(&c, out, n) || compositor_prepare(a->compositor, &c, out, n)) {
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
        bool stream_annotations_changed =
            annotation_policy_changed ||
            c.annotations_stream_keys != a->config.annotations_stream_keys ||
            c.annotations_stream_clicks != a->config.annotations_stream_clicks;
        if (stream_annotations_changed) {
            frame_free(&a->stream_raw);
            media_stream_barrier(a->media);
        }
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
