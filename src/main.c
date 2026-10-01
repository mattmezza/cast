#define _GNU_SOURCE
#include "cast.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void signal_stop(int signo)
{
    (void)signo;
    stopping = 1;
}
static int error(char *e, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e, n, fmt, ap);
    va_end(ap);
    return -1;
}
static int copy_string(char *dst, size_t n, const char *src, char *e, size_t en)
{
    if (strlen(src) >= n) {
        return error(e, en, "value is too long");
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
        return error(e, n, "invalid %s: expected %g..%g%s", s, min, max, percent ? "%" : "");
    }
    *v = d;
    return 0;
}
static int integer(const char *s, int min, int max, int *v, char *e, size_t n)
{
    double d;
    if (number(s, min, max, &d, false, e, n) || floor(d) != d) {
        return error(e, n, "%s must be an integer %d..%d", s, min, max);
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
        return error(e, n, "expected on|off|toggle");
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

static const char help[] =
    "cast " CAST_VERSION " — Linux presentation camera and recorder\n"
    "Usage: cast [startup options] | cast [--config PATH] [--socket PATH] COMMAND\n"
    "Startup: --backend xorg|wayland --output-device PATH --camera-device PATH\n"
    "  --width N --height N --fps N --no-live --no-camera --mic-source NAME\n"
    "  --desktop-source NAME --record-dir PATH --container NAME --video-codec NAME\n"
    "  --audio-codec NAME --countdown SECONDS --config PATH --socket PATH\n"
    "Test-only: --backend synthetic --camera-device synthetic --output-device none\n"
    "Commands (append --help for this reference):\n"
    "  layout overlay|split|screen|camera|next\n"
    "  split side left|right; split ratio PERCENT%\n"
    "  camera show|hide|toggle; camera size +5%|-5%|25%\n"
    "  camera move DX DY; camera position X Y\n"
    "  camera anchor top-left|top-right|bottom-left|bottom-right|next\n"
    "  camera shape rectangle|rounded|circle|next; camera aspect native|16:9|4:3|1:1\n"
    "  camera crop move DX DY; camera mirror on|off|toggle\n"
    "  camera list; camera device PATH\n"
    "  screen list|next; screen select NAME\n"
    "  capture monitor; capture region select|X Y W H; capture window select|active\n"
    "  capture fit contain|cover\n"
    "  zoom toggle|in|out|reset; zoom set FACTOR; zoom follow on|off\n"
    "  cursor on|off|toggle; cursor highlight on|off|toggle\n"
    "  clicks on|off|toggle; keys on|off|toggle; keys mode shortcuts|all; keys clear\n"
    "  annotations live|record keys|clicks on|off\n"
    "  pause; resume; live pause|resume|toggle|freeze|unfreeze\n"
    "  record start [PATH]; record stop|pause|resume|toggle\n"
    "  audio list; audio mic|desktop|virtual on|off|toggle\n"
    "  audio mic|desktop source NAME; audio mic|desktop gain PERCENT%\n"
    "  preset NAME|next; preview on|off|toggle; preview target live|record\n"
    "  status [--json]; doctor; config check [PATH]; config defaults; config reload\n"
    "  reset; quit; --help; --version\n"
    "Live starts privacy-paused. Recording toggle only pauses/resumes an existing file.\n";

typedef struct {
    const char *flag;
    size_t offset, size;
    bool numeric;
    int min, max;
} Option;
#define OS(flag, field) {flag, offsetof(Config, field), sizeof(((Config *)0)->field), false, 0, 0}
#define OI(flag, field, min, max) {flag, offsetof(Config, field), 0, true, min, max}
static const Option options[] = {OS("--backend", backend),
                                 OS("--output-device", output_device),
                                 OS("--camera-device", camera_device),
                                 OS("--socket", socket_path),
                                 OI("--width", width, 64, 7680),
                                 OI("--height", height, 64, 4320),
                                 OI("--fps", fps, 1, 120),
                                 OS("--mic-source", mic_source),
                                 OS("--desktop-source", desktop_source),
                                 OS("--record-dir", record_dir),
                                 OS("--container", record_container),
                                 OS("--video-codec", video_codec),
                                 OS("--audio-codec", audio_codec),
                                 OI("--countdown", record_countdown, 0, 60)};
#define NOPT (sizeof options / sizeof options[0])
typedef struct {
    char config_path[PATH_MAX];
    bool explicit_config;
    int override_count;
    const Option *override[32];
    const char *values[32];
    bool no_live, no_camera;
} Startup;
static int apply_overrides(Config *c, const Startup *s, char *e, size_t n)
{
    for (int i = 0; i < s->override_count; i++) {
        const Option *o = s->override[i];
        char *p = (char *)c + o->offset;
        if (o->numeric) {
            if (integer(s->values[i], o->min, o->max, (int *)p, e, n)) {
                return -1;
            }
        } else if (copy_string(p, o->size, s->values[i], e, n)) {
            return -1;
        }
    }
    if (s->no_live) {
        c->live_enabled = false;
    }
    if (s->no_camera) {
        c->camera_enabled = false;
    }
    return config_validate(c, e, n);
}
static int config_path(Startup *s, char *e, size_t n)
{
    if (s->explicit_config) {
        return 0;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg && xdg[0] == '/') {
        if (snprintf(s->config_path, sizeof s->config_path, "%s/cast/cast.conf", xdg) >=
            (int)sizeof s->config_path) {
            return error(e, n, "XDG_CONFIG_HOME path too long");
        }
    } else {
        if (xdg && *xdg) {
            fprintf(stderr, "cast: relative XDG_CONFIG_HOME ignored; using HOME/.config\n");
        }
        if (!home || home[0] != '/') {
            return error(e, n, "HOME must be absolute when XDG_CONFIG_HOME is unset/invalid");
        }
        if (snprintf(s->config_path, sizeof s->config_path, "%s/.config/cast/cast.conf", home) >=
            (int)sizeof s->config_path) {
            return error(e, n, "config path too long");
        }
    }
    return 0;
}
static int startup_parse(int argc, char **argv, Startup *s, char *e, size_t n)
{
    int i = 1;
    memset(s, 0, sizeof *s);
    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (!strcmp(argv[i], "--config")) {
            if (++i == argc) {
                return error(e, n, "--config requires a path");
            }
            s->explicit_config = true;
            if (copy_string(s->config_path, sizeof s->config_path, argv[i++], e, n)) {
                return -1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--no-live")) {
            s->no_live = true;
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--no-camera")) {
            s->no_camera = true;
            i++;
            continue;
        }
        bool found = false;
        for (size_t j = 0; j < NOPT; j++) {
            if (!strcmp(argv[i], options[j].flag)) {
                if (i + 1 == argc) {
                    return error(e, n, "%s requires a value", argv[i]);
                }
                if (s->override_count == 32) {
                    return error(e, n, "too many startup options");
                }
                s->override[s->override_count] = &options[j];
                s->values[s->override_count++] = argv[i + 1];
                i += 2;
                found = true;
                break;
            }
        }
        if (!found) {
            return error(e, n, "unknown option %s (cast --help)", argv[i]);
        }
    }
    return i;
}

static int socket_path(Config *c, char *e, size_t n)
{
    if (c->socket_path[0]) {
        return 0;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        return error(
            e, n, "set an absolute XDG_RUNTIME_DIR or --socket PATH in an owned private directory");
    }
    struct stat st;
    if (lstat(runtime, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0022)) {
        return error(e, n,
                     "XDG_RUNTIME_DIR must be an owned directory without group/other write access");
    }
    if (snprintf(c->socket_path, sizeof c->socket_path, "%s/cast.sock", runtime) >=
        (int)sizeof c->socket_path) {
        return error(e, n, "runtime path too long");
    }
    return 0;
}
static int validate_socket_parent(const char *path, char *e, size_t n)
{
    if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return error(e, n, "socket path must be absolute and shorter than %zu bytes",
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
        (st.st_mode & 0022)) {
        return error(e, n, "socket parent must be owned by you and not group/other writable: %s",
                     parent);
    }
    return 0;
}
static int connect_socket(const char *path, int timeout, char *e, size_t n)
{
    if (validate_socket_parent(path, e, n)) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
        return error(e, n, "no owned user-only cast socket at %s; start cast first", path);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return error(e, n, "socket: %s", strerror(errno));
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, path);
    if (connect(fd, (void *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        return error(e, n, "connect %s: %s", path, strerror(errno));
    }
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, timeout) <= 0) {
        close(fd);
        return error(e, n, "daemon connection timeout");
    }
    int result = 0;
    socklen_t len = sizeof result;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &len) || result) {
        close(fd);
        return error(e, n, "daemon connection failed: %s", strerror(result ? result : errno));
    }
    struct ucred cred;
    len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
        close(fd);
        return error(e, n, "daemon peer ownership mismatch");
    }
    return fd;
}
static int client(const Config *c, int argc, char **argv)
{
    char e[CAST_ERR], packet[CAST_IPC_MAX];
    if (argc > CAST_MAX_ARGS) {
        return fprintf(stderr, "cast: too many command arguments\n"), 1;
    }
    size_t size = 0;
    memcpy(packet, "CAST1\0", 6);
    size = 6;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (size + len > sizeof packet) {
            return fprintf(stderr, "cast: command exceeds IPC limit\n"), 1;
        }
        memcpy(packet + size, argv[i], len);
        size += len;
    }
    int fd = connect_socket(c->socket_path, c->ipc_timeout_ms, e, sizeof e);
    if (fd < 0) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (send(fd, packet, size, MSG_NOSIGNAL) != (ssize_t)size) {
        close(fd);
        return fprintf(stderr, "cast: cannot send command: %s\n", strerror(errno)), 1;
    }
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, c->ipc_timeout_ms) <= 0) {
        close(fd);
        return fprintf(stderr,
                       "cast: command timed out; its completion is unknown (check status)\n"),
               1;
    }
    ssize_t received = recv(fd, packet, sizeof packet - 1, MSG_TRUNC);
    close(fd);
    if (received < 2 || received >= (ssize_t)sizeof packet) {
        return fprintf(stderr, "cast: invalid daemon response\n"), 1;
    }
    packet[received] = 0;
    bool ok = packet[0] == '0';
    FILE *out = ok ? stdout : stderr;
    fprintf(out, "%s\n", packet + 2);
    return ok ? 0 : 1;
}

