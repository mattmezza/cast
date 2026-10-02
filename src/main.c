#include "app_internal.h"
#ifdef WITH_PANEL
#include "panel.h"
#endif
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
void app_request_stop(void)
{
    stopping = 1;
}
static void signal_stop(int signo)
{
    (void)signo;
    stopping = 1;
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
    "  pause; resume; live pause|resume|toggle|freeze|unfreeze; live message TEXT\n"
    "  record start [PATH]; record stop|pause|resume|toggle\n"
    "  audio list; audio mic|desktop|virtual on|off|toggle\n"
    "  audio mic|desktop source NAME; audio mic|desktop gain PERCENT%\n"
    "  preset NAME|next; preview on|off|toggle; preview target live|record\n"
    "  status [--json]; doctor; config check [PATH]; config defaults; config reload\n"
    "  settings SECTION.KEY VALUE [SECTION.KEY VALUE ...] (session only)\n"
    "  panel (optional native control panel)\n"
    "  reset; quit; --help; --version\n"
    "Live starts privacy-paused. Recording toggle only pauses/resumes an existing file.\n";

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
int app_apply_overrides(Config *c, const Startup *s, char *e, size_t n)
{
    for (int i = 0; i < s->override_count; i++) {
        const Option *o = s->override[i];
        char *p = (char *)c + o->offset;
        if (o->numeric) {
            if (app_integer(s->values[i], o->min, o->max, (int *)p, e, n)) {
                return -1;
            }
        } else if (app_copy_string(p, o->size, s->values[i], e, n)) {
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
            return app_error(e, n, "XDG_CONFIG_HOME path too long");
        }
    } else {
        if (xdg && *xdg) {
            fprintf(stderr, "cast: relative XDG_CONFIG_HOME ignored; using HOME/.config\n");
        }
        if (!home || home[0] != '/') {
            return app_error(e, n, "HOME must be absolute when XDG_CONFIG_HOME is unset/invalid");
        }
        if (snprintf(s->config_path, sizeof s->config_path, "%s/.config/cast/cast.conf", home) >=
            (int)sizeof s->config_path) {
            return app_error(e, n, "config path too long");
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
                return app_error(e, n, "--config requires a path");
            }
            s->explicit_config = true;
            if (app_copy_string(s->config_path, sizeof s->config_path, argv[i++], e, n)) {
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
                    return app_error(e, n, "%s requires a value", argv[i]);
                }
                if (s->override_count == 32) {
                    return app_error(e, n, "too many startup options");
                }
                s->override[s->override_count] = &options[j];
                s->values[s->override_count++] = argv[i + 1];
                i += 2;
                found = true;
                break;
            }
        }
        if (!found) {
            return app_error(e, n, "unknown option %s (cast --help)", argv[i]);
        }
    }
    return i;
}

int app_socket_path(Config *c, char *e, size_t n)
{
    if (c->socket_path[0]) {
        return 0;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        return app_error(
            e, n, "set an absolute XDG_RUNTIME_DIR or --socket PATH in an owned private directory");
    }
    struct stat st;
    if (lstat(runtime, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        return app_error(e, n, "XDG_RUNTIME_DIR must be an owned directory with mode 0700");
    }
    if (snprintf(c->socket_path, sizeof c->socket_path, "%s/cast.sock", runtime) >=
        (int)sizeof c->socket_path) {
        return app_error(e, n, "runtime path too long");
    }
    return 0;
}
static int validate_socket_parent(const char *path, char *e, size_t n)
{
    if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return app_error(e, n, "socket path must be absolute and shorter than %zu bytes",
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
        (st.st_mode & 0077)) {
        return app_error(e, n, "socket parent must be owned by you and private (mode 0700): %s",
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
        return app_error(e, n, "no owned user-only cast socket at %s; start cast first", path);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return app_error(e, n, "socket: %s", strerror(errno));
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, path);
    if (connect(fd, (void *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        return app_error(e, n, "connect %s: %s", path, strerror(errno));
    }
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, timeout) <= 0) {
        close(fd);
        return app_error(e, n, "daemon connection timeout");
    }
    int result = 0;
    socklen_t len = sizeof result;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &len) || result) {
        close(fd);
        return app_error(e, n, "daemon connection failed: %s", strerror(result ? result : errno));
    }
    struct ucred cred;
    len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
        close(fd);
        return app_error(e, n, "daemon peer ownership mismatch");
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
    if ((packet[0] != '0' && packet[0] != '1') || packet[1] != ' ') {
        return fprintf(stderr, "cast: invalid daemon response header\n"), 1;
    }
    bool ok = packet[0] == '0';
    FILE *out = ok ? stdout : stderr;
    fprintf(out, "%s\n", packet + 2);
    return ok ? 0 : 1;
}

