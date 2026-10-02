#include "app_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define PANEL_PIXELS ((size_t)PANEL_PREVIEW_WIDTH * PANEL_PREVIEW_HEIGHT * 4)
#define PANEL_QUEUE 16
typedef struct {
    char magic[8];
    uint32_t version, snapshot_size;
    uint64_t window;
} AttachRequest;
typedef struct {
    char magic[8];
    uint32_t version, snapshot_size;
    uint64_t bytes, generation;
} AttachReply;
typedef struct {
    int width, height, stride;
    uint64_t timestamp;
} PreviewInfo;
typedef struct {
    _Atomic uint32_t sequence;
    uint32_t version, snapshot_size;
    uint64_t generation;
    PanelSnapshot snapshot;
    PreviewInfo frame[2];
    uint8_t pixels[2][PANEL_PIXELS];
} SharedPreview;
struct PanelTransport {
    int peer, writer, reader;
    SharedPreview *shared;
    uint64_t generation, frames, privacy_epoch;
    pid_t pid;
};
typedef struct {
    size_t size;
    char packet[CAST_IPC_MAX];
    uint64_t id;
} QueuedCommand;
struct PanelClient {
    Config config;
    uint64_t window;
    pthread_t worker;
    pthread_mutex_t mutex;
    bool stop;
    int peer, wake;
    SharedPreview *shared;
    PanelSnapshot snapshot;
    QueuedCommand queue[PANEL_QUEUE];
    unsigned head, count;
    uint64_t queued, completed, generation, last_frame[2];
    uint8_t *staging;
};

