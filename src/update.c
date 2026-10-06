#include "update.h"
#include "edition.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

static int community_update(int argc, char **argv, char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }
    if (argc < 1 || !argv || !argv[0] || strcmp(argv[0], "update")) {
        return update_error(error, error_size,
                            "expected update [vMAJOR.MINOR[.PATCH]] [--download-only DIRECTORY]");
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
            return update_error(error, error_size,
                                "version must be vMAJOR.MINOR or vMAJOR.MINOR.PATCH");
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
            return update_error(error, error_size, "preparing embedded installer: %s",
                                strerror(saved));
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
        update_error(error, error_size, "installer exited with status %d; see its output above",
                     result);
    }
    return result;
}

/* Offline Pro bundles contain exactly manifest.json and the signed cast-pro artifact.
 * Copy first, then authenticate the copied bytes. Source changes cannot race the hash. */
static int copy_artifact(const char *source, int destination, char *error, size_t size)
{
    int fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 256 * 1024 * 1024) {
        if (fd >= 0) {
            close(fd);
        }
        return update_error(error, size, "update artifact must be a regular file of 1–256 MiB");
    }
    unsigned char buffer[65536];
    off_t total = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(fd);
            return update_error(error, size, "reading artifact: %s", strerror(errno));
        }
        if (!count) {
            break;
        }
        total += count;
        if (total > st.st_size) {
            close(fd);
            return update_error(error, size, "artifact changed during copy");
        }
        for (ssize_t used = 0; used < count;) {
            ssize_t written = write(destination, buffer + used, (size_t)(count - used));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                close(fd);
                return update_error(error, size, "staging artifact: %s", strerror(errno));
            }
            used += written;
        }
    }
    close(fd);
    if (total != st.st_size || fsync(destination)) {
        return update_error(error, size, "artifact changed or could not be synced");
    }
    return 0;
}

static int read_manifest(const char *path, unsigned char *data, size_t *length, char *error,
                         size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > CAST_LICENSE_MAX) {
        if (fd >= 0) {
            close(fd);
        }
        return update_error(error, size, "manifest must be a bounded regular file");
    }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t count = read(fd, data + used, (size_t)st.st_size - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return update_error(error, size, "reading manifest failed");
        }
        used += (size_t)count;
    }
    unsigned char extra;
    ssize_t trailing = read(fd, &extra, 1);
    close(fd);
    if (trailing) {
        return update_error(error, size, "manifest changed during read");
    }
    *length = used;
    return 0;
}

