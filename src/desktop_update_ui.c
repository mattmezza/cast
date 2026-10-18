#include "desktop_update_ui.h"
#include "app_internal.h"
#include "desktop_update.h"
#include "edition.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
static int launch_argc;
static char **launch_argv;
void cast_desktop_update_arguments(int argc, char **argv)
{
    launch_argc = argc;
    launch_argv = argv;
}
struct CastDesktopUpdater {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    Config config;
    CastDesktopUpdateProgress progress;
    CastDesktopUpdate candidate;
    bool stop;
    atomic_bool cancel;
    int request;
};
static int problem(char *e, size_t n, const char *message)
{
    snprintf(e, n, "%s", message);
    return -1;
}
static int executable(char *path, size_t n)
{
#ifdef __APPLE__
    uint32_t length = (uint32_t)n;
    char raw[PATH_MAX];
    if (_NSGetExecutablePath(raw, &length))
        return -1;
    return realpath(raw, path) ? 0 : -1;
#else
    ssize_t length = readlink("/proc/self/exe", path, n - 1);
    if (length <= 0 || (size_t)length >= n - 1)
        return -1;
    path[length] = 0;
    return 0;
#endif
}
static int gate_path(char *, size_t, char *, size_t);
static void recovery_message(char *message, size_t n, bool save)
{
    char path[PATH_MAX], error[CAST_ERR];
    if (gate_path(path, sizeof path, error, sizeof error))
        return;
    char *base = strrchr(path, '/');
    if (!base || (size_t)(base - path) + 24 >= sizeof path)
        return;
    strcpy(base + 1, "last-update-error.txt");
    if (save && !message[0]) {
        unlink(path);
        return;
    }
    int fd =
        open(path, (save ? O_WRONLY | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (fd < 0)
        return;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_nlink == 1 && !(st.st_mode & 0077)) {
        if (save) {
            if (ftruncate(fd, 0)) { close(fd); return; }
            (void)write(fd, message, strlen(message));
            (void)fsync(fd);
        } else {
            ssize_t length = read(fd, message, n - 1);
            if (length > 0)
                message[length] = 0;
        }
    }
    close(fd);
}
static void report(CastDesktopUpdater *u, CastDesktopUpdateState state, const char *version,
                   const char *message)
{
    pthread_mutex_lock(&u->mutex);
    u->progress.state = state;
    if (state == CAST_DESKTOP_CHECKING)
        u->progress.recovery_error = false;
    snprintf(u->progress.version, sizeof u->progress.version, "%s", version ? version : "");
    snprintf(u->progress.message, sizeof u->progress.message, "%s", message);
    pthread_mutex_unlock(&u->mutex);
}
static int eligible(const Config *config, const CastDesktopUpdate *update, char *e, size_t n)
{
    if (!cast_edition_identity()->pro)
        return 0;
    CastLicenseInfo license;
    if (cast_license_inspect(config->licensing_file, (int64_t)time(NULL), &license, e, n))
        return -1;
    if (license.state != CAST_LICENSE_VALID || !license.platform_supported ||
        (!license.perpetual && license.expires_at && update->released_at > license.expires_at) ||
        (license.updates_until && update->released_at > license.updates_until))
        return problem(e, n, "This Pro release is outside your license's update entitlement.");
    return 0;
}
static int private_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) && errno != EEXIST)
        return -1;
    return lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0022)
               ? -1
               : 0;
}
/* Linux loose downloads migrate to a complete private runtime. The original
 * executable becomes a launcher symlink; arbitrary parent folders are never swapped. */