static void remember_error(App *a, const char *e)
{
    if (e && *e && strcmp(a->state.last_error, e)) {
        snprintf(a->state.last_error, sizeof a->state.last_error, "%.1023s", e);
        fprintf(stderr, "cast: %s\n", e);
    }
}
void app_sync_source(App *a)
{
    uint64_t generation = platform_source_generation(a->platform);
    if (generation == a->source_generation) {
        return;
    }
    a->source_generation = generation;
    a->config.zoom_factor = 1;
    platform_events(a->platform, a->compositor, &a->config, true);
    compositor_clear(a->compositor);
    panel_transport_barrier(a->panel, a, true);
    media_barrier(a->media);
    frame_free(&a->live);
    frame_free(&a->record);
}
int app_shutdown_privacy(App *a, char *e, size_t n)
{
    a->state.live_paused = true;
    a->state.live_frozen = false;
    a->state.group_live_restore = false;
    a->countdown = false;
    a->countdown_path[0] = 0;
    frame_free(&a->live);
    frame_free(&a->frozen);
    if (a->platform && a->compositor) {
        platform_events(a->platform, a->compositor, &a->config, true);
        compositor_clear(a->compositor);
    }
    panel_transport_barrier(a->panel, a, true);
    int result = 0;
    if (a->media) {
        /* media_live silences virtual audio without dropping accepted recorder jobs. */
        result = media_live(a->media, &a->neutral, true, e, n);
    }
    if (a->platform && platform_capabilities(a->platform).preview) {
        a->config.preview = false;
        if (platform_preview(a->platform, &a->neutral, &a->state, &a->config, e, n)) {
            result = -1;
        }
    }
    return result;
}
static void tick(App *a)
{
    char e[CAST_ERR] = "";
    uint64_t now = cast_now_ns();
    bool running, paused;
    uint64_t dropped;
    media_status(a->media, &running, &paused, &dropped, e, sizeof e);
    bool finalizing = media_record_finalizing(a->media);
    if (a->record_finalizing && !finalizing) {
        char recording_error[CAST_ERR];
        media_record_error(a->media, recording_error, sizeof recording_error);
        fprintf(stderr, "cast: %s%s\n",
                recording_error[0] ? "finalization failed: " : "recording finalized: ",
                recording_error[0] ? recording_error : a->state.record_path);
    }
    a->record_finalizing = finalizing;
    a->state.dropped_frames = dropped + a->loop_drops;
    if (a->state.recording && !running) {
        a->state.recording = a->state.record_paused = a->state.group_record_restore = false;
    }
    remember_error(a, e);
    if (a->countdown && now >= a->countdown_deadline) {
        a->countdown = false;
        if (app_recording_start(a, a->countdown_path[0] ? a->countdown_path : NULL, e, sizeof e)) {
            remember_error(a, e);
        } else {
            fprintf(stderr, "cast: recording %s\n", a->state.record_path);
        }
    }
    bool input_private =
        (!a->config.live_enabled || a->state.live_paused || a->state.live_frozen) &&
        (!a->state.recording || a->state.record_paused);
    platform_events(a->platform, a->compositor, &a->config, input_private);
    bool capture_ok = platform_capture(a->platform, &a->screen, &a->cursor, e, sizeof e) == 0;
    /* Portal responses can commit a source during capture's event dispatch. */
    app_sync_source(a);
    if (!capture_ok) {
        remember_error(a, e);
    }
    bool camera_ok = a->config.camera_enabled;
    if (a->config.camera_enabled && media_camera(a->media, &a->camera, e, sizeof e)) {
        camera_ok = false;
        remember_error(a, e);
    }
    bool need_live = a->config.live_enabled && !a->state.live_paused && !a->state.live_frozen;
    bool need_record = a->state.recording && !a->state.record_paused;
    bool shared_composition =
        need_live && need_record &&
        (!a->config.keys || a->config.annotations_live_keys == a->config.annotations_record_keys) &&
        (!a->config.clicks ||
         a->config.annotations_live_clicks == a->config.annotations_record_clicks);
    bool composition_ok = capture_ok || !strcmp(a->config.layout, "camera");
    if ((need_live || need_record) && composition_ok) {
        if (need_live &&
            compositor_render(a->compositor, &a->config, &a->screen, camera_ok ? &a->camera : NULL,
                              &a->cursor, false, &a->live, e, sizeof e)) {
            composition_ok = false;
            remember_error(a, e);
        }
        if (need_record && !shared_composition &&
            compositor_render(a->compositor, &a->config, &a->screen, camera_ok ? &a->camera : NULL,
                              &a->cursor, true, &a->record, e, sizeof e)) {
            composition_ok = false;
            remember_error(a, e);
        }
    }
    now = cast_now_ns();
    a->neutral.ts_ns = now;
    a->live.ts_ns = now;
    a->record.ts_ns = now;
    const Frame *live = &a->neutral, *record = &a->neutral;
    if (!a->state.live_paused) {
        if (a->state.live_frozen && a->frozen.data) {
            live = &a->frozen;
        } else if (composition_ok && a->live.data) {
            live = &a->live;
        }
    }
    if (composition_ok) {
        if (shared_composition && a->live.data) {
            /* Media consumers copy/convert synchronously; this shared frame stays owned by App. */
            record = &a->live;
        } else if (a->record.data) {
            record = &a->record;
        }
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
    panel_transport_publish(a->panel, a, live, need_record ? record : &a->neutral);
    if (a->config.preview || platform_capabilities(a->platform).preview) {
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
    pid_t pid;
} Peer;
static int decode_packet(char *packet, ssize_t size, int *argc, char **argv, char *e, size_t n)
{
    if (size < 7 || size > CAST_IPC_MAX || memcmp(packet, "CAST1\0", 6)) {
        return app_error(e, n, "invalid IPC header or size");
    }
    size_t offset = 6;
    *argc = 0;
    while (offset < (size_t)size) {
        if (*argc == CAST_MAX_ARGS) {
            return app_error(e, n, "too many arguments");
        }
        char *end = memchr(packet + offset, 0, (size_t)size - offset);
        bool empty_value =
            (*argc == 2 && !strcmp(argv[0], "live") && !strcmp(argv[1], "message")) ||
            (*argc >= 2 && !(*argc & 1) && !strcmp(argv[0], "settings"));
        if (!end || (end == packet + offset && !empty_value)) {
            return app_error(e, n, "empty or unterminated command argument");
        }
        argv[(*argc)++] = packet + offset;
        offset = (size_t)(end - packet) + 1;
    }
    return *argc ? 0 : app_error(e, n, "empty command");
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
            if (panel_transport_request(a->panel, a, p->fd, packet, size, getuid(), p->pid)) {
                memmove(peers, peers + 1, (size_t)(--*count) * sizeof *peers);
                continue;
            }
            int argc = 0;
            char *argv[CAST_MAX_ARGS];
            char *command = packet;
            if (size >= 8 && !memcmp(packet, "CASTG1\0", 8)) {
                uint64_t generation = 0;
                if (size >= 16) {
                    memcpy(&generation, packet + 8, sizeof generation);
                }
                panel_transport_check(a->panel, a);
                if (!panel_transport_authorize(a->panel, generation, p->pid)) {
                    reply_peer(p->fd, -1, "stale or unauthenticated panel generation");
                    memmove(peers, peers + 1, (size_t)(--*count) * sizeof *peers);
                    continue;
                }
                command += 16;
                size -= 16;
            }
            rc = decode_packet(command, size, &argc, argv, out, sizeof out);
            if (!rc) {
                rc = app_command(a, argc, argv, out, sizeof out);
                if (rc) {
                    remember_error(a, out);
                }
                panel_transport_barrier(a->panel, a, false);
            }
        }
        reply_peer(p->fd, rc, out);
        memmove(peers, peers + 1, (size_t)(--*count) * sizeof *peers);
        if (stopping) {
            return;
        }
    }
}
static int daemon_socket(const Config *c, int *lockfd, char *e, size_t n)
{
    if (validate_socket_parent(c->socket_path, e, n)) {
        return -1;
    }
    char lock[PATH_MAX];
    if (snprintf(lock, sizeof lock, "%s.lock", c->socket_path) >= (int)sizeof lock) {
        return app_error(e, n, "lock path too long");
    }
    *lockfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (*lockfd < 0) {
        return app_error(e, n, "open instance lock: %s", strerror(errno));
    }
    struct stat st;
    if (fstat(*lockfd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077)) {
        return app_error(e, n, "unsafe instance lock ownership/type/permissions");
    }
    if (flock(*lockfd, LOCK_EX | LOCK_NB)) {
        return app_error(e, n, "cast instance already running at %s", c->socket_path);
    }
    if (!lstat(c->socket_path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
            return app_error(e, n, "refusing to remove unowned/non-socket path %s", c->socket_path);
        }
        int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, c->socket_path);
        if (probe < 0) {
            return app_error(e, n, "socket probe: %s", strerror(errno));
        }
        int connected = connect(probe, (void *)&a, sizeof a), saved = errno;
        close(probe);
        if (!connected || saved == EINPROGRESS || saved == EAGAIN) {
            return app_error(e, n, "another producer owns %s", c->socket_path);
        }
        if (saved != ECONNREFUSED && saved != ENOENT) {
            return app_error(e, n, "cannot safely probe stale socket: %s", strerror(saved));
        }
        if (unlink(c->socket_path)) {
            return app_error(e, n, "remove stale socket: %s", strerror(errno));
        }
    } else if (errno != ENOENT) {
        return app_error(e, n, "inspect socket: %s", strerror(errno));
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return app_error(e, n, "socket: %s", strerror(errno));
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, c->socket_path);
    if (bind(fd, (void *)&a, sizeof a)) {
        close(fd);
        return app_error(e, n, "bind: %s", strerror(errno));
    }
    if (chmod(c->socket_path, 0600) || listen(fd, 16)) {
        int saved = errno;
        close(fd);
        unlink(c->socket_path);
        return app_error(e, n, "secure socket/listen: %s", strerror(saved));
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
    Peer peers[16];
    int count = 0;
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
    a->source_generation = platform_source_generation(a->platform);
    a->panel = panel_transport_create();
    uint64_t interval = 1000000000ULL / (unsigned)config.fps, next = cast_now_ns();
    while (!stopping) {
        uint64_t now = cast_now_ns();
        panel_transport_check(a->panel, a);
        if (now >= next) {
            tick(a);
            next += interval;
            if (next < now) {
                uint64_t missed = (now - next) / interval + 1;
                a->loop_drops += missed;
                a->state.dropped_frames += missed;
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
                peers[count++] = (Peer){
                    fd, cast_now_ns() + (uint64_t)a->config.ipc_timeout_ms * 1000000ULL, cred.pid};
            }
        }
        if (stopping) {
            break;
        }
        drain_peers(a, peers, &count);
    }
    if (app_shutdown_privacy(a, e, sizeof e)) {
        fprintf(stderr, "cast: shutdown privacy publication failed: %s\n", e);
    }
    if (a->state.recording) {
        if (media_record_stop(a->media, e, sizeof e)) {
            fprintf(stderr, "cast: finalization failed: %s\n", e);
        } else {
            fprintf(stderr, "cast: %s\n", e);
            a->record_finalizing = true;
        }
    }
    rc = 0;
    goto cleanup;
failed:
    fprintf(stderr, "cast: %s\n", e);
cleanup:
    for (int i = 0; i < count; i++) {
        reply_peer(peers[i].fd, -1, "daemon shutting down");
    }
    panel_transport_destroy(a->panel, a);
    a->panel = NULL;
    if (a->media) {
        /* Retry a busy virtual output briefly before releasing the producer. */
        uint64_t deadline = cast_now_ns() + UINT64_C(100000000);
        while (app_shutdown_privacy(a, e, sizeof e) && a->neutral.data &&
               cast_now_ns() < deadline) {
            struct timespec delay = {.tv_nsec = 5000000};
            nanosleep(&delay, NULL);
        }
        if (a->record_finalizing || media_record_finalizing(a->media)) {
            /* Controls have already stopped; shutdown waits for the durable trailer. */
            while (media_record_finalizing(a->media)) {
                struct timespec delay = {.tv_nsec = 20000000};
                nanosleep(&delay, NULL);
            }
            e[0] = 0;
            media_record_error(a->media, e, sizeof e);
            fprintf(stderr, "cast: %s%s\n",
                    e[0] ? "finalization failed: " : "recording finalized: ",
                    e[0] ? e : a->state.record_path);
        }
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
    if ((argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))) ||
        (argc > 2 && !strcmp(argv[argc - 1], "--help"))) {
        fputs(help, stdout);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("cast " CAST_VERSION);
        return 0;
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
    if (first < argc && !strcmp(argv[first], "config") && first + 1 < argc &&
        !strcmp(argv[first + 1], "check")) {
        if (argc - first > 3) {
            return fprintf(stderr, "cast: config check [PATH]\n"), 1;
        }
        if (argc - first != 3 && config_path(&startup, e, sizeof e)) {
            return fprintf(stderr, "cast: %s\n", e), 1;
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
    bool explicit_socket = false;
    for (int i = 0; i < startup.override_count; i++) {
        if (!strcmp(startup.override[i]->flag, "--socket")) {
            explicit_socket = true;
        }
    }
    bool remote_command = first < argc && strcmp(argv[first], "doctor");
    Config config;
    if (remote_command && explicit_socket) {
        /* Socket routing must survive a broken daemon configuration for status/reload. */
        config_defaults(&config);
    } else {
        if (config_path(&startup, e, sizeof e) ||
            config_load(&config, startup.config_path, startup.explicit_config, e, sizeof e)) {
            return fprintf(stderr, "cast: %s\n", e), 1;
        }
    }
    if (app_apply_overrides(&config, &startup, e, sizeof e)) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc && !strcmp(argv[first], "doctor")) {
        if (argc - first != 1) {
            return fprintf(stderr, "cast: doctor takes no arguments\n"), 1;
        }
        doctor(&config);
        return 0;
    }
    if (app_socket_path(&config, e, sizeof e)) {
        return fprintf(stderr, "cast: %s\n", e), 1;
    }
    if (first < argc) {
        if (!strcmp(argv[first], "panel")) {
            if (argc - first != 1) {
                return fprintf(stderr, "cast: panel takes no arguments\n"), 1;
            }
#ifdef WITH_PANEL
            int rc = panel_run(&config, e, sizeof e);
            if (rc) {
                fprintf(stderr, "cast: %s\n", e);
            }
            return rc ? 1 : 0;
#else
            return fprintf(stderr, "cast: panel support is disabled; rebuild with PANEL=1\n"), 1;
#endif
        }
        return client(&config, argc - first, argv + first);
    }
    return run_daemon(config, startup);
}
