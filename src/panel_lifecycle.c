#include "panel_lifecycle.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#define HANDOFF_FD 3
#define HANDOFF_SEALS (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)
typedef struct {
    char magic[8], version[32];
    uint32_t config_size;
    Config config;
} LaunchConfig;
struct PanelLifecycle {
    Config config;
    char **arguments;
    int argument_count, log_fd;
    off_t log_offset;
    pid_t child;
    uint64_t startup_deadline;
    bool stop_requested;
    PanelDaemonState state;
    int exit_status;
    char error[CAST_ERR], line[CAST_ERR], diagnostic[CAST_ERR];
    size_t line_used;
};
static int fail(char *error, size_t n, const char *message)
{
    if (error && n) {
        snprintf(error, n, "%s", message);
    }
    return -1;
}
static bool private_parent(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return false;
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
    return !lstat(parent, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 0077);
}
static bool existing_producer(const Config *config)
{
    struct stat st;
    if (lstat(config->socket_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof address.sun_path, "%s", config->socket_path);
    bool connected = connect(fd, (void *)&address, sizeof address) == 0;
    struct ucred peer;
    socklen_t bytes = sizeof peer;
    connected = connected && !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &bytes) &&
                peer.uid == getuid();
    close(fd);
    return connected;
}
static void diagnostic_read(PanelLifecycle *lifecycle)
{
    if (lifecycle->log_fd < 0) {
        return;
    }
    char bytes[2048];
    /* Limit each UI poll even if a faulty backend writes diagnostics continuously. */
    for (int chunk = 0; chunk < 4; chunk++) {
        ssize_t count = pread(lifecycle->log_fd, bytes, sizeof bytes, lifecycle->log_offset);
        if (count <= 0) {
            break;
        }
        lifecycle->log_offset += count;
        for (ssize_t i = 0; i < count; i++) {
            unsigned char byte = (unsigned char)bytes[i];
            if (byte == '\n') {
                lifecycle->line[lifecycle->line_used] = 0;
                if (lifecycle->line_used) {
                    snprintf(lifecycle->diagnostic, sizeof lifecycle->diagnostic, "%s",
                             lifecycle->line);
                }
                lifecycle->line_used = 0;
            } else if (byte >= 32 || byte == '\t') {
                if (lifecycle->line_used + 1 < sizeof lifecycle->line) {
                    lifecycle->line[lifecycle->line_used++] = (char)byte;
                }
            }
        }
    }
}
PanelLifecycle *panel_lifecycle_create(const Config *config, int argc, const char *const *argv,
                                       char *error, size_t n)
{
    if (!config || argc < 0 || argc > 128 || (argc && !argv)) {
        fail(error, n, "invalid application startup arguments");
        return NULL;
    }
    PanelLifecycle *lifecycle = calloc(1, sizeof *lifecycle);
    if (!lifecycle) {
        fail(error, n, "cannot allocate daemon lifecycle");
        return NULL;
    }
    lifecycle->log_fd = -1;
    lifecycle->config = *config;
    lifecycle->arguments = calloc((size_t)argc + 1, sizeof *lifecycle->arguments);
    if (!lifecycle->arguments) {
        goto failed;
    }
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || !(lifecycle->arguments[i] = strdup(argv[i]))) {
            goto failed;
        }
        lifecycle->argument_count++;
    }
    return lifecycle;