static int destination(const char *binary, char *target, size_t n, char *e, size_t en)
{
#ifdef __APPLE__
    const char *tail = strstr(binary, ".app/Contents/MacOS/");
    if (!tail || (size_t)(tail + 4 - binary) >= n)
        return problem(e, en, "Install the .app in Applications before updating.");
    memcpy(target, binary, (size_t)(tail + 4 - binary));
    target[tail + 4 - binary] = 0;
    return 0;
#else
    struct stat st;
    if (lstat(binary, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        access(binary, W_OK) || !strncmp(binary, "/usr/", 5) || !strncmp(binary, "/opt/", 5))
        return problem(e, en,
                       "Use your package manager to update this installation. Desktop updates "
                       "require a user-owned download.");
    const char *home = getenv("HOME");
    if (!home || *home != '/')
        return problem(e, en, "An absolute home directory is required.");
    char path[PATH_MAX];
    const char *parts[] = {".local", ".local/share", ".local/share/cast",
                           ".local/share/cast/desktop"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i) {
        if (snprintf(path, sizeof path, "%s/%s", home, parts[i]) >= (int)sizeof path ||
            private_directory(path))
            return problem(e, en, "Cannot create an owned desktop update directory.");
    }
    if (snprintf(path, sizeof path, "%s/.local/share/cast/desktop/%s", home,
                 cast_edition_identity()->edition) >= (int)sizeof path ||
        private_directory(path))
        return problem(e, en, "Cannot create an owned desktop update directory.");
    if (snprintf(target, n, "%s/runtime", path) >= (int)n)
        return problem(e, en, "Update installation path is too long.");
    if (mkdir(target, 0700) && errno != EEXIST)
        return problem(e, en, "Cannot prepare the desktop runtime directory.");
    return 0;
#endif
}
static int daemon_action(const Config *config, const char *action, char *e, size_t n)
{
    const char *arguments[] = {"desktop-update", action};
    return app_runtime_client(config, 2, arguments, e, n);
}
static int launch_helper(const Config *config, const CastDesktopUpdateStage *stage,
                         const char *target, const char *binary, bool daemon, char *e, size_t n)
{
    int pipefd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd))
        return problem(e, n, "Cannot create the update handoff.");
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    char parent[32];
    snprintf(parent, sizeof parent, "%ld", (long)getpid());
    char *arguments[CAST_MAX_ARGS + 16];
    int count = 0;
    arguments[count++] = (char *)binary;
    arguments[count++] = "--desktop-update-helper";
    arguments[count++] = (char *)stage->directory;
    arguments[count++] = (char *)target;
    arguments[count++] = (char *)binary;
    arguments[count++] = (char *)config->socket_path;
    arguments[count++] = (char *)config->licensing_file;
    arguments[count++] = parent;
    arguments[count++] = "3";
    for (int i = 1; i < launch_argc && count < CAST_MAX_ARGS + 14; ++i) {
        if (!strcmp(launch_argv[i], "update"))
            break;
        /* An inherited configuration descriptor is consumed once by main. */
        if (!strcmp(launch_argv[i], "--panel-config-fd") ||
            !strcmp(launch_argv[i], "--internal-daemon-config")) {
            ++i;
            continue;
        }
        arguments[count++] = launch_argv[i];
    }
    arguments[count] = NULL;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[0], 3);
    if (pipefd[0] != 3)
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (pipefd[1] != 3)
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    pid_t child;
    int rc = posix_spawn(&child, binary, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[0]);
    if (rc) {
        close(pipefd[1]);
        return problem(e, n, "Cannot start the update installer.");
    }
    struct pollfd ready = {.fd = pipefd[1], .events = POLLIN};
    char acknowledged = 0;
    if (poll(&ready, 1, 35000) <= 0 || recv(pipefd[1], &acknowledged, 1, 0) != 1 ||
        acknowledged != 'R') {
        close(pipefd[1]);
        return problem(e, n, "Installer could not verify the staged update; app remains running.");
    }
    if (daemon && daemon_action(config, "commit", e, n)) {
        close(pipefd[1]);
        waitpid(child, NULL, 0);
        return -1;
    }
    char authorized = 'Y';