typedef struct {
    Config config, defaults;
    Startup startup;
    State state;
    Platform *platform;
    Compositor *compositor;
    Media *media;
    Frame screen, camera, live, record, neutral, frozen;
    Cursor cursor;
    double zoom_last;
    char current_preset[64];
    bool countdown;
    uint64_t countdown_deadline;
    char countdown_path[PATH_MAX];
} App;
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
static void status(App *a, bool json, char *out, size_t n)
{
    State *s = &a->state;
    Config *c = &a->config;
    Capabilities cap = platform_capabilities(a->platform);
    char path[PATH_MAX * 2], err[CAST_ERR * 2], source[512], audio[1024], backend[64];
    json_string(path, sizeof path, s->record_path);
    json_string(err, sizeof err, s->last_error);
    json_string(source, sizeof source, c->monitor);
    json_string(backend, sizeof backend, c->backend);
    media_audio_status(a->media, audio, sizeof audio);
    double seconds = media_record_duration(a->media) / 1e9;
    if (json) {
        snprintf(out, n,
                 "{\"version\":\"%s\",\"backend\":%s,\"capabilities\":{\"capture\":%s,\"cursor_"
                 "metadata\":%s,\"embedded_cursor\":%s,\"input\":%s,\"region_selection\":%s,"
                 "\"window_selection\":%s,\"preview\":%s},\"source\":{\"monitor\":%s,\"kind\":\"%"
                 "s\",\"region\":[%d,%d,%d,%d]},\"live\":{\"enabled\":%s,\"state\":\"%s\"},"
                 "\"record\":{\"state\":\"%s\",\"path\":%s,\"duration\":%.3f,\"countdown\":%s},"
                 "\"group_paused\":%s,\"layout\":\"%s\",\"zoom\":%.3f,\"camera_visible\":%s,"
                 "\"audio\":%s,\"last_error\":%s,\"dropped_frames\":%llu}",
                 CAST_VERSION, backend, cap.capture ? "true" : "false",
                 cap.cursor_metadata ? "true" : "false", cap.embedded_cursor ? "true" : "false",
                 cap.input ? "true" : "false", cap.region_selection ? "true" : "false",
                 cap.window_selection ? "true" : "false", cap.preview ? "true" : "false", source,
                 c->capture_kind, c->region_x, c->region_y, c->region_w, c->region_h,
                 c->live_enabled ? "true" : "false",
                 s->live_paused   ? "paused"
                 : s->live_frozen ? "frozen"
                                  : "live",
                 s->recording ? (s->record_paused ? "paused" : "recording") : "stopped", path,
                 seconds, a->countdown ? "true" : "false", s->group_paused ? "true" : "false",
                 c->layout, c->zoom_factor, c->camera_visible ? "true" : "false",
                 audio[0] ? audio : "{}", err, (unsigned long long)s->dropped_frames);
    } else {
        snprintf(out, n,
                 "backend=%s source=%s:%s layout=%s camera=%s zoom=%.2f\nlive=%s recording=%s "
                 "duration=%.3fs path=%s countdown=%s\naudio=%s\ndropped_frames=%llu error=%s",
                 c->backend, c->capture_kind, c->monitor[0] ? c->monitor : "selected", c->layout,
                 c->camera_visible ? "visible" : "hidden", c->zoom_factor,
                 s->live_paused   ? "PAUSED"
                 : s->live_frozen ? "FROZEN"
                                  : "LIVE",
                 s->recording ? (s->record_paused ? "RECORDING-PAUSED" : "RECORDING") : "stopped",
                 seconds, s->record_path, a->countdown ? "pending" : "off", audio,
                 (unsigned long long)s->dropped_frames, s->last_error);
    }
}
static int barrier(App *a, State *candidate, char *e, size_t n)
{
    if (a->state.recording && candidate->record_paused != a->state.record_paused) {
        if (media_record_pause(a->media, candidate->record_paused, e, n)) {
            return -1;
        }
    }
    bool was_frozen = a->state.live_frozen;
    a->state = *candidate;
    platform_events(a->platform, a->compositor, &a->config, true);
    compositor_clear(a->compositor);
    media_privacy(a->media, candidate->live_paused, candidate->live_frozen,
                  candidate->record_paused);
    if (candidate->live_paused) {
        compositor_clear(a->compositor);
        frame_copy(&a->frozen, &a->neutral);
        if (a->config.live_enabled && media_live(a->media, &a->neutral, true, e, n)) {
            return -1;
        }
    } else if (candidate->live_frozen && !was_frozen) {
        if (frame_copy(&a->frozen, a->live.data ? &a->live : &a->neutral)) {
            return error(e, n, "cannot allocate freeze frame");
        }
    }
    a->state = *candidate;
    return 0;
}
static int recording_start(App *a, const char *path, char *e, size_t n)
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
        return error(e, n, "backend does not support global keys/click observation");
    }
    if (c->zoom_follow && !cap.cursor_metadata) {
        return error(e, n, "backend has no separate cursor metadata; zoom follow is unsupported");
    }
    if (c->preview && !cap.preview) {
        return error(e, n, "backend preview unsupported");
    }
    if (restart_reasons(&a->config, c, a->state.recording, e, n)) {
        return -1;
    }
    /* Stage fallible platform acquisition first. Roll back if media acquisition fails. */
    if (platform_reconfigure(a->platform, c, e, n)) {
        return -1;
    }
    if (media_reconfigure(a->media, c, a->state.recording, e, n)) {
        char rollback[CAST_ERR];
        if (platform_reconfigure(a->platform, &a->config, rollback, sizeof rollback)) {
            snprintf(a->state.last_error, sizeof a->state.last_error, "reload rollback: %.900s",
                     rollback);
        }
        return -1;
    }
    a->config = *c;
    compositor_clear(a->compositor);
    compositor_neutral(c, &a->neutral);
    return 0;
}