failed:
    fail(error, n, "cannot copy daemon startup arguments");
    panel_lifecycle_destroy(lifecycle);
    return NULL;
}
static int config_memfd(const Config *config, char *error, size_t n)
{
    LaunchConfig *blob = calloc(1, sizeof *blob);
    if (!blob) {
        return fail(error, n, "cannot allocate daemon configuration handoff");
    }
    memcpy(blob->magic, "CASTAPP1", 8);
    snprintf(blob->version, sizeof blob->version, "%s", CAST_VERSION);
    blob->config_size = sizeof(Config);
    blob->config = *config;
    int fd = memfd_create("cast-app-config", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0 && (ftruncate(fd, sizeof *blob) ||
                    pwrite(fd, blob, sizeof *blob, 0) != (ssize_t)sizeof *blob ||
                    fcntl(fd, F_ADD_SEALS, HANDOFF_SEALS))) {
        close(fd);
        fd = -1;
    }
    free(blob);
    if (fd < 0) {
        fail(error, n, "cannot create sealed daemon configuration handoff");
    }
    return fd;
}
int panel_lifecycle_start(PanelLifecycle *lifecycle, const Config *config, char *error, size_t n)
{
    if (!lifecycle || !config) {
        return fail(error, n, "daemon lifecycle is unavailable");
    }
    if (lifecycle->child > 0) {
        return lifecycle->state == PANEL_DAEMON_FAILED
                   ? fail(error, n, "daemon process still exists; stop it before retrying startup")
                   : 0;
    }
    if (config_validate(config, error, n)) {
        return -1;
    }
    if (!private_parent(config->socket_path)) {
        return fail(error, n, "daemon socket needs an owned private directory and a valid path");
    }
    lifecycle->config = *config;
    lifecycle->error[0] = lifecycle->diagnostic[0] = 0;
    lifecycle->line_used = 0;
    lifecycle->stop_requested = false;
    lifecycle->startup_deadline = cast_now_ns() + UINT64_C(10000000000);
    if (existing_producer(config)) {
        lifecycle->state = PANEL_DAEMON_STARTING;
        return 0;
    }
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof log_path, "%s.log", config->socket_path);
    int log =
        open(log_path, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (log < 0 || fstat(log, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_nlink != 1 || (st.st_mode & 0077)) {
        if (log >= 0) {
            close(log);
        }
        return fail(error, n, "cannot open private daemon log beside its socket");
    }
    int blob = config_memfd(config, error, n);
    int inherited = blob >= 0 ? fcntl(blob, F_DUPFD_CLOEXEC, HANDOFF_FD + 1) : -1;
    int diagnostics = fcntl(log, F_DUPFD_CLOEXEC, HANDOFF_FD + 1);
    if (blob < 0 || inherited < 0 || diagnostics < 0) {
        if (blob >= 0) {
            close(blob);
        }
        if (inherited >= 0) {
            close(inherited);
        }
        if (diagnostics >= 0) {
            close(diagnostics);
        }
        close(log);
        return fail(error, n, "cannot prepare daemon launch descriptors");
    }
    char **arguments = calloc((size_t)lifecycle->argument_count + 6, sizeof *arguments);
    if (!arguments) {
        close(blob);
        close(inherited);
        close(diagnostics);
        close(log);
        return fail(error, n, "cannot allocate daemon launch arguments");
    }
    arguments[0] = (char *)"cast";
    arguments[1] = (char *)"--internal-daemon-config";
    arguments[2] = (char *)"3";
    arguments[3] = (char *)"--headless";
    for (int i = 0; i < lifecycle->argument_count; i++) {
        arguments[i + 4] = lifecycle->arguments[i];
    }
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    bool initialized = result == 0;
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, inherited, HANDOFF_FD);
    }
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, diagnostics, STDERR_FILENO);
    }
    if (!result) {
        result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (!result) {
        result =
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (!result) {
        result = posix_spawn_file_actions_addclosefrom_np(&actions, HANDOFF_FD + 1);
    }
    pid_t child = 0;
    if (!result) {
        result = posix_spawn(&child, "/proc/self/exe", &actions, NULL, arguments, environ);
    }
    if (initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    free(arguments);
    close(blob);
    close(inherited);
    close(diagnostics);
    if (result) {
        close(log);
        if (error && n) {
            snprintf(error, n, "cannot start daemon: %s", strerror(result));
        }
        return -1;
    }
    if (lifecycle->log_fd >= 0) {
        close(lifecycle->log_fd);
    }
    lifecycle->log_fd = log;
    lifecycle->log_offset = st.st_size;
    lifecycle->child = child;
    lifecycle->state = PANEL_DAEMON_STARTING;
    lifecycle->exit_status = 0;
    return 0;
}
int panel_lifecycle_stop_owned(PanelLifecycle *lifecycle, char *error, size_t n)
{
    if (!lifecycle || lifecycle->child <= 0) {
        return 0;
    }
    if (kill(lifecycle->child, SIGTERM) && errno != ESRCH) {
        if (error && n) {
            snprintf(error, n, "cannot stop daemon: %s", strerror(errno));
        }
        return -1;
    }
    lifecycle->stop_requested = true;
    lifecycle->error[0] = 0;
    return 0;
}
void panel_lifecycle_poll(PanelLifecycle *lifecycle, bool connected,
                          PanelLifecycleSnapshot *snapshot)
{
    if (!lifecycle || !snapshot) {
        return;
    }
    diagnostic_read(lifecycle);
    if (lifecycle->child > 0) {
        int status;
        pid_t child = waitpid(lifecycle->child, &status, WNOHANG);
        if (child == lifecycle->child || (child < 0 && errno == ECHILD)) {
            lifecycle->child = 0;
            lifecycle->exit_status = child < 0           ? 0
                                     : WIFEXITED(status) ? WEXITSTATUS(status)
                                                         : 128 + WTERMSIG(status);
            if (lifecycle->exit_status && !lifecycle->stop_requested) {
                lifecycle->state = PANEL_DAEMON_FAILED;
                snprintf(lifecycle->error, sizeof lifecycle->error, "%s",
                         lifecycle->diagnostic[0] ? lifecycle->diagnostic
                                                  : "daemon exited before startup completed");
            } else {
                lifecycle->state = PANEL_DAEMON_STOPPED;
                lifecycle->error[0] = 0;
            }
        }
    }
    if (!connected && lifecycle->state == PANEL_DAEMON_STARTING &&
        cast_now_ns() >= lifecycle->startup_deadline && !lifecycle->stop_requested) {
        lifecycle->state = PANEL_DAEMON_FAILED;
        snprintf(lifecycle->error, sizeof lifecycle->error,
                 "daemon did not attach within 10 seconds; check capture consent or %.256s.log",
                 lifecycle->config.socket_path);
    }
    if (connected) {
        lifecycle->state = PANEL_DAEMON_RUNNING;
        lifecycle->error[0] = 0;
    } else if (!lifecycle->child && lifecycle->state == PANEL_DAEMON_RUNNING) {
        lifecycle->state = PANEL_DAEMON_STOPPED;
    }
    *snapshot = (PanelLifecycleSnapshot){.state = lifecycle->state,
                                         .child_pid = lifecycle->child,
                                         .owned = lifecycle->child > 0,
                                         .exit_status = lifecycle->exit_status};
    snprintf(snapshot->error, sizeof snapshot->error, "%s", lifecycle->error);
}
void panel_lifecycle_destroy(PanelLifecycle *lifecycle)
{
    if (!lifecycle) {
        return;
    }
    if (lifecycle->child > 0) {
        waitpid(lifecycle->child, NULL, WNOHANG);
    }
    if (lifecycle->log_fd >= 0) {
        close(lifecycle->log_fd);
    }
    for (int i = 0; i < lifecycle->argument_count; i++) {
        free(lifecycle->arguments[i]);
    }
    free(lifecycle->arguments);
    free(lifecycle);
}
int panel_lifecycle_config_fd(int fd, Config *config, char *error, size_t n)
{
    struct stat st;
    int result = -1;
    LaunchConfig *blob = malloc(sizeof *blob);
    int seals = fd >= 0 ? fcntl(fd, F_GET_SEALS) : -1;
    if (!config || !blob || seals < 0 || (seals & HANDOFF_SEALS) != HANDOFF_SEALS ||
        fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_size != (off_t)sizeof *blob ||
        pread(fd, blob, sizeof *blob, 0) != (ssize_t)sizeof *blob ||
        memcmp(blob->magic, "CASTAPP1", 8) || !memchr(blob->version, 0, sizeof blob->version) ||
        strcmp(blob->version, CAST_VERSION) || blob->config_size != sizeof(Config)) {
        fail(error, n, "invalid or incompatible sealed daemon configuration");
    } else if (!config_validate(&blob->config, error, n)) {
        *config = blob->config;
        result = 0;
    }
    free(blob);
    if (fd >= 0) {
        close(fd);
    }
    return result;
}