#ifdef __APPLE__
    int no_sigpipe = 1;
    setsockopt(pipefd[1], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
    ssize_t sent = send(pipefd[1], &authorized, 1, MSG_NOSIGNAL);
    close(pipefd[1]);
    if (sent != 1)
        return problem(e, n, "Update installer handoff failed.");
    return 0;
}
static int install_update(const Config *config, const CastDesktopUpdate *update, char *e, size_t n)
{
    if (eligible(config, update, e, n))
        return -1;
    char binary[PATH_MAX], target[PATH_MAX];
    if (executable(binary, sizeof binary) || destination(binary, target, sizeof target, e, n))
        return -1;
    /* Network, verification and extraction complete before reserving the daemon.
     * Recording is free to continue throughout download. */
    CastDesktopUpdateStage stage = {0};
    if (cast_update_stage(update, &stage, e, n))
        return -1;
    bool daemon = daemon_action(config, "prepare", e, n) == 0;
    if (!daemon) {
        /* A failed request must never be mistaken for an idle producer. Only an
         * uncontended, existing instance lock proves no daemon owns this session. */
        char lockpath[PATH_MAX];
        struct stat st;
        snprintf(lockpath, sizeof lockpath, "%s.lock", config->socket_path);
        int fd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        bool idle = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                    !(st.st_mode & 0077) && !flock(fd, LOCK_EX | LOCK_NB);
        if (fd >= 0)
            close(fd);
        if (!idle) {
            cast_update_stage_cleanup(&stage);
            return -1;
        }
    }
    if (launch_helper(config, &stage, target, binary, daemon, e, n)) {
        if (daemon) {
            char ignored[CAST_ERR];
            daemon_action(config, "cancel", ignored, sizeof ignored);
        }
        cast_update_stage_cleanup(&stage);
        return -1;
    }
    return 0;
}
static void *worker(void *context)
{
    CastDesktopUpdater *u = context;
    cast_update_cancel_scope(&u->cancel);
    pthread_mutex_lock(&u->mutex);
    time_t next_check = u->progress.recovery_error ? time(NULL) + 600 : 0;
    while (!u->stop) {
        if (!u->request && time(NULL) < next_check) {
            struct timespec deadline = {.tv_sec = next_check};
            pthread_cond_timedwait(&u->wake, &u->mutex, &deadline);
            continue;
        }
        int request = u->request ? u->request : 1;
        u->request = 0;
        pthread_mutex_unlock(&u->mutex);
        char error[CAST_ERR] = "";
        if (request == 1) {
            report(u, CAST_DESKTOP_CHECKING, NULL, "Checking for updates…");
            CastDesktopUpdate update;
            const CastEditionIdentity *id = cast_edition_identity();
            int result = cast_update_check(id->edition, id->platform, id->version, &update, error,
                                           sizeof error);
            if (!result) {
                u->candidate = update;
                if (eligible(&u->config, &update, error, sizeof error))
                    report(u, CAST_DESKTOP_INELIGIBLE, update.version, error);
                else
                    report(u, CAST_DESKTOP_AVAILABLE, update.version,
                           "An update is ready to download and install.");
            } else if (result == 1)
                report(u, CAST_DESKTOP_CURRENT, NULL, "You’re up to date.");
            else if (result == 2)
                report(u, CAST_DESKTOP_CURRENT, NULL,
                       "No update channel has been published for this platform yet.");
            else
                report(u, CAST_DESKTOP_FAILED, NULL, error);
            next_check = time(NULL) + 24 * 60 * 60;
        } else {
            report(u, CAST_DESKTOP_DOWNLOADING, u->candidate.version,
                   "Downloading and verifying the complete update…");
            if (install_update(&u->config, &u->candidate, error, sizeof error))
                report(u, CAST_DESKTOP_FAILED, u->candidate.version, error);
            else
                report(u, CAST_DESKTOP_RESTARTING, u->candidate.version,
                       "Restarting to install the update…");
        }
        pthread_mutex_lock(&u->mutex);
    }
    pthread_mutex_unlock(&u->mutex);
    cast_update_cancel_scope(NULL);
    return NULL;
}
CastDesktopUpdater *cast_desktop_updater_open(const Config *config)
{
    CastDesktopUpdater *u = calloc(1, sizeof *u);
    if (!u)
        return NULL;
    u->config = *config;
    recovery_message(u->progress.message, sizeof u->progress.message, false);
    if (u->progress.message[0]) {
        u->progress.state = CAST_DESKTOP_FAILED;
        u->progress.recovery_error = true;
    }
    atomic_init(&u->cancel, false);
    pthread_mutex_init(&u->mutex, NULL);
    pthread_cond_init(&u->wake, NULL);
    if (pthread_create(&u->thread, NULL, worker, u)) {
        pthread_mutex_destroy(&u->mutex);
        pthread_cond_destroy(&u->wake);
        free(u);
        return NULL;
    }
    return u;
}
void cast_desktop_updater_close(CastDesktopUpdater *u)
{
    if (!u)
        return;
    atomic_store(&u->cancel, true);
    pthread_mutex_lock(&u->mutex);
    u->stop = true;
    pthread_cond_signal(&u->wake);
    pthread_mutex_unlock(&u->mutex);
    pthread_join(u->thread, NULL);
    pthread_cond_destroy(&u->wake);
    pthread_mutex_destroy(&u->mutex);
    free(u);
}
void cast_desktop_updater_poll(CastDesktopUpdater *u, CastDesktopUpdateProgress *out)
{
    if (!u) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&u->mutex);
    *out = u->progress;
    pthread_mutex_unlock(&u->mutex);
}
static int request(CastDesktopUpdater *u, int action)
{
    if (!u)
        return -1;
    pthread_mutex_lock(&u->mutex);
    bool busy = u->request || u->progress.state == CAST_DESKTOP_CHECKING ||
                u->progress.state == CAST_DESKTOP_DOWNLOADING ||
                u->progress.state == CAST_DESKTOP_RESTARTING;
    if (!busy && (action == 1 || u->progress.state == CAST_DESKTOP_AVAILABLE)) {
        u->request = action;
        pthread_cond_signal(&u->wake);
    } else
        busy = true;
    pthread_mutex_unlock(&u->mutex);
    return busy ? -1 : 0;
}
int cast_desktop_updater_check(CastDesktopUpdater *u) { return request(u, 1); }
int cast_desktop_updater_install(CastDesktopUpdater *u) { return request(u, 2); }
int cast_desktop_update_cli(int argc, char **argv, const Config *config, char *e, size_t n)
{
    if (argc != 2)
        return problem(e, n, "Use update --check or update --install.");
    const CastEditionIdentity *id = cast_edition_identity();
    CastDesktopUpdate update;
    int result = cast_update_check(id->edition, id->platform, id->version, &update, e, n);
    if (result == 1) {
        puts("You’re up to date.");
        return 0;
    }
    if (result == 2) {
        puts("No update channel has been published for this platform yet.");
        return 0;
    }
    if (result)
        return 5;
    printf("%s %s is available.\n", id->pro ? "Super Screencast Pro" : "Super Screencast",
           update.version);
    if (eligible(config, &update, e, n))
        return 4;
    if (!strcmp(argv[1], "--check"))
        return 0;
    return install_update(config, &update, e, n) ? 5 : 0;
}