static int pro_update(int argc, char **argv, const char *license_path, const char *socket_path,
                      char *error, size_t size)
{
    const char *bundle = NULL, *install = NULL, *download = NULL;
    bool rollback = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts("cast-pro update --bundle DIRECTORY [--download-only DIRECTORY | --install "
                 "/absolute/path/cast-pro] [--rollback]\n"
                 "Offline signed Pro channel; hosted updates are unavailable. Stop the Pro daemon "
                 "before installation.\n"
                 "--rollback explicitly permits an older eligible signed release; user files are "
                 "preserved.");
            return 0;
        }
        const char **value = NULL;
        if (!strcmp(argv[i], "--bundle")) {
            value = &bundle;
        } else if (!strcmp(argv[i], "--install")) {
            value = &install;
        } else if (!strcmp(argv[i], "--download-only")) {
            value = &download;
        } else if (!strcmp(argv[i], "--rollback")) {
            if (rollback) {
                return 2;
            }
            rollback = true;
            continue;
        } else {
            return update_error(
                       error, size,
                       "Pro uses update --bundle DIRECTORY; public Community updates are separate"),
                   2;
        }
        if (*value || ++i == argc || !argv[i][0]) {
            return update_error(error, size, "each update path option requires one value"), 2;
        }
        *value = argv[i];
    }
    if (!bundle) {
        return update_error(error, size,
                            "hosted Pro updates are unavailable; supply --bundle DIRECTORY"),
               5;
    }
    if (install && download) {
        return update_error(error, size, "choose --install or --download-only"), 2;
    }
    if (install && (install[0] != '/' || strcmp(strrchr(install, '/') + 1, "cast-pro"))) {
        return update_error(error, size, "installation target must be an absolute cast-pro path"),
               2;
    }
    char manifest_path[PATH_MAX], artifact_path[PATH_MAX];
    if (snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", bundle) >=
            (int)sizeof manifest_path ||
        snprintf(artifact_path, sizeof artifact_path, "%s/cast-pro", bundle) >=
            (int)sizeof artifact_path) {
        return update_error(error, size, "bundle path is too long"), 2;
    }
    unsigned char manifest[CAST_LICENSE_MAX];
    size_t length;
    if (read_manifest(manifest_path, manifest, &length, error, size)) {
        return 5;
    }
    const char *target = install;
    char downloaded[PATH_MAX];
    if (download) {
        if (snprintf(downloaded, sizeof downloaded, "%s/cast-pro", download) >=
            (int)sizeof downloaded) {
            return update_error(error, size, "download directory is too long"), 2;
        }
        target = downloaded;
    }
    char verification_dir[] = "/tmp/cast-pro-verify-XXXXXX";
    if (!target && !mkdtemp(verification_dir)) {
        return update_error(error, size, "creating verification directory failed"), 5;
    }
    char parent[PATH_MAX];
    if (!target) {
        snprintf(parent, sizeof parent, "%s/artifact", verification_dir);
    } else {
        snprintf(parent, sizeof parent, "%s", target);
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        snprintf(parent, sizeof parent, ".");
    } else if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    /* Resolve the parent once and hold its descriptor. No artifact-provided paths used. */
    char resolved[PATH_MAX];
    struct stat st;
    if (!realpath(parent, resolved) || lstat(parent, &st) || !S_ISDIR(st.st_mode) ||
        (st.st_mode & 0022) || st.st_uid != getuid()) {
        return update_error(error, size,
                            "update destination parent must exist, be owned by you and not "
                            "group/world writable"),
               5;
    }
    int directory = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        return update_error(error, size, "opening destination: %s", strerror(errno)), 5;
    }
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof temporary, "%s/.cast-pro-update-XXXXXX", resolved) >=
        (int)sizeof temporary) {
        close(directory);
        return 5;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        close(directory);
        return update_error(error, size, "creating private update stage: %s", strerror(errno)), 5;
    }
    int result = 5;
    int lock = -1;
    if (copy_artifact(artifact_path, fd, error, size)) {
        goto done;
    }
    CastUpdateMetadata metadata;
    result = cast_edition_verify_update(manifest, length, temporary, license_path,
                                        (int64_t)time(NULL), &metadata, error, size);
    if (result && !(download && result == 4)) {
        goto done;
    }
    if (result == 4) {
        fprintf(stderr, "cast-pro update: valid bundle is outside this license's release "
                        "entitlement; download only\n");
    }
    if (install && !rollback &&
        metadata.release_timestamp < cast_edition_identity()->release_timestamp) {
        result = 4;
        update_error(error, size,
                     "older release requires explicit --rollback; current installation preserved");
        goto done;
    }
    if (!target) {
        printf("Verified eligible offline Cast Pro %s (%s/%s); supply --install PATH to replace an "
               "installation\n",
               metadata.version, metadata.platform, metadata.media_profile);
        result = 0;
        goto done;
    }
    if (install) {
        char lock_path[PATH_MAX];
        if (!socket_path || !socket_path[0] ||
            snprintf(lock_path, sizeof lock_path, "%s.lock", socket_path) >=
                (int)sizeof lock_path) {
            result = 5;
            update_error(error, size,
                         "daemon session path is required to enforce stop-before-replace");
            goto done;
        }
        lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || fstat(lock, &st) || st.st_uid != getuid() || !S_ISREG(st.st_mode) ||
            (st.st_mode & 0077) || flock(lock, LOCK_EX | LOCK_NB)) {
            result = 5;
            update_error(error, size, "stop the Cast Pro daemon before replacing its executable");
            goto done;
        }
    }
    const char *name = strrchr(target, '/');
    name = name ? name + 1 : target;
    if (!fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0022)) {
            result = 5;
            update_error(error, size, "existing installation must be an owned regular file");
            goto done;
        }
        if (download) {
            result = 5;
            update_error(error, size, "download target already exists; no files overwritten");
            goto done;
        }
        if (linkat(directory, name, directory, "cast-pro.rollback", 0)) {
            result = 5;
            update_error(error, size,
                         "preserve/remove existing cast-pro.rollback before installing: %s",
                         strerror(errno));
            goto done;
        }
    } else if (errno != ENOENT) {
        result = 5;
        goto done;
    }
    if (fchmod(fd, 0755) || fsync(fd) ||
        renameat(directory, strrchr(temporary, '/') + 1, directory, name)) {
        result = 5;
        update_error(error, size, "atomic update failed: %s; installed executable preserved",
                     strerror(errno));
        goto done;
    }
    if (fsync(directory)) {
        /* Best effort rollback after a post-rename durability failure. */
        if (install) {
            renameat(directory, "cast-pro.rollback", directory, name);
        }
        result = 5;
        update_error(error, size, "update durability failed; rollback attempted");
        goto done;
    }
    if (download) {
        int mf = openat(directory, "manifest.json",
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (mf < 0 || write(mf, manifest, length) != (ssize_t)length || fsync(mf)) {
            if (mf >= 0) {
                close(mf);
            }
            unlinkat(directory, name, 0);
            result = 5;
            update_error(error, size, "saving download manifest failed");
            goto done;
        }
        close(mf);
        fsync(directory);
    }
    printf("%s offline Cast Pro %s; configuration, license and media preserved\n",
           download ? "Saved" : "Installed", metadata.version);
    result = 0;
done:
    if (lock >= 0) {
        close(lock);
    }
    close(fd);
    unlink(temporary);
    close(directory);
    if (!target) {
        rmdir(verification_dir);
    }
    return result;
}

int cast_update_with_context(int argc, char **argv, const char *license_path,
                             const char *socket_path, char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }
    if (argc < 1 || !argv || !argv[0] || strcmp(argv[0], "update")) {
        return update_error(error, error_size, "expected update arguments");
    }
    return cast_edition_identity()->pro
               ? pro_update(argc, argv, license_path, socket_path, error, error_size)
               : community_update(argc, argv, error, error_size);
}

int cast_update(int argc, char **argv, char *error, size_t error_size)
{
    return cast_update_with_context(argc, argv, NULL, NULL, error, error_size);
}
