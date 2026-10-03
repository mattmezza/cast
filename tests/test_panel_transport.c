/* Exercise the real synthetic daemon, including its private panel attachment. */
#define main cast_panel_test_cli_entry
int cast_panel_test_cli_entry(int, char **);
#include "../src/main.c"
#undef main
#include <assert.h>
#include <libavutil/log.h>
#include <sys/mman.h>
#include <sys/wait.h>
#ifdef WITH_PANEL
int panel_run(const Config *config, char *error, size_t n)
{
    (void)config;
    (void)error;
    (void)n;
    assert(!"transport tests must not open a panel");
    return -1;
}
#endif

#define PREVIEW_BYTES ((size_t)PANEL_PREVIEW_WIDTH * PANEL_PREVIEW_HEIGHT * 4)
typedef struct {
    char magic[8];
    uint32_t version, snapshot_size;
    uint64_t window;
} TestAttach;
typedef struct {
    char magic[8];
    uint32_t version, snapshot_size;
    uint64_t bytes, generation;
} TestReply;
typedef struct {
    int width, height, stride;
    uint64_t timestamp;
} TestFrameInfo;
typedef struct {
    uint32_t sequence, version, snapshot_size;
    uint64_t generation;
    PanelSnapshot snapshot;
    TestFrameInfo frame[3];
    uint8_t pixels[3][PREVIEW_BYTES];
} TestShared;
static void delay(void)
{
    struct timespec ts = {.tv_nsec = 2000000};
    nanosleep(&ts, NULL);
}
static pid_t start_daemon(Config config, Startup startup)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        stopping = 0;
        _exit(run_daemon(config, startup));
    }
    uint64_t deadline = cast_now_ns() + UINT64_C(5000000000);
    char error[CAST_ERR];
    int fd;
    do {
        fd = connect_socket(config.socket_path, 100, error, sizeof error);
        if (fd >= 0) {
            close(fd);
            return pid;
        }
        delay();
    } while (cast_now_ns() < deadline);
    assert(!"synthetic daemon did not start");
    return pid;
}
static int send_command(const Config *config, uint64_t generation, int argc, const char **argv)
{
    char error[CAST_ERR], packet[CAST_IPC_MAX];
    int fd = connect_socket(config->socket_path, 1000, error, sizeof error);
    assert(fd >= 0);
    size_t size = 0;
    if (generation) {
        memcpy(packet, "CASTG1\0", 8);
        memcpy(packet + 8, &generation, sizeof generation);
        size = 16;
    }
    memcpy(packet + size, "CAST1\0", 6);
    size += 6;
    for (int i = 0; i < argc; i++) {
        size_t bytes = strlen(argv[i]) + 1;
        assert(size + bytes <= sizeof packet);
        memcpy(packet + size, argv[i], bytes);
        size += bytes;
    }
    assert(send(fd, packet, size, MSG_NOSIGNAL) == (ssize_t)size);
    struct pollfd p = {fd, POLLIN, 0};
    assert(poll(&p, 1, 3000) > 0);
    ssize_t bytes = recv(fd, packet, sizeof packet - 1, MSG_TRUNC);
    assert(bytes >= 2 && bytes < (ssize_t)sizeof packet && packet[1] == ' ');
    close(fd);
    return packet[0] == '0' ? 0 : -1;
}
#define CMD(config, success, ...)                                                                  \
    do {                                                                                           \
        const char *args[] = {__VA_ARGS__};                                                        \
        assert((send_command(config, 0, sizeof args / sizeof args[0], args) == 0) == success);     \
    } while (0)