static void transport_detach(PanelTransport *t, App *a)
{
    if (t->peer >= 0) {
        close(t->peer);
        t->peer = -1;
        platform_panel_unregister(a->platform);
    }
    if (t->shared) {
        /* Revoke the old mapping's content, including padding, before releasing it. */
        atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_acq_rel);
        memset(t->shared->pixels, 0, sizeof t->shared->pixels);
        t->shared->snapshot.connected = false;
        memset(t->shared->frame, 0, sizeof t->shared->frame);
        atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_release);
        munmap(t->shared, sizeof *t->shared);
        t->shared = NULL;
    }
    if (t->writer >= 0) {
        close(t->writer);
        t->writer = -1;
    }
    if (t->reader >= 0) {
        close(t->reader);
        t->reader = -1;
    }
}
PanelTransport *panel_transport_create(void)
{
    PanelTransport *t = calloc(1, sizeof *t);
    if (t) {
        t->peer = t->writer = t->reader = -1;
        if (getrandom(&t->generation, sizeof t->generation, GRND_NONBLOCK) !=
            (ssize_t)sizeof t->generation) {
            t->generation = cast_now_ns() ^ ((uint64_t)getpid() << 32);
        }
        if (!t->generation) {
            t->generation = 1;
        }
    }
    return t;
}
void panel_transport_destroy(PanelTransport *t, App *a)
{
    if (t) {
        transport_detach(t, a);
        free(t);
    }
}
bool panel_transport_attached(const PanelTransport *t)
{
    return t && t->peer >= 0;
}
void panel_transport_check(PanelTransport *t, App *a)
{
    if (!t || t->peer < 0) {
        return;
    }
    char byte;
    ssize_t rc = recv(t->peer, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    /* The attachment channel has no subsequent requests. Any input is a protocol violation. */
    if (rc >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        transport_detach(t, a);
    }
}
static void snapshot_fill(PanelSnapshot *s, App *a, PanelTransport *t)
{
    memset(s, 0, sizeof *s);
    s->config = a->config;
    s->state = a->state;
    s->capabilities = platform_capabilities(a->platform);
    s->connected = true;
    s->countdown = a->countdown;
    s->finalizing = media_record_finalizing(a->media);
    s->duration_ns = media_record_duration(a->media);
    uint64_t now = cast_now_ns();
    s->countdown_remaining_ns =
        a->countdown && a->countdown_deadline > now ? a->countdown_deadline - now : 0;
    s->daemon_generation = t->generation;
    s->frame_sequence = ++t->frames;
    s->privacy_epoch = t->privacy_epoch;
    media_audio_status(a->media, s->audio_status, sizeof s->audio_status);
    platform_panel_status(a->platform, s->exclusion, sizeof s->exclusion);
}
static void preview_write(SharedPreview *s, int target, const Frame *f)
{
    PreviewInfo *info = &s->frame[target];
    if (!f || !f->data || f->width < 1 || f->height < 1 || f->stride < f->width * 4) {
        memset(info, 0, sizeof *info);
        memset(s->pixels[target], 0, PANEL_PIXELS);
        return;
    }
    int w = f->width, h = f->height;
    if (w > PANEL_PREVIEW_WIDTH) {
        h = (int)((int64_t)h * PANEL_PREVIEW_WIDTH / w);
        w = PANEL_PREVIEW_WIDTH;
    }
    if (h > PANEL_PREVIEW_HEIGHT) {
        w = (int)((int64_t)w * PANEL_PREVIEW_HEIGHT / h);
        h = PANEL_PREVIEW_HEIGHT;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    *info = (PreviewInfo){w, h, w * 4, f->ts_ns};
    for (int y = 0; y < h; y++) {
        const uint8_t *row = f->data + (size_t)((int64_t)y * f->height / h) * f->stride;
        uint8_t *dst = s->pixels[target] + (size_t)y * info->stride;
        for (int x = 0; x < w; x++) {
            memcpy(dst + x * 4, row + ((int64_t)x * f->width / w) * 4, 4);
        }
    }
    /* Old images cannot survive in unused rows after a geometry/privacy change. */
    size_t used = (size_t)info->stride * h;
    memset(s->pixels[target] + used, 0, PANEL_PIXELS - used);
}
void panel_transport_publish(PanelTransport *t, App *a, const Frame *live, const Frame *record)
{
    if (!t || !t->shared) {
        return;
    }
    panel_transport_check(t, a);
    if (!t->shared) {
        return;
    }
    if (!a->config.live_enabled || a->state.live_paused) {
        live = &a->neutral;
    }
    if (a->state.record_paused ||
        ((!a->state.recording || a->state.record_cut) && !a->countdown)) {
        record = &a->neutral;
    }
    atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_acq_rel);
    snapshot_fill(&t->shared->snapshot, a, t);
    preview_write(t->shared, 0, live);
    preview_write(t->shared, 1, record);
    atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_release);
}
void panel_transport_barrier(PanelTransport *t, App *a, bool invalidate)
{
    if (!t || !t->shared) {
        return;
    }
    panel_transport_check(t, a);
    if (!t->shared) {
        return;
    }
    atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_acq_rel);
    t->privacy_epoch++;
    snapshot_fill(&t->shared->snapshot, a, t);
    if (invalidate || !a->config.live_enabled || a->state.live_paused) {
        preview_write(t->shared, 0, &a->neutral);
    } else {
        /* The daemon has applied freeze, then blur before this acknowledged barrier. */
        preview_write(t->shared, 0, a->live.data ? &a->live : &a->neutral);
    }
    if (invalidate || a->state.record_paused ||
        ((!a->state.recording || a->state.record_cut) && !a->countdown)) {
        preview_write(t->shared, 1, &a->neutral);
    } else {
        preview_write(t->shared, 1, a->record.data ? &a->record : &a->neutral);
    }
    atomic_fetch_add_explicit(&t->shared->sequence, 1, memory_order_release);
}
static void attach_error(int fd, const char *error)
{
    char packet[CAST_ERR + 2];
    snprintf(packet, sizeof packet, "1 %s", error);
    send(fd, packet, strlen(packet), MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}
bool panel_transport_request(PanelTransport *t, App *a, int fd, const void *packet, ssize_t size,
                             uid_t uid, pid_t pid)
{
    if (size < 6 || memcmp(packet, "CASTP1", 6)) {
        return false;
    }
    char error[CAST_ERR];
    AttachRequest req;
    if (!t || size != (ssize_t)sizeof req) {
        attach_error(fd, "invalid panel protocol size");
        return true;
    }
    memcpy(&req, packet, sizeof req);
    if (memcmp(req.magic, "CASTP1\0", 8) || req.version != PANEL_PROTOCOL_VERSION ||
        req.snapshot_size != sizeof(PanelSnapshot) || uid != getuid()) {
        attach_error(fd, "panel protocol/build or ownership mismatch");
        return true;
    }
    panel_transport_check(t, a);
    if (t->peer >= 0) {
        attach_error(fd, "a control panel is already attached");
        return true;
    }
    /* The panel's display backend can differ from the daemon's capture backend. */
    uint64_t window = platform_capabilities(a->platform).panel_exclusion ? req.window : 0;
    if (platform_panel_register(a->platform, window, (int)pid, error, sizeof error)) {
        attach_error(fd, error);
        return true;
    }
    t->peer = fd;
    t->pid = pid;
    /* A replaced attachment must also reject its predecessor's queued commands. */
    if (!++t->generation) {
        t->generation = 1;
    }
    t->writer = memfd_create("cast-panel-preview", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (t->writer < 0 || ftruncate(t->writer, sizeof(SharedPreview))) {
        snprintf(error, sizeof error, "panel memfd: %s", strerror(errno));
        goto failed;
    }
    t->shared = mmap(NULL, sizeof(SharedPreview), PROT_READ | PROT_WRITE, MAP_SHARED, t->writer, 0);
    if (t->shared == MAP_FAILED) {
        t->shared = NULL;
        snprintf(error, sizeof error, "panel mmap: %s", strerror(errno));
        goto failed;
    }
    /* FUTURE_WRITE permits only this existing daemon mapping to keep writing. */
    if (fcntl(t->writer, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL)) {
        snprintf(error, sizeof error, "panel memfd seals: %s", strerror(errno));
        goto failed;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", t->writer);
    t->reader = open(path, O_RDONLY | O_CLOEXEC);
    if (t->reader < 0) {
        snprintf(error, sizeof error, "panel read-only descriptor: %s", strerror(errno));
        goto failed;
    }
    atomic_init(&t->shared->sequence, 0);
    t->shared->version = PANEL_PROTOCOL_VERSION;
    t->shared->snapshot_size = sizeof(PanelSnapshot);
    t->shared->generation = t->generation;
    /* Registration may invalidate capture; begin neutral until the next capture tick. */
    panel_transport_publish(t, a, &a->neutral, &a->neutral);
    AttachReply reply = {.magic = "CASTP1",
                         .version = PANEL_PROTOCOL_VERSION,
                         .snapshot_size = sizeof(PanelSnapshot),
                         .bytes = sizeof(SharedPreview),
                         .generation = t->generation};
    struct iovec iov = {&reply, sizeof reply};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr msg = {.msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = control.bytes,
                         .msg_controllen = sizeof control.bytes};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &t->reader, sizeof(int));
    if (sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof reply) {
        transport_detach(t, a);
    }
    return true;
failed:
    /* Detach owns fd; send the error before releasing registration and mapping. */
    char response[CAST_ERR + 2];
    snprintf(response, sizeof response, "1 %s", error);
    send(fd, response, strlen(response), MSG_NOSIGNAL | MSG_DONTWAIT);
    transport_detach(t, a);
    return true;
}

bool panel_transport_authorize(PanelTransport *t, uint64_t generation, pid_t pid)
{
    return t && t->peer >= 0 && t->generation == generation && t->pid == pid;
}
static int client_wait(PanelClient *, int, short, int);
static int connect_daemon(PanelClient *client, char *error, size_t n)
{
    const Config *c = &client->config;
    const char *path = c->socket_path;
    if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return app_error(error, n, "invalid daemon socket path");
    }
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        slash[1] = 0;
    } else {
        *slash = 0;
    }
    struct stat st;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077) || lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        return app_error(error, n, "daemon socket and parent must be owned and user-only");
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return app_error(error, n, "panel socket: %s", strerror(errno));
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    if (connect(fd, (void *)&address, sizeof address) && errno != EINPROGRESS) {
        goto failed;
    }
    if (client_wait(client, fd, POLLOUT, c->ipc_timeout_ms) <= 0) {
        errno = ETIMEDOUT;
        goto failed;
    }
    int result = 0;
    socklen_t size = sizeof result;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &size) || result) {
        if (result) {
            errno = result;
        }
        goto failed;
    }
    struct ucred credentials;
    size = sizeof credentials;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) ||
        credentials.uid != getuid()) {
        errno = EPERM;
        goto failed;
    }
    return fd;