#define IS(i, text) (ac > (i) && !strcmp(av[i], text))
#define ARITY(count)                                                                               \
    do {                                                                                           \
        if (ac != (count))                                                                         \
            return error(out, n, "invalid arguments; cast %s --help", av[0]);                      \
    } while (0)
#define ENUM(dst, value, choices)                                                                  \
    do {                                                                                           \
        if (!oneof(value, choices))                                                                \
            return error(out, n, "expected %s", choices);                                          \
        snprintf(dst, sizeof dst, "%s", value);                                                    \
    } while (0)

static int command_camera(App *a, Config *candidate, int ac, char **av, char *out, size_t n)
{
    Config c = *candidate;
    char e[CAST_ERR];

    if (ac < 2) {
        return error(out, n, "camera subcommand required");
    }
    if (IS(1, "list")) {
        ARITY(2);
        media_doctor(&c, out, n);
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
            return error(out, n, "camera size must be 1..100 percentage points");
        }
        int x, y, w, h;
        compositor_geometry(&c, a->camera.width ? a->camera.width : 640,
                            a->camera.height ? a->camera.height : 480, &x, &y, &w, &h, e, sizeof e);
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
        if (integer(av[2], -7680, 7680, &x, out, n) || integer(av[3], -4320, 4320, &y, out, n)) {
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
            return error(out, n, "camera crop move DX DY");
        }
        int x, y;
        if (integer(av[3], -16384, 16384, &x, out, n) ||
            integer(av[4], -16384, 16384, &y, out, n)) {
            return -1;
        }
        if (abs(c.crop_x + x) > 16384 || abs(c.crop_y + y) > 16384) {
            return error(out, n, "camera crop offset exceeds range");
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
        if (copy_string(c.camera_device, sizeof c.camera_device, av[2], out, n)) {
            return -1;
        }
        c.camera_enabled = true;
    } else {
        return error(out, n, "unknown camera command");
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
        return error(out, n, "zoom subcommand required");
    }
    if (IS(1, "set")) {
        ARITY(3);
        if (number(av[2], c.zoom_min, c.zoom_max, &c.zoom_factor, false, out, n)) {
            return -1;
        }
    } else if (IS(1, "follow")) {
        ARITY(3);
        if (!IS(2, "on") && !IS(2, "off")) {
            return error(out, n, "zoom follow on|off");
        }
        if (IS(2, "on") && !cap.cursor_metadata) {
            return error(out, n, "cursor-follow unsupported without cursor metadata");
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
            return error(out, n, "zoom toggle|in|out|reset|set FACTOR|follow on|off");
        }
    }
    if (c.zoom_factor > 1) {
        a->zoom_last = c.zoom_factor;
    }

    *candidate = c;
    return 0;
}