static int gate_path(char *path, size_t n, char *e, size_t en)
{
    const char *home = getenv("HOME");
    if (!home || home[0] != '/')
        return problem(e, en, "Absolute home directory required for update safety lock.");
    char directory[PATH_MAX];
    const char *parts[] = {".local", ".local/share", ".local/share/cast",
                           ".local/share/cast/desktop"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i) {
        if (snprintf(directory, sizeof directory, "%s/%s", home, parts[i]) >=
                (int)sizeof directory ||
            private_directory(directory))
            return problem(e, en, "Cannot create desktop update safety lock directory.");
    }
    if (snprintf(directory, sizeof directory, "%s/.local/share/cast/desktop/%s", home,
                 cast_edition_identity()->edition) >= (int)sizeof directory ||
        private_directory(directory) || snprintf(path, n, "%s/run.lock", directory) >= (int)n)
        return problem(e, en, "Cannot resolve desktop update safety lock.");
    return 0;
}
static int safety_lock(bool exclusive, char *e, size_t n)
{
    char path[PATH_MAX];
    if (gate_path(path, sizeof path, e, n))
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077) || st.st_nlink != 1 ||
        flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        if (fd >= 0)
            close(fd);
        return problem(e, n,
                       exclusive
                           ? "Another app session is still running; update was not installed."
                           : "An update is being installed; start the app again when it finishes.");
    }
    return fd;
}
int cast_desktop_update_runtime_lock(char *e, size_t n)
{
    /* Deliberately held for the entire producer lifetime, including finalization.
     * All custom socket sessions of this edition share this safety gate. */
    static int descriptor = -1;
    if (descriptor < 0)
        descriptor = safety_lock(false, e, n);
    return descriptor < 0 ? -1 : 0;
}
static int launch_new(const char *binary, int argc, char **argv)
{
    char **arguments = calloc((size_t)argc + 2, sizeof *arguments);
    if (!arguments)
        return -1;
    arguments[0] = (char *)binary;
    for (int i = 0; i < argc; ++i)
        arguments[i + 1] = argv[i];
    pid_t child;
    int rc = posix_spawn(&child, binary, NULL, NULL, arguments, environ);
    free(arguments);
    return rc ? -1 : 0;
}
static int preflight(const char *binary, const char *version, char *e, size_t n)
{
    int output[2];
    if (pipe(output))
        return problem(e, n, "Cannot test the updated executable.");
    fcntl(output[0], F_SETFD, FD_CLOEXEC);
    fcntl(output[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    if (output[1] > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&actions, output[1]);
    char *arguments[] = {(char *)binary, "--version", NULL};
    pid_t child;
    int rc = posix_spawn(&child, binary, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    if (rc) {
        close(output[0]);
        return problem(e, n, "The updated executable cannot start; existing app preserved.");
    }
    int status = 0;
    bool finished = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (waitpid(child, &status, WNOHANG) == child) {
            finished = true;
            break;
        }
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    if (!finished) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        close(output[0]);
        return problem(e, n, "The updated executable failed its startup check.");
    }
    char text[256] = {0};
    fcntl(output[0], F_SETFL, O_NONBLOCK);
    ssize_t length = read(output[0], text, sizeof text - 1);
    close(output[0]);
    char expected[128];
    snprintf(expected, sizeof expected, "%s %s\n",
             cast_edition_identity()->pro ? "cast-pro" : "cast", version);
    if (!WIFEXITED(status) || WEXITSTATUS(status) || length <= 0 || strcmp(text, expected))
        return problem(e, n, "The updated executable failed its version or library startup check.");
    return 0;
}
static void restore_previous(const char *target, const char *backup, char *e, size_t n)
{
    char failed[PATH_MAX];
    if (!backup[0] ||
        snprintf(failed, sizeof failed, "%s.failed-XXXXXX", target) >= (int)sizeof failed ||
        !mkdtemp(failed) || rmdir(failed) || rename(target, failed) || rename(backup, target)) {
        snprintf(e, n, "Update failed; recover the previous installation from %.350s", backup);
        return;
    }
    problem(e, n, "Updated app failed its installed startup check; previous version restored.");
}
int cast_desktop_update_helper(int argc, char **argv)
{
    /* Private invocation requires a live, inherited authorization pipe. The
     * helper authenticates staged bytes again, then waits for process/daemon exit. */
    if (argc < 8)
        return 2;
    char *end = NULL;
    long parent = strtol(argv[6], &end, 10);
    if (!end || *end || parent <= 1 || parent != getppid() || strcmp(argv[7], "3"))
        return 2;
    struct stat pipe_stat;
    if (fstat(3, &pipe_stat) || !S_ISSOCK(pipe_stat.st_mode))
        return 2;
    char error[CAST_ERR] = "";
    CastDesktopUpdateStage stage = {0};
    if (strlen(argv[1]) >= sizeof stage.directory)
        return 2;
    strcpy(stage.directory, argv[1]);
    const CastEditionIdentity *id = cast_edition_identity();
    CastDesktopUpdate update;
    Config config = {0};
    if (strlen(argv[5]) >= sizeof config.licensing_file)
        return 2;
    strcpy(config.licensing_file, argv[5]);
    if (cast_update_stage_verify(&stage, id->edition, id->platform, id->version, &update, error,
                                 sizeof error) ||
        eligible(&config, &update, error, sizeof error)) {
        close(3);
        fprintf(stderr, "Super Screencast update: %s\n", error);
        return 5;
    }
#ifdef __APPLE__
    int no_sigpipe = 1;
    setsockopt(3, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
    char staged_binary[PATH_MAX];
    if (snprintf(staged_binary, sizeof staged_binary, "%s/%s", stage.payload, update.binary) >=
            (int)sizeof staged_binary ||
        preflight(staged_binary, update.version, error, sizeof error)) {
        close(3);
        fprintf(stderr, "Super Screencast update: %s\n", error);
        return 5;
    }
    char ready = 'R';
    if (send(3, &ready, 1, MSG_NOSIGNAL) != 1) {
        close(3);
        return 5;
    }
    char authorized = 0;
    ssize_t got;
    do {
        got = read(3, &authorized, 1);
    } while (got < 0 && errno == EINTR);
    close(3);
    if (got != 1 || authorized != 'Y')
        return 5;
    /* Wait without signalling or killing any process. A timeout leaves the
     * installed app untouched. The parent's UI must exit voluntarily. */
    for (int attempt = 0; attempt < 300 && getppid() == parent; ++attempt) {
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    if (getppid() == parent) {
        problem(error, sizeof error, "App did not exit; update cancelled.");
        goto failed;
    }
    char lockpath[PATH_MAX];
    if (snprintf(lockpath, sizeof lockpath, "%s.lock", argv[4]) >= (int)sizeof lockpath)
        goto failed;
    int lock = open(lockpath, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077) || st.st_nlink != 1) {
        if (lock >= 0)
            close(lock);
        goto failed;
    }
    bool stopped = false;
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (!flock(lock, LOCK_EX | LOCK_NB)) {
            stopped = true;
            break;
        }
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    if (!stopped) {
        close(lock);
        problem(error, sizeof error, "Recording daemon is still running; update cancelled.");
        goto failed;
    }
    int gate = safety_lock(true, error, sizeof error);
    if (gate < 0) {
        close(lock);
        goto failed;
    }
    char backup_runtime[PATH_MAX] = "";
    if (cast_update_install_with_backup(stage.payload, argv[2], backup_runtime,
                                        sizeof backup_runtime, error, sizeof error)) {
        close(gate);
        close(lock);
        goto failed;
    }
    char restart[PATH_MAX];
    if (snprintf(restart, sizeof restart, "%s/%s", argv[2], update.binary) >= (int)sizeof restart ||
        preflight(restart, update.version, error, sizeof error)) {
        restore_previous(argv[2], backup_runtime, error, sizeof error);
        close(gate);
        close(lock);
        goto failed;
    }
#ifndef __APPLE__
    if (strcmp(argv[3], restart)) {
        /* Keep the original command path usable without replacing its parent.
         * A hard-link backup retains the original loose executable. */
        char backup[PATH_MAX] = "", replacement[PATH_MAX] = "";
        if (snprintf(backup, sizeof backup, "%s.before-desktop-update", argv[3]) >=
                (int)sizeof backup ||
            snprintf(replacement, sizeof replacement, "%s.update-%ld", argv[3], (long)getpid()) >=
                (int)sizeof replacement ||
            link(argv[3], backup) || symlink(restart, replacement) ||
            rename(replacement, argv[3])) {
            if (replacement[0])
                unlink(replacement);
            restore_previous(argv[2], backup_runtime, error, sizeof error);
            close(gate);
            close(lock);
            goto failed;
        }
    }
#endif
    close(gate);
    close(lock);
    if (launch_new(restart, argc - 8, argv + 8)) {
        problem(error, sizeof error,
                "Update installed, but automatic restart failed. Open the app again.");
        goto failed;
    }
    cast_update_stage_cleanup(&stage);
    char clear_error[1] = "";
    recovery_message(clear_error, sizeof clear_error, true);
    return 0;
failed:
    if (!error[0]) problem(error, sizeof error, "Update installation was cancelled safely.");
    fprintf(stderr, "Super Screencast update: %s\n", error);
    /* Preserve staged evidence and existing rollback directories for recovery.
     * Reopen the previous app on failure when still present. */
    recovery_message(error, sizeof error, true);
    if (getppid() != parent)
        launch_new(argv[3], argc - 8, argv + 8);
    return 5;
}