failed:
    app_error(error, n, "panel connection: %s", strerror(errno));
    close(fd);
    return -1;
}
static void client_disconnect(PanelClient *c, const char *error)
{
    pthread_mutex_lock(&c->mutex);
    if (c->shared) {
        munmap(c->shared, sizeof(SharedPreview));
        c->shared = NULL;
    }
    if (c->peer >= 0) {
        close(c->peer);
        c->peer = -1;
    }
    c->snapshot.connected = false;
    c->last_frame[0] = c->last_frame[1] = 0;
    if (c->count || c->queued > c->completed) {
        c->completed = c->queued;
        c->snapshot.command_failed = true;
        snprintf(c->snapshot.last_reply, sizeof c->snapshot.last_reply,
                 "daemon disconnected; pending commands discarded; inspect state");
    }
    c->count = 0; /* Commands from an old daemon generation must never be replayed. */
    snprintf(c->snapshot.error, sizeof c->snapshot.error, "%s", error);
    pthread_mutex_unlock(&c->mutex);
}
static int client_wait(PanelClient *c, int fd, short events, int timeout)
{
    uint64_t deadline = cast_now_ns() + (uint64_t)timeout * UINT64_C(1000000);
    for (;;) {
        struct pollfd p[2] = {{fd, events, 0}, {c->wake, POLLIN, 0}};
        uint64_t now = cast_now_ns();
        int remaining = deadline > now ? (int)((deadline - now + 999999) / 1000000) : 0;
        int rc = poll(p, 2, remaining);
        if (rc <= 0) {
            return rc;
        }
        if (p[1].revents) {
            uint64_t value;
            ssize_t drained = read(c->wake, &value, sizeof value);
            (void)drained;
            pthread_mutex_lock(&c->mutex);
            bool stop = c->stop;
            pthread_mutex_unlock(&c->mutex);
            if (stop) {
                return -1;
            }
        }
        if (p[0].revents) {
            return p[0].revents;
        }
        if (cast_now_ns() >= deadline) {
            return 0;
        }
    }
}
static int client_attach(PanelClient *c, char *error, size_t n)
{
    int fd = connect_daemon(c, error, n);
    if (fd < 0) {
        return -1;
    }
    AttachRequest request = {.magic = "CASTP1",
                             .version = PANEL_PROTOCOL_VERSION,
                             .snapshot_size = sizeof(PanelSnapshot),
                             .window = c->window};
    if (send(fd, &request, sizeof request, MSG_NOSIGNAL) != (ssize_t)sizeof request) {
        app_error(error, n, "cannot send panel attachment");
        close(fd);
        return -1;
    }
    if (client_wait(c, fd, POLLIN, c->config.ipc_timeout_ms) <= 0) {
        app_error(error, n, "panel attachment timed out");
        close(fd);
        return -1;
    }
    char response[CAST_ERR + 2];
    struct iovec iov = {response, sizeof response};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * 4)];
    } control = {0};
    struct msghdr msg = {.msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = control.bytes,
                         .msg_controllen = sizeof control.bytes};
    ssize_t received = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_TRUNC);
    int mapping = -1, descriptors = 0;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS &&
            h->cmsg_len >= CMSG_LEN(0)) {
            size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int incoming;
                memcpy(&incoming, (char *)CMSG_DATA(h) + i * sizeof(int), sizeof incoming);
                if (!descriptors++) {
                    mapping = incoming;
                } else {
                    close(incoming);
                }
            }
        }
    }
    AttachReply reply;
    bool valid =
        received == sizeof reply && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && descriptors == 1;
    if (valid) {
        memcpy(&reply, response, sizeof reply);
        valid = !memcmp(reply.magic, "CASTP1\0", 8) && reply.version == PANEL_PROTOCOL_VERSION &&
                reply.snapshot_size == sizeof(PanelSnapshot) &&
                reply.bytes == sizeof(SharedPreview) && reply.generation;
    }
    struct stat st;
    int seals = mapping >= 0 ? fcntl(mapping, F_GET_SEALS) : -1;
    int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL;
    valid = valid && !fstat(mapping, &st) && st.st_size == sizeof(SharedPreview) &&
            (fcntl(mapping, F_GETFL) & O_ACCMODE) == O_RDONLY &&
            (seals & required_seals) == required_seals;
    SharedPreview *shared =
        valid ? mmap(NULL, sizeof(SharedPreview), PROT_READ, MAP_SHARED, mapping, 0) : MAP_FAILED;
    if (mapping >= 0) {
        close(mapping);
    }
    if (shared == MAP_FAILED || shared->version != PANEL_PROTOCOL_VERSION ||
        shared->snapshot_size != sizeof(PanelSnapshot) || shared->generation != reply.generation) {
        if (shared != MAP_FAILED) {
            munmap(shared, sizeof(SharedPreview));
        }
        if (received > 2 && received < (ssize_t)sizeof response && !memcmp(response, "1 ", 2)) {
            response[received] = 0;
            app_error(error, n, "%s", response + 2);
        } else {
            app_error(error, n, "invalid panel descriptor/protocol/build");
        }
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&c->mutex);
    c->peer = fd;
    c->shared = shared;
    c->generation = reply.generation;
    c->last_frame[0] = c->last_frame[1] = 0;
    pthread_mutex_unlock(&c->mutex);
    return 0;
}
static bool shared_snapshot(const SharedPreview *s, PanelSnapshot *snapshot)
{
    for (int retry = 0; retry < 3; retry++) {
        uint32_t before = atomic_load_explicit(&s->sequence, memory_order_acquire);
        if (before & 1) {
            continue;
        }
        memcpy(snapshot, &s->snapshot, sizeof *snapshot);
        atomic_thread_fence(memory_order_acquire);
        if (before == atomic_load_explicit(&s->sequence, memory_order_acquire)) {
            return true;
        }
    }
    return false;
}
static void client_refresh(PanelClient *c)
{
    PanelSnapshot snapshot;
    pthread_mutex_lock(&c->mutex);
    if (c->shared && shared_snapshot(c->shared, &snapshot) &&
        snapshot.daemon_generation == c->generation) {
        snapshot.command_queued = c->queued;
        snapshot.command_completed = c->completed;
        snapshot.command_failed = c->snapshot.command_failed;
        snprintf(snapshot.last_reply, sizeof snapshot.last_reply, "%s", c->snapshot.last_reply);
        c->snapshot = snapshot;
    }
    pthread_mutex_unlock(&c->mutex);
}
static int client_execute(PanelClient *c, const QueuedCommand *command, char *reply, size_t n)
{
    int fd = connect_daemon(c, reply, n);
    if (fd < 0) {
        return -1;
    }
    int rc = -1;
    if (send(fd, command->packet, command->size, MSG_NOSIGNAL) != (ssize_t)command->size) {
        app_error(reply, n, "cannot send command");
        goto done;
    }
    if (client_wait(c, fd, POLLIN, c->config.ipc_timeout_ms) <= 0) {
        app_error(reply, n, "command timed out; completion unknown; inspect state");
        goto done;
    }
    char packet[CAST_IPC_MAX];
    ssize_t size = recv(fd, packet, sizeof packet - 1, MSG_TRUNC);
    if (size < 2 || size >= (ssize_t)sizeof packet || (packet[0] != '0' && packet[0] != '1') ||
        packet[1] != ' ') {
        app_error(reply, n, "invalid command acknowledgement");
        goto done;
    }
    packet[size] = 0;
    snprintf(reply, n, "%.*s", (int)(n ? n - 1 : 0), packet + 2);
    rc = packet[0] == '0' ? 0 : -1;
done:
    close(fd);
    return rc;
}
static void *client_worker(void *data)
{
    PanelClient *c = data;
    uint64_t reconnect = 0;
    for (;;) {
        pthread_mutex_lock(&c->mutex);
        bool stop = c->stop;
        int peer = c->peer;
        pthread_mutex_unlock(&c->mutex);
        if (stop) {
            break;
        }
        if (peer < 0 && cast_now_ns() >= reconnect) {
            char error[CAST_ERR];
            if (client_attach(c, error, sizeof error)) {
                client_disconnect(c, error);
                reconnect = cast_now_ns() + UINT64_C(1000000000);
            }
        }
        pthread_mutex_lock(&c->mutex);
        peer = c->peer;
        QueuedCommand command;
        bool have_command = peer >= 0 && c->count;
        if (have_command) {
            command = c->queue[c->head];
            c->head = (c->head + 1) % PANEL_QUEUE;
            c->count--;
        }
        pthread_mutex_unlock(&c->mutex);
        if (have_command) {
            char reply[CAST_ERR];
            int result = client_execute(c, &command, reply, sizeof reply);
            pthread_mutex_lock(&c->mutex);
            c->completed = command.id;
            c->snapshot.command_failed = result != 0;
            snprintf(c->snapshot.last_reply, sizeof c->snapshot.last_reply, "%s", reply);
            pthread_mutex_unlock(&c->mutex);
        }
        if (client_wait(c, peer, POLLIN | POLLHUP, have_command ? 0 : 30) > 0) {
            client_disconnect(c, "daemon disconnected; previews cleared");
            reconnect = cast_now_ns() + UINT64_C(1000000000);
        } else {
            client_refresh(c);
        }
    }
    client_disconnect(c, "panel closed");
    return NULL;
}
PanelClient *panel_client_open(const Config *config, uint64_t window, char *error, size_t n)
{
    PanelClient *c = calloc(1, sizeof *c);
    if (!c) {
        app_error(error, n, "cannot allocate panel client");
        return NULL;
    }
    c->config = *config;
    c->window = window;
    c->peer = -1;
    c->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    c->staging = malloc(PANEL_PIXELS);
    snprintf(c->snapshot.error, sizeof c->snapshot.error, "connecting to daemon");
    int rc = pthread_mutex_init(&c->mutex, NULL);
    if (rc || !c->staging || c->wake < 0) {
        app_error(error, n, "cannot allocate panel synchronization");
        if (!rc) {
            pthread_mutex_destroy(&c->mutex);
        }
        free(c->staging);
        if (c->wake >= 0) {
            close(c->wake);
        }
        free(c);
        return NULL;
    }
    rc = pthread_create(&c->worker, NULL, client_worker, c);
    if (rc) {
        app_error(error, n, "cannot create panel worker: %s", strerror(rc));
        pthread_mutex_destroy(&c->mutex);
        free(c->staging);
        close(c->wake);
        free(c);
        return NULL;
    }
    return c;
}
void panel_client_close(PanelClient *c)
{
    if (c) {
        pthread_mutex_lock(&c->mutex);
        c->stop = true;
        if (c->peer >= 0) {
            shutdown(c->peer, SHUT_RDWR);
        }
        pthread_mutex_unlock(&c->mutex);
        uint64_t wake = 1;
        ssize_t notified = write(c->wake, &wake, sizeof wake);
        (void)notified;
        pthread_join(c->worker, NULL);
        pthread_mutex_destroy(&c->mutex);
        free(c->staging);
        close(c->wake);
        free(c);
    }
}
bool panel_client_snapshot(PanelClient *c, PanelSnapshot *snapshot)
{
    if (!c || pthread_mutex_trylock(&c->mutex)) {
        return false;
    }
    PanelSnapshot fresh;
    if (c->shared && shared_snapshot(c->shared, &fresh) &&
        fresh.daemon_generation == c->generation) {
        fresh.command_failed = c->snapshot.command_failed;
        snprintf(fresh.last_reply, sizeof fresh.last_reply, "%s", c->snapshot.last_reply);
        *snapshot = fresh;
    } else {
        *snapshot = c->snapshot;
    }
    struct pollfd peer = {c->peer, POLLIN | POLLHUP, 0};
    if (c->peer >= 0 && poll(&peer, 1, 0) > 0 && peer.revents) {
        snapshot->connected = false;
        snprintf(snapshot->error, sizeof snapshot->error, "daemon disconnected; previews cleared");
    }
    snapshot->command_queued = c->queued;
    snapshot->command_completed = c->completed;
    pthread_mutex_unlock(&c->mutex);
    return true;
}
int panel_client_frame(PanelClient *c, bool record, Frame *frame, char *error, size_t n)
{
    if (!c) {
        return app_error(error, n, "panel client unavailable");
    }
    if (pthread_mutex_trylock(&c->mutex)) {
        return 0;
    }
    int result = 0, target = record ? 1 : 0;
    SharedPreview *s = c->shared;
    struct pollfd p = {c->peer, POLLIN | POLLHUP, 0};
    if (!s || c->peer < 0 || (poll(&p, 1, 0) > 0 && p.revents)) {
        result = app_error(error, n, "daemon disconnected; clear preview");
        goto done;
    }
    for (int retry = 0; retry < 3; retry++) {
        uint32_t before = atomic_load_explicit(&s->sequence, memory_order_acquire);
        if (before & 1) {
            continue;
        }
        uint64_t sequence = s->snapshot.frame_sequence;
        if (sequence == c->last_frame[target]) {
            break;
        }
        PreviewInfo info = s->frame[target];
        if (s->generation != c->generation || !s->snapshot.connected || info.width < 1 ||
            info.width > PANEL_PREVIEW_WIDTH || info.height < 1 ||
            info.height > PANEL_PREVIEW_HEIGHT || info.stride != info.width * 4) {
            result = app_error(error, n, "invalid/stale panel frame");
            break;
        }
        size_t bytes = (size_t)info.stride * info.height;
        memcpy(c->staging, s->pixels[target], bytes);
        atomic_thread_fence(memory_order_acquire);
        if (before != atomic_load_explicit(&s->sequence, memory_order_acquire)) {
            continue;
        }
        Frame source = {c->staging, info.width, info.height, info.stride, info.timestamp};
        if (frame_copy(frame, &source)) {
            result = app_error(error, n, "cannot allocate panel frame");
            break;
        }
        c->last_frame[target] = sequence;
        result = 1;
        break;
    }
done:
    if (result < 0) {
        frame_free(frame);
    }
    pthread_mutex_unlock(&c->mutex);
    return result;
}
int panel_client_command(PanelClient *c, int argc, const char *const *argv, char *error, size_t n)
{
    if (!c || argc < 1 || argc > CAST_MAX_ARGS) {
        return app_error(error, n, "invalid panel command argument count");
    }
    QueuedCommand command = {.size = 22};
    memcpy(command.packet, "CASTG1\0", 8);
    memcpy(command.packet + 16, "CAST1\0", 6);
    for (int i = 0; i < argc; i++) {
        size_t size = strlen(argv[i]) + 1;
        if (size > sizeof command.packet - command.size) {
            return app_error(error, n, "panel command exceeds IPC limit");
        }
        memcpy(command.packet + command.size, argv[i], size);
        command.size += size;
    }
    if (pthread_mutex_trylock(&c->mutex)) {
        return app_error(error, n, "panel busy; retry");
    }
    if (!c->snapshot.connected || c->count == PANEL_QUEUE) {
        pthread_mutex_unlock(&c->mutex);
        return app_error(error, n, "daemon disconnected or command queue full");
    }
    command.id = ++c->queued;
    memcpy(command.packet + 8, &c->generation, sizeof c->generation);
    c->queue[(c->head + c->count) % PANEL_QUEUE] = command;
    c->count++;
    pthread_mutex_unlock(&c->mutex);
    uint64_t wake = 1;
    ssize_t notified = write(c->wake, &wake, sizeof wake);
    (void)notified;
    return 0;
}
int panel_client_setting(PanelClient *c, const char *key, const char *value, char *error, size_t n)
{
    const char *args[] = {"settings", key, value};
    return panel_client_command(c, 3, args, error, n);
}