static int command_preset(App *a, Config *candidate, int ac, char **av, char *out, size_t n)
{
    Config c = *candidate;

    ARITY(2);
    char name[64];
    if (IS(1, "next")) {
        snprintf(name, sizeof name, "%s", a->current_preset);
        cycle(name, sizeof name, c.preset_order);
    } else if (copy_string(name, sizeof name, av[1], out, n)) {
        return -1;
    }
    Preset *p = NULL;
    for (int i = 0; i < c.preset_count; i++) {
        if (!strcmp(c.presets[i].name, name)) {
            p = &c.presets[i];
        }
    }
    if (!p) {
        return error(out, n, "unknown preset %s (configured in [preset.NAME])", name);
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
    snprintf(a->current_preset, sizeof a->current_preset, "%s", name);

    *candidate = c;
    return 0;
}

static int command(App *a, int ac, char **av, char *out, size_t n)
{
    Config c = a->config;
    State s = a->state;
    Capabilities cap = platform_capabilities(a->platform);
    int result = 0;
    bool change = false;
    out[0] = 0;
    if (IS(0, "status")) {
        if (ac != 1 && !(ac == 2 && IS(1, "--json"))) {
            return error(out, n, "status [--json]");
        }
        status(a, ac == 2, out, n);
        return 0;
    }
    if (IS(0, "quit")) {
        ARITY(1);
        stopping = 1;
        snprintf(out, n, "cast shutting down");
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
            return error(out, n, "record start [PATH]|stop|pause|resume|toggle");
        }
        if (IS(1, "start")) {
            if (ac != 2 && ac != 3) {
                return error(out, n, "record start [PATH]");
            }
            if (a->state.recording || a->countdown) {
                return error(out, n, "a recording or countdown already exists");
            }
            const char *path = ac == 3 ? av[2] : NULL;
            if (c.record_countdown) {
                if (path &&
                    copy_string(a->countdown_path, sizeof a->countdown_path, path, out, n)) {
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
            if (recording_start(a, path, out, n)) {
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
                return error(out, n, "no recording to stop");
            }
            int rc = media_record_stop(a->media, out, n);
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
            return error(out, n, "config reload (check/defaults run locally)");
        }
        Config fresh;
        if (config_load(&fresh, a->startup.config_path, a->startup.explicit_config, out, n) ||
            apply_overrides(&fresh, &a->startup, out, n) || socket_path(&fresh, out, n)) {
            return -1;
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
            return error(out, n, "split side left|right; split ratio PERCENT%%");
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
            if (!IS(0, "preview") && !(IS(0, "capture") && IS(1, "fit"))) {
                a->config.zoom_factor = 1;
                compositor_clear(a->compositor);
                media_barrier(a->media);
            }
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
                return error(out, n,
                             "cursor is embedded by capture backend; restart portal with cursor "
                             "policy to change visibility");
            }
            if (toggle(&c.cursor, av[1], out, n)) {
                return -1;
            }
        }
        if (!cap.cursor_metadata && !cap.embedded_cursor) {
            return error(out, n, "cursor metadata unsupported");
        }
        if (cap.embedded_cursor && ac == 3) {
            return error(out, n,
                         "cursor highlighting requires metadata; unsupported for embedded cursor");
        }
        change = true;
    } else if (IS(0, "clicks")) {
        ARITY(2);
        if (!cap.input) {
            return error(out, n, "global click observation unsupported on this backend");
        }
        if (toggle(&c.clicks, av[1], out, n)) {
            return -1;
        }
        change = true;
    } else if (IS(0, "keys")) {
        if (!cap.input) {
            return error(out, n, "global key observation unsupported on this backend");
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
            return error(out, n, "annotations live|record keys|clicks on|off");
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
        if (command_preset(a, &c, ac, av, out, n)) {
            return -1;
        }
        change = true;
    } else {
        return error(out, n, "unknown command %s; cast --help", av[0]);
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
        a->config = c;
        if (!c.keys || !c.clicks) {
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

static void remember_error(App *a, const char *e)
{
    if (e && *e && strcmp(a->state.last_error, e)) {
        snprintf(a->state.last_error, sizeof a->state.last_error, "%s", e);
        fprintf(stderr, "cast: %s\n", e);
    }
}
static void tick(App *a)
{
    char e[CAST_ERR] = "";
    uint64_t now = cast_now_ns();
    bool running, paused;
    uint64_t dropped;
    media_status(a->media, &running, &paused, &dropped, e, sizeof e);
    a->state.dropped_frames = dropped;
    if (a->state.recording && !running) {
        a->state.recording = a->state.record_paused = a->state.group_record_restore = false;
    }
    remember_error(a, e);
    if (a->countdown && now >= a->countdown_deadline) {
        a->countdown = false;
        if (recording_start(a, a->countdown_path[0] ? a->countdown_path : NULL, e, sizeof e)) {
            remember_error(a, e);
        } else {
            fprintf(stderr, "cast: recording %s\n", a->state.record_path);
        }
    }
    bool input_private = a->state.live_paused || (a->state.recording && a->state.record_paused);
    platform_events(a->platform, a->compositor, &a->config, input_private);
    bool capture_ok = platform_capture(a->platform, &a->screen, &a->cursor, e, sizeof e) == 0;
    if (!capture_ok) {
        remember_error(a, e);
    }
    bool camera_ok = true;
    if (a->config.camera_enabled && media_camera(a->media, &a->camera, e, sizeof e)) {
        camera_ok = false;
        remember_error(a, e);
    }
    bool need_live = !a->state.live_paused && !a->state.live_frozen;
    bool need_record = a->state.recording && !a->state.record_paused;
    if ((need_live || need_record) && capture_ok) {
        if (need_live &&
            compositor_render(a->compositor, &a->config, &a->screen, camera_ok ? &a->camera : NULL,
                              &a->cursor, false, &a->live, e, sizeof e)) {
            capture_ok = false;
            remember_error(a, e);
        }
        if (need_record &&
            compositor_render(a->compositor, &a->config, &a->screen, camera_ok ? &a->camera : NULL,
                              &a->cursor, true, &a->record, e, sizeof e)) {
            capture_ok = false;
            remember_error(a, e);
        }
    }
    a->neutral.ts_ns = now;
    const Frame *live = &a->neutral, *record = &a->neutral;
    if (!a->state.live_paused) {
        if (a->state.live_frozen && a->frozen.data) {
            live = &a->frozen;
        } else if (capture_ok && a->live.data) {
            live = &a->live;
        }
    }
    if (capture_ok && a->record.data) {
        record = &a->record;
    }
    if (a->config.live_enabled &&
        media_live(a->media, live, a->state.live_paused || a->state.live_frozen, e, sizeof e)) {
        remember_error(a, e);
    }
    if (need_record) {
        if (media_record_frame(a->media, record, e, sizeof e)) {
            remember_error(a, e);
        }
    }
    if (a->config.preview) {
        const Frame *target = !strcmp(a->config.preview_target, "record")
                                  ? (need_record ? record : &a->neutral)
                                  : live;
        if (platform_preview(a->platform, target, &a->state, &a->config, e, sizeof e)) {
            remember_error(a, e);
        }
    }
}

/* seqpacket preserves message boundaries; accepted peers are processed in FIFO order. */
typedef struct {
    int fd;
    uint64_t deadline;
} Peer;
static int decode_packet(char *packet, ssize_t size, int *argc, char **argv, char *e, size_t n)
{
    if (size < 7 || size > CAST_IPC_MAX || memcmp(packet, "CAST1\0", 6)) {
        return error(e, n, "invalid IPC header or size");
    }
    size_t offset = 6;
    *argc = 0;
    while (offset < (size_t)size) {
        if (*argc == CAST_MAX_ARGS) {
            return error(e, n, "too many arguments");
        }
        char *end = memchr(packet + offset, 0, (size_t)size - offset);
        if (!end || end == packet + offset) {
            return error(e, n, "empty or unterminated command argument");
        }
        argv[(*argc)++] = packet + offset;
        offset = (size_t)(end - packet) + 1;
    }
    return *argc ? 0 : error(e, n, "empty command");
}
static void reply_peer(int fd, int rc, const char *out)
{
    char response[CAST_IPC_MAX];
    response[0] = rc ? '1' : '0';
    response[1] = ' ';
    snprintf(response + 2, sizeof response - 2, "%s",
             out[0] ? out : (rc ? "command failed" : "ok"));
    send(fd, response, strlen(response), MSG_NOSIGNAL);
    close(fd);
}
static void drain_peers(App *a, Peer *peers, int *count)
{
    while (*count) {
        Peer *p = &peers[0];
        char packet[CAST_IPC_MAX], out[CAST_IPC_MAX - 16];
        ssize_t size = recv(p->fd, packet, sizeof packet, MSG_DONTWAIT | MSG_TRUNC);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && cast_now_ns() < p->deadline) {
            return;
        }
        int rc = -1;
        if (size < 0) {
            snprintf(out, sizeof out, "IPC receive timeout/error");
        } else {
            int argc = 0;
            char *argv[CAST_MAX_ARGS];
            rc = decode_packet(packet, size, &argc, argv, out, sizeof out);
            if (!rc) {
                rc = command(a, argc, argv, out, sizeof out);
            }
        }
        reply_peer(p->fd, rc, out);
        memmove(peers, peers + 1, (size_t)(--*count) * sizeof *peers);
    }
}
static int daemon_socket(const Config *c, int *lockfd, char *e, size_t n)
{
    if (validate_socket_parent(c->socket_path, e, n)) {
        return -1;
    }
    char lock[PATH_MAX];
    if (snprintf(lock, sizeof lock, "%s.lock", c->socket_path) >= (int)sizeof lock) {
        return error(e, n, "lock path too long");
    }
    *lockfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (*lockfd < 0) {
        return error(e, n, "open instance lock: %s", strerror(errno));
    }
    struct stat st;
    if (fstat(*lockfd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077)) {
        return error(e, n, "unsafe instance lock ownership/type/permissions");
    }
    if (flock(*lockfd, LOCK_EX | LOCK_NB)) {
        return error(e, n, "cast instance already running at %s", c->socket_path);
    }
    if (!lstat(c->socket_path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
            return error(e, n, "refusing to remove unowned/non-socket path %s", c->socket_path);
        }
        int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, c->socket_path);
        if (probe < 0) {
            return error(e, n, "socket probe: %s", strerror(errno));
        }
        int connected = connect(probe, (void *)&a, sizeof a), saved = errno;
        close(probe);
        if (!connected || saved == EINPROGRESS || saved == EAGAIN) {
            return error(e, n, "another producer owns %s", c->socket_path);
        }
        if (saved != ECONNREFUSED && saved != ENOENT) {
            return error(e, n, "cannot safely probe stale socket: %s", strerror(saved));
        }
        if (unlink(c->socket_path)) {
            return error(e, n, "remove stale socket: %s", strerror(errno));
        }
    } else if (errno != ENOENT) {
        return error(e, n, "inspect socket: %s", strerror(errno));
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return error(e, n, "socket: %s", strerror(errno));
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, c->socket_path);
    if (bind(fd, (void *)&a, sizeof a) || chmod(c->socket_path, 0600) || listen(fd, 16)) {
        close(fd);
        return error(e, n, "bind/listen: %s", strerror(errno));
    }
    return fd;
}
static void doctor(const Config *c)
{
    char report[CAST_IPC_MAX];
    puts("cast doctor (read-only; no setup commands executed)");
    platform_doctor(c, report, sizeof report);
    puts(report);
    media_doctor(c, report, sizeof report);
    puts(report);
    struct statvfs fs;
    if (access(c->record_dir, W_OK) || statvfs(c->record_dir, &fs)) {
        printf("Recording directory %s: %s; create an owned writable directory\n", c->record_dir,
               strerror(errno));
    } else {
        printf("Recording directory %s: writable, %.1f MiB available\n", c->record_dir,
               (double)fs.f_bavail * fs.f_frsize / (1024 * 1024));
    }
    printf("Config: INI validated; backend %s; output %dx%d @ %d fps\n", c->backend, c->width,
           c->height, c->fps);
}
static int run_daemon(Config config, Startup startup)
{
    char e[CAST_ERR];
    App *a = calloc(1, sizeof *a);
    if (!a) {
        return fprintf(stderr, "cast: out of memory\n"), 1;
    }
    a->config = a->defaults = config;
    a->startup = startup;
    a->state.live_paused = true;
    a->zoom_last = config.zoom_factor;
    a->config.zoom_factor = 1;
    int lockfd = -1, server = daemon_socket(&config, &lockfd, e, sizeof e);
    bool socket_owned = server >= 0;
    int rc = 1;
    if (server < 0) {
        goto failed;
    }
    struct sigaction act = {0};
    act.sa_handler = signal_stop;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    signal(SIGPIPE, SIG_IGN);
    a->compositor = compositor_create();
    if (!a->compositor) {
        snprintf(e, sizeof e, "cannot create compositor");
        goto failed;
    }
    a->platform = platform_open(&a->config, e, sizeof e);
    if (!a->platform) {
        goto failed;
    }
    Capabilities cap = platform_capabilities(a->platform);
    if (!cap.cursor_metadata && a->config.zoom_follow) {
        a->config.zoom_follow = false;
        a->defaults.zoom_follow = false;
        fprintf(stderr, "cast: backend lacks cursor metadata; cursor-follow disabled\n");
    }
    if ((a->config.keys || a->config.clicks) && !cap.input) {
        snprintf(e, sizeof e, "configured keys/clicks unsupported by backend");
        goto failed;
    }
    if (a->config.preview && !cap.preview) {
        snprintf(e, sizeof e, "configured preview unsupported by backend");
        goto failed;
    }
    a->media = media_open(&a->config, e, sizeof e);
    if (!a->media) {
        goto failed;
    }
    if (frame_alloc(&a->neutral, config.width, config.height)) {
        snprintf(e, sizeof e, "cannot allocate output canvas");
        goto failed;
    }
    compositor_neutral(&a->config, &a->neutral);
    media_privacy(a->media, true, false, false);
    if (config.live_enabled && media_live(a->media, &a->neutral, true, e, sizeof e)) {
        goto failed;
    }
    fprintf(stderr, "cast: %s backend; live privacy-paused; socket %s\n", config.backend,
            config.socket_path);
    Peer peers[16];
    int count = 0;
    uint64_t interval = 1000000000ULL / (unsigned)config.fps, next = cast_now_ns();
    while (!stopping) {
        uint64_t now = cast_now_ns();
        if (now >= next) {
            tick(a);
            next += interval;
            if (next < now) {
                a->state.dropped_frames += (now - next) / interval + 1;
                next = now + interval;
            }
        }
        struct pollfd pollers[17] = {{server, POLLIN, 0}};
        for (int i = 0; i < count; i++) {
            pollers[i + 1] = (struct pollfd){peers[i].fd, POLLIN, 0};
        }
        now = cast_now_ns();
        int timeout = next > now ? (int)((next - now + 999999) / 1000000) : 0;
        if (timeout > 50) {
            timeout = 50;
        }
        int ready = poll(pollers, (nfds_t)count + 1, timeout);
        if (ready < 0 && errno != EINTR) {
            snprintf(e, sizeof e, "poll: %s", strerror(errno));
            goto failed;
        }
        if (ready > 0 && (pollers[0].revents & POLLIN)) {
            int fd;
            while ((fd = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
                struct ucred cred;
                socklen_t len = sizeof cred;
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
                    reply_peer(fd, -1, "peer ownership mismatch");
                    continue;
                }
                if (count == 16) {
                    reply_peer(fd, -1, "control queue full; retry command");
                    continue;
                }
                peers[count++] =
                    (Peer){fd, cast_now_ns() + (uint64_t)config.ipc_timeout_ms * 1000000ULL};
            }
        }
        drain_peers(a, peers, &count);
    }
    for (int i = 0; i < count; i++) {
        reply_peer(peers[i].fd, -1, "daemon shutting down");
    }
    if (a->state.recording) {
        if (media_record_stop(a->media, e, sizeof e)) {
            fprintf(stderr, "cast: finalization failed: %s\n", e);
        } else {
            fprintf(stderr, "cast: %s\n", e);
        }
    }
    rc = 0;
    goto cleanup;
failed:
    fprintf(stderr, "cast: %s\n", e);
cleanup:
    if (a->media) {
        media_close(a->media);
    }
    if (a->platform) {
        platform_close(a->platform);
    }
    if (a->compositor) {
        compositor_destroy(a->compositor);
    }
    frame_free(&a->screen);
    frame_free(&a->camera);
    frame_free(&a->live);
    frame_free(&a->record);
    frame_free(&a->neutral);
    frame_free(&a->frozen);
    if (server >= 0) {
        close(server);
    }
    if (socket_owned) {
        unlink(config.socket_path);
    }
    if (lockfd >= 0) {
        close(lockfd);
    }
    free(a);
    return rc;
}
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "help")) {
            fputs(help, stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts("cast " CAST_VERSION);
            return 0;
        }
    }
    char e[CAST_ERR];
    Startup startup;
    int first = startup_parse(argc, argv, &startup, e, sizeof e);
    if (first < 0) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc && !strcmp(argv[first], "config") && first + 1 < argc &&
        !strcmp(argv[first + 1], "defaults")) {
        if (first + 2 != argc) {
            return fprintf(stderr, "cast: config defaults takes no arguments\n"), 1;
        }
        config_print_defaults();
        return 0;
    }
    if (config_path(&startup, e, sizeof e)) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc && !strcmp(argv[first], "config") && first + 1 < argc &&
        !strcmp(argv[first + 1], "check")) {
        if (argc - first > 3) {
            return fprintf(stderr, "cast: config check [PATH]\n"), 1;
        }
        const char *path = argc - first == 3 ? argv[first + 2] : startup.config_path;
        Config candidate;
        if (config_load(&candidate, path, argc - first == 3 || startup.explicit_config, e,
                        sizeof e)) {
            return fprintf(stderr, "cast: %s\n", e), 1;
        }
        printf("configuration valid: %s\n", path);
        return 0;
    }
    Config config;
    if (config_load(&config, startup.config_path, startup.explicit_config, e, sizeof e) ||
        apply_overrides(&config, &startup, e, sizeof e)) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc && !strcmp(argv[first], "doctor")) {
        if (argc - first != 1) {
            return fprintf(stderr, "cast: doctor takes no arguments\n"), 1;
        }
        doctor(&config);
        return 0;
    }
    if (socket_path(&config, e, sizeof e)) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc) {
        return client(&config, argc - first, argv + first);
    }
    return run_daemon(config, startup);
}