static int attach(const Config *config, int *descriptor, TestReply *reply)
{
    char error[CAST_ERR];
    int fd = connect_socket(config->socket_path, 1000, error, sizeof error);
    assert(fd >= 0);
    TestAttach request = {.magic = "CASTP1",
                          .version = PANEL_PROTOCOL_VERSION,
                          .snapshot_size = sizeof(PanelSnapshot)};
    assert(send(fd, &request, sizeof request, MSG_NOSIGNAL) == sizeof request);
    struct pollfd p = {fd, POLLIN, 0};
    assert(poll(&p, 1, 2000) > 0);
    char response[CAST_ERR];
    struct iovec iov = {response, sizeof response};
    union {
        struct cmsghdr align;
        char data[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr msg = {.msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = control.data,
                         .msg_controllen = sizeof control.data};
    ssize_t count = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    if (count != sizeof *reply) {
        assert(count > 2 && !memcmp(response, "1 ", 2));
        close(fd);
        return -1;
    }
    memcpy(reply, response, sizeof *reply);
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    assert(h && h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS);
    memcpy(descriptor, CMSG_DATA(h), sizeof *descriptor);
    return fd;
}
static void reject_attach(const Config *config, uint32_t version, uint32_t snapshot_size,
                          uint64_t window)
{
    char error[CAST_ERR];
    int fd = connect_socket(config->socket_path, 1000, error, sizeof error);
    assert(fd >= 0);
    TestAttach request = {
        .magic = "CASTP1", .version = version, .snapshot_size = snapshot_size, .window = window};
    assert(send(fd, &request, sizeof request, MSG_NOSIGNAL) == sizeof request);
    struct pollfd p = {fd, POLLIN, 0};
    assert(poll(&p, 1, 2000) > 0);
    assert(recv(fd, error, sizeof error, 0) > 2 && !memcmp(error, "1 ", 2));
    close(fd);
}
static void wait_raw(TestShared *shared, bool paused, bool recording)
{
    uint64_t deadline = cast_now_ns() + UINT64_C(3000000000);
    while ((!shared->snapshot.connected || shared->snapshot.state.virtual_paused != paused ||
            shared->snapshot.state.recording != recording || (shared->sequence & 1)) &&
           cast_now_ns() < deadline) {
        delay();
    }
    assert(shared->snapshot.connected && shared->snapshot.state.virtual_paused == paused &&
           shared->snapshot.state.recording == recording && !(shared->sequence & 1));
}
static PanelSnapshot wait_client(PanelClient *client, bool connected, uint64_t completed)
{
    uint64_t deadline = cast_now_ns() + UINT64_C(5000000000);
    PanelSnapshot snapshot = {0};
    while (cast_now_ns() < deadline) {
        if (panel_client_snapshot(client, &snapshot) && snapshot.connected == connected &&
            snapshot.command_completed >= completed) {
            return snapshot;
        }
        delay();
    }
    fprintf(stderr, "panel wait failed: connected=%d wanted=%d error=%s\n", snapshot.connected,
            connected, snapshot.error);
    assert(!"panel client wait timed out");
    return snapshot;
}
static void queue(PanelClient *client, int argc, const char **args)
{
    char error[CAST_ERR];
    uint64_t deadline = cast_now_ns() + UINT64_C(1000000000);
    while (panel_client_command(client, argc, args, error, sizeof error)) {
        assert(cast_now_ns() < deadline);
        delay();
    }
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    char directory[] = "/tmp/cast-panel-test-XXXXXX";
    assert(mkdtemp(directory));
    Config config;
    config_defaults(&config);
    strcpy(config.backend, "synthetic");
    strcpy(config.output_device, "none");
    config.camera_enabled = false;
    config.width = 800;
    config.height = 600;
    config.fps = 20;
    config.ipc_timeout_ms = 500;
    strcpy(config.video_codec, "ffv1");
    snprintf(config.socket_path, sizeof config.socket_path, "%s/cast.sock", directory);
    snprintf(config.record_dir, sizeof config.record_dir, "%s", directory);
    Startup startup = {0};
    pid_t daemon = start_daemon(config, startup);
    reject_attach(&config, PANEL_PROTOCOL_VERSION + 1, sizeof(PanelSnapshot), 0);
    reject_attach(&config, PANEL_PROTOCOL_VERSION, sizeof(PanelSnapshot) - 1, 0);
    int descriptor = -1;
    TestReply reply;
    int peer = attach(&config, &descriptor, &reply);
    assert(peer >= 0 && descriptor >= 0 && reply.bytes == sizeof(TestShared));
    assert((fcntl(descriptor, F_GETFL) & O_ACCMODE) == O_RDONLY);
    int seals = fcntl(descriptor, F_GET_SEALS);
    assert((seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL)) ==
           (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL));
    uint8_t bad = 0;
    assert(write(descriptor, &bad, 1) < 0);
    assert(ftruncate(descriptor, 0) < 0);
    assert(mmap(NULL, reply.bytes, PROT_WRITE, MAP_SHARED, descriptor, 0) == MAP_FAILED);
    char descriptor_path[64];
    snprintf(descriptor_path, sizeof descriptor_path, "/proc/self/fd/%d", descriptor);
    int reopened = open(descriptor_path, O_RDWR | O_CLOEXEC);
    assert(reopened >= 0);
    assert(write(reopened, &bad, 1) < 0 && errno == EPERM);
    assert(mmap(NULL, reply.bytes, PROT_WRITE, MAP_SHARED, reopened, 0) == MAP_FAILED);
    assert(ftruncate(reopened, 0) < 0);
    close(reopened);
    TestShared *shared = mmap(NULL, reply.bytes, PROT_READ, MAP_SHARED, descriptor, 0);
    assert(shared != MAP_FAILED);
    close(descriptor);
    wait_raw(shared, true, false);
    assert(shared->frame[0].width == 480 && shared->frame[0].height == 360);
    assert(shared->pixels[0][0] == 0x20 && shared->pixels[0][1] == 0x25);
    int duplicate_fd;
    TestReply duplicate;
    assert(attach(&config, &duplicate_fd, &duplicate) < 0);
    CMD(&config, true, "virtual", "message", "Sharing is paused");
    assert(!strcmp(shared->snapshot.config.pause_text, "Sharing is paused"));
    CMD(&config, true, "virtual", "message", "");
    assert(!shared->snapshot.config.pause_text[0]);
    uint64_t epoch = shared->snapshot.privacy_epoch;
    CMD(&config, true, "virtual", "resume");
    wait_raw(shared, false, false);
    uint64_t deadline = cast_now_ns() + UINT64_C(2000000000);
    while (shared->pixels[0][0] == 0x20 && cast_now_ns() < deadline) {
        delay();
    }
    assert(shared->pixels[0][0] != 0x20);
    CMD(&config, true, "virtual", "freeze");
    assert(shared->snapshot.state.virtual_frozen);
    uint8_t frozen[32];
    memcpy(frozen, shared->pixels[0], sizeof frozen);
    CMD(&config, true, "virtual", "message", "Frozen label unchanged");
    delay();
    assert(!memcmp(frozen, shared->pixels[0], sizeof frozen));
    CMD(&config, true, "virtual", "blur", "on");
    assert(shared->snapshot.state.virtual_frozen && shared->snapshot.state.virtual_blurred);
    assert(memcmp(frozen, shared->pixels[0], sizeof frozen));
    uint8_t blurred[32];
    memcpy(blurred, shared->pixels[0], sizeof blurred);
    delay();
    assert(!memcmp(blurred, shared->pixels[0], sizeof blurred));
    CMD(&config, true, "virtual", "blur", "off");
    assert(shared->snapshot.state.virtual_frozen && !shared->snapshot.state.virtual_blurred);
    assert(!memcmp(frozen, shared->pixels[0], sizeof frozen));
    CMD(&config, true, "virtual", "pause");
    /* Acknowledgement comes after both stale pixels and metadata have been replaced. */
    assert(shared->snapshot.state.virtual_paused && shared->snapshot.privacy_epoch > epoch);
    assert(shared->pixels[0][0] == 0x20);
    CMD(&config, true, "virtual", "unfreeze");
    assert(shared->snapshot.state.virtual_paused);
    CMD(&config, true, "virtual", "resume");
    char recording[PATH_MAX];
    snprintf(recording, sizeof recording, "%s/panel.mkv", directory);
    CMD(&config, true, "settings", "record.countdown", "3");
    CMD(&config, true, "record", "start", recording);
    assert(shared->snapshot.countdown && !shared->snapshot.state.recording);
    deadline = cast_now_ns() + UINT64_C(1000000000);
    while (shared->pixels[1][0] == 0x20 && cast_now_ns() < deadline) {
        delay();
    }
    assert(shared->snapshot.countdown && shared->pixels[1][0] != 0x20);
    CMD(&config, true, "record", "cancel");
    assert(!shared->snapshot.countdown && !shared->snapshot.state.recording &&
           shared->pixels[1][0] == 0x20);
    CMD(&config, true, "settings", "record.countdown", "0");
    CMD(&config, true, "record", "start", recording);
    wait_raw(shared, false, true);
    CMD(&config, true, "pause");
    assert(shared->snapshot.state.group_paused && shared->snapshot.state.record_paused);
    assert(shared->pixels[0][0] == 0x20 && shared->pixels[1][0] == 0x20);
    CMD(&config, true, "record", "pause");
    CMD(&config, true, "resume");
    assert(!shared->snapshot.state.virtual_paused && shared->snapshot.state.record_paused);
    CMD(&config, true, "record", "freeze");
    CMD(&config, true, "record", "blur", "on");
    assert(shared->snapshot.state.record_paused && shared->snapshot.state.record_frozen &&
           shared->snapshot.state.record_blurred);
    assert(shared->pixels[1][0] == 0x20);
    CMD(&config, true, "record", "cut");
    assert(shared->snapshot.state.record_cut && shared->pixels[1][0] == 0x20);
    CMD(&config, true, "settings", "record.countdown", "1");
    CMD(&config, true, "record", "resume");
    assert(shared->snapshot.countdown && shared->snapshot.state.record_cut);
    assert(shared->pixels[1][0] == 0x20); /* Solid pause still overrides countdown. */
    CMD(&config, true, "record", "cut");
    assert(!shared->snapshot.countdown && shared->snapshot.state.record_cut);
    assert(!strcmp(shared->snapshot.state.record_path, recording));
    CMD(&config, true, "record", "toggle");
    CMD(&config, true, "record", "unfreeze");
    CMD(&config, true, "record", "blur", "off");
    CMD(&config, true, "settings", "record.countdown", "3");
    CMD(&config, true, "record", "resume");
    deadline = cast_now_ns() + UINT64_C(1000000000);
    while (shared->pixels[1][0] == 0x20 && cast_now_ns() < deadline) {
        delay();
    }
    assert(shared->snapshot.countdown && shared->snapshot.state.record_cut &&
           shared->pixels[1][0] != 0x20);
    CMD(&config, true, "record", "cut");
    assert(!shared->snapshot.countdown && shared->snapshot.state.record_cut &&
           shared->pixels[1][0] == 0x20);
    CMD(&config, true, "record", "stop");
    CMD(&config, true, "preset", "coding");
    assert(!strcmp(shared->snapshot.current_preset, "coding"));
    CMD(&config, true, "settings", "camera.radius", "37", "camera.border_width", "5");
    assert(shared->snapshot.config.radius == 37 && shared->snapshot.config.border_width == 5);
    CMD(&config, false, "settings", "camera.radius", "90", "output.width", "640");
    assert(shared->snapshot.config.radius == 37);
    const char *virtual_resume[] = {"virtual", "resume"};
    assert(send_command(&config, reply.generation ^ 1, 2, virtual_resume) < 0);
    close(peer);
    deadline = cast_now_ns() + UINT64_C(1000000000);
    while (shared->snapshot.connected && cast_now_ns() < deadline) {
        delay();
    }
    assert(!shared->snapshot.connected && !shared->frame[0].width);
    assert(!shared->pixels[0][0] && !shared->pixels[1][PREVIEW_BYTES - 1]);
    assert(send_command(&config, reply.generation, 2, virtual_resume) < 0);
    munmap(shared, reply.bytes);

    char error[CAST_ERR];
    /* A synthetic/portal capture backend accepts an unrelated X11 UI window ID. */
    PanelClient *client = panel_client_open(&config, 1234, error, sizeof error);
    assert(client);
    PanelSnapshot snapshot = wait_client(client, true, 0);
    assert(snapshot.config.radius == 37 && snapshot.daemon_generation != reply.generation);
    uint64_t client_generation = snapshot.daemon_generation;
    assert(send_command(&config, reply.generation, 2, virtual_resume) < 0);
    assert(strstr(snapshot.exclusion, "unsupported"));
    const char *pause[] = {"virtual", "pause"};
    queue(client, 2, pause);
    snapshot = wait_client(client, true, 1);
    assert(snapshot.state.virtual_paused && !snapshot.command_failed);
    Frame frame = {0};
    deadline = cast_now_ns() + UINT64_C(1000000000);
    int got = 0;
    while (!got && cast_now_ns() < deadline) {
        got = panel_client_frame(client, false, &frame, error, sizeof error);
        delay();
    }
    assert(got == 1 && frame.width == 480 && frame.height == 360 && frame.data[0] == 0x20);
    int status;
    /* A stalled daemon cannot make the UI wait or grow its command queue. */
    assert(!kill(daemon, SIGSTOP));
    unsigned accepted = 0;
    deadline = cast_now_ns() + UINT64_C(1000000000);
    while (cast_now_ns() < deadline) {
        int result = panel_client_command(client, 2, pause, error, sizeof error);
        if (!result) {
            accepted++;
            assert(accepted <= 17); /* Sixteen queued plus at most one in flight. */
        } else if (strstr(error, "queue full")) {
            break;
        }
        delay();
    }
    assert(accepted >= 16 && accepted <= 17);
    assert(!kill(daemon, SIGKILL));
    assert(waitpid(daemon, &status, 0) == daemon && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGKILL);
    snapshot = wait_client(client, false, (uint64_t)accepted + 1);
    assert(snapshot.command_completed == snapshot.command_queued && snapshot.command_failed);
    assert(panel_client_frame(client, false, &frame, error, sizeof error) < 0);
    assert(!frame.data);
    daemon = start_daemon(config, startup);
    snapshot = wait_client(client, true, 1);
    assert(snapshot.daemon_generation != client_generation && snapshot.state.virtual_paused);
    assert(snapshot.config.radius != 37); /* Session edits were never persisted. */
    assert(send_command(&config, reply.generation, 2, virtual_resume) < 0);
    CMD(&config, true, "quit");
    assert(waitpid(daemon, &status, 0) == daemon && WIFEXITED(status) && !WEXITSTATUS(status));
    panel_client_close(client);
    frame_free(&frame);

    /* Closing a worker waiting for an unresponsive attachment cannot wait 30 seconds. */
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    assert(server >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, config.socket_path);
    assert(!bind(server, (void *)&address, sizeof address) && !chmod(config.socket_path, 0600) &&
           !listen(server, 4));
    config.ipc_timeout_ms = 30000;
    client = panel_client_open(&config, 0, error, sizeof error);
    assert(client);
    struct pollfd ready = {server, POLLIN, 0};
    assert(poll(&ready, 1, 2000) > 0);
    int stalled = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    assert(stalled >= 0);
    delay();
    uint64_t started = cast_now_ns();
    panel_client_close(client);
    assert(cast_now_ns() - started < UINT64_C(500000000));
    close(stalled);
    close(server);
    unlink(config.socket_path);

    /* The daemon side rejects mismatched credentials without platform access. */
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    PanelTransport *transport = panel_transport_create();
    TestAttach request = {.magic = "CASTP1",
                          .version = PANEL_PROTOCOL_VERSION,
                          .snapshot_size = sizeof(PanelSnapshot)};
    assert(panel_transport_request(transport, NULL, pair[0], &request, sizeof request, getuid() + 1,
                                   getpid()));
    char rejected[CAST_ERR];
    assert(recv(pair[1], rejected, sizeof rejected, 0) > 2 && !memcmp(rejected, "1 ", 2));
    close(pair[1]);
    panel_transport_destroy(transport, NULL);
    char lock[PATH_MAX + sizeof ".lock"];
    snprintf(lock, sizeof lock, "%s.lock", config.socket_path);
    unlink(lock);
    unlink(recording);
    rmdir(directory);
    puts("panel transport: ownership, sealed bounded frames, privacy barriers, freeze/group state, "
         "session settings, stale generations, reconnect and cancellable shutdown passed");
    return 0;
}
