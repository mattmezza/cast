#include "update.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern const unsigned char cast_install_script_start[];
extern const unsigned char cast_install_script_end[];

static int update_error(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (error && size) {
        vsnprintf(error, size, format, args);
    }
    va_end(args);
    return -1;
}

static int valid_tag(const char *tag)
{
    if (!tag || tag[0] != 'v' || strlen(tag) > 64) {
        return 0;
    }
    int dots = 0, digits = 0;
    for (const unsigned char *p = (const unsigned char *)tag + 1; *p; p++) {
        if (isdigit(*p)) {
            digits++;
        } else if (*p == '.' && digits) {
            dots++;
            digits = 0;
        } else {
            return 0;
        }
    }
    return digits && (dots == 1 || dots == 2);
}

int cast_update(int argc, char **argv, char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }
    if (argc < 1 || !argv || !argv[0] || strcmp(argv[0], "update")) {
        return update_error(error, error_size, "expected update [vMAJOR.MINOR[.PATCH]] [--download-only DIRECTORY]");
    }
    int version_seen = 0, directory_seen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            continue;
        }
        if (!strcmp(argv[i], "--download-only")) {
            if (directory_seen || ++i == argc || !argv[i][0]) {
                return update_error(error, error_size, "--download-only requires one directory");
            }
            directory_seen = 1;
        } else if (version_seen || !valid_tag(argv[i])) {
            return update_error(error, error_size, "version must be vMAJOR.MINOR or vMAJOR.MINOR.PATCH");
        } else {
            version_seen = 1;
        }
    }
    char **shell_argv = calloc((size_t)argc + 2, sizeof *shell_argv);
    if (!shell_argv) {
        return update_error(error, error_size, "preparing updater: %s", strerror(errno));
    }
    shell_argv[0] = "/bin/sh";
    for (int i = 1; i < argc; i++) {
        shell_argv[i + 1] = argv[i];
    }
    /* An anonymous seekable script avoids pipe deadlocks and checkout dependencies. */
    int fd = memfd_create("cast-installer", MFD_CLOEXEC);
    if (fd < 0) {
        free(shell_argv);
        return update_error(error, error_size, "opening embedded installer: %s", strerror(errno));
    }
    const unsigned char *script = cast_install_script_start;
    size_t remaining = (size_t)(cast_install_script_end - cast_install_script_start);
    while (remaining) {
        ssize_t count = write(fd, script, remaining);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            int saved = errno;
            close(fd);
            free(shell_argv);
            return update_error(error, error_size, "preparing embedded installer: %s", strerror(saved));
        }
        script += count;
        remaining -= (size_t)count;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        int saved = errno;
        close(fd);
        free(shell_argv);
        return update_error(error, error_size, "rewinding embedded installer: %s", strerror(saved));
    }
    char script_path[64];
    snprintf(script_path, sizeof script_path, "/proc/self/fd/%d", fd);
    shell_argv[1] = script_path;
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        /* Keep user stdin available for prompts; only this script descriptor survives. */
        if (fcntl(fd, F_SETFD, 0) < 0) {
            perror("cast update: installer descriptor");
            _exit(126);
        }
        execv(shell_argv[0], shell_argv);
        perror("cast update: launching /bin/sh");
        _exit(127);
    }
    int saved = errno;
    close(fd);
    free(shell_argv);
    if (child < 0) {
        return update_error(error, error_size, "launching installer: %s", strerror(saved));
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return update_error(error, error_size, "waiting for installer: %s", strerror(errno));
        }
    }
    int result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result) {
        update_error(error, error_size, "installer exited with status %d; see its output above", result);
    }
    return result;
}
