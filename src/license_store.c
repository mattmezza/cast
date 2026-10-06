#include "license_store.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail(char *e, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (n) {
        vsnprintf(e, n, fmt, ap);
    }
    va_end(ap);
    return -1;
}
int cast_license_store_path(const char *configured, char *out, size_t n, char *e, size_t en)
{
    int r;
    if (configured && *configured) {
        r = snprintf(out, n, "%s", configured);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) {
            if (*xdg != '/') {
                return fail(e, en, "XDG_DATA_HOME must be absolute");
            }
            r = snprintf(out, n, "%s/cast/license.json", xdg);
        } else {
            if (!home || *home != '/') {
                return fail(e, en, "HOME must be absolute for license storage");
            }
            r = snprintf(out, n, "%s/.local/share/cast/license.json", home);
        }
    }
    if (r < 0 || (size_t)r >= n) {
        return fail(e, en, "license path exceeds limit");
    }
    return 0;
}
/* Walk directory components using descriptors. No component can be a symlink. */
static int parent(const char *path, char *leaf, size_t ln, int create, char *e, size_t en)
{
    if (!path || !*path || strlen(path) >= PATH_MAX) {
        return fail(e, en, "invalid license path");
    }
    char copy[PATH_MAX];
    strcpy(copy, path);
    char *slash = strrchr(copy, '/'), *base = slash ? slash + 1 : copy;
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..") || strlen(base) >= ln) {
        return fail(e, en, "invalid license filename");
    }
    strcpy(leaf, base);
    int fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return fail(e, en, "open license directory: %s", strerror(errno));
    }
    if (slash) {
        *slash = 0;
    } else {
        copy[0] = 0;
    }
    char *save = NULL;
    for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        /* Descriptor traversal resolves ordinary ./../relative aliases onto the
         * same directory inode. Symlink components remain rejected below. */
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, p, 0700) && errno != EEXIST) {
                close(fd);
                return fail(e, en, "create license directory: %s", strerror(errno));
            }
            next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        close(fd);
        fd = next;
        if (fd < 0) {
            return fail(e, en, "unsafe/unavailable license directory: %s", strerror(errno));
        }
    }
    return fd;
}
static int regular(int fd, int private, char *e, size_t n)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (private && (st.st_uid != geteuid() || (st.st_mode & 077) || st.st_nlink != 1))) {
        return fail(e, n, "license target must be a regular private user-owned file");
    }
    return 0;
}
int cast_license_store_read(const char *path, unsigned char *b, size_t cap, size_t *len, char *e,
                            size_t n)
{
    char leaf[NAME_MAX + 1];
    int dir = parent(path, leaf, sizeof leaf, 0, e, n);
    if (dir < 0) {
        if (errno == ENOENT) {
            return 1;
        }
        return -1;
    }
    int fd = openat(dir, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    close(dir);
    errno = saved;
    if (fd < 0) {
        if (errno == ENOENT) {
            return 1;
        }
        return fail(e, n, "open license file: %s", strerror(errno));
    }
    if (regular(fd, 0, e, n)) {
        close(fd);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || (uintmax_t)st.st_size > cap) {
        close(fd);
        return fail(e, n, "license exceeds bounded file limit");
    }
    size_t used = 0;
    while (used < cap) {
        ssize_t r = read(fd, b + used, cap - used);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            close(fd);
            return fail(e, n, "read license: %s", strerror(errno));
        }
        if (!r) {
            break;
        }
        used += (size_t)r;
    }
    unsigned char extra;
    ssize_t r = read(fd, &extra, 1);
    close(fd);
    if (r != 0) {
        return fail(e, n, "license exceeds bounded file limit");
    }
    *len = used;
    return 0;
}
static int store_lock(const char *path, const char *suffix, struct stat *directory, char *leaf_out,
                      char *e, size_t n)
{
    char leaf[NAME_MAX + 1], lock[NAME_MAX + 1];
    int dir = parent(path, leaf, sizeof leaf, 1, e, n);
    if (dir < 0) {
        return -1;
    }
    if (snprintf(lock, sizeof lock, ".%s.%s", leaf, suffix) >= (int)sizeof lock) {
        close(dir);
        return fail(e, n, "license lock filename too long");
    }
    struct stat st;
    if (fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 022)) {
        close(dir);
        return fail(e, n,
                    "license store directory must be user-owned and not group/world writable");
    }
    if (directory) {
        *directory = st;
    }
    if (leaf_out) {
        strcpy(leaf_out, leaf);
    }
    int fd = openat(dir, lock, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    close(dir);
    if (fd < 0) {
        return fail(e, n, "open secure license lock: %s", strerror(errno));
    }
    if (regular(fd, 1, e, n)) {
        close(fd);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int saved = errno;
        close(fd);
        fail(e, n, "license store busy or unsafe");
        errno = saved;
        return -1;
    }
    return fd;
}
int cast_license_store_lock(const char *path, char *e, size_t n)
{
    return store_lock(path, "lock", NULL, NULL, e, n);
}
struct CastLicenseSession {
    int fd;
    dev_t directory_device;
    ino_t directory_inode;
    char filename[NAME_MAX + 1];
};
CastLicenseSession *cast_license_session_open(const char *configured, char *e, size_t n)
{
    char path[PATH_MAX], leaf[NAME_MAX + 1];
    struct stat directory;
    if (cast_license_store_path(configured, path, sizeof path, e, n)) {
        return NULL;
    }
    int fd = store_lock(path, "session", &directory, leaf, e, n);
    if (fd < 0) {
        /* The message covers other editions, custom sockets and standalone
         * writers without pretending to infer their process/socket identities. */
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            fail(e, n,
                 "license store is owned by another daemon/session; use its authenticated daemon "
                 "command or stop the owning daemon");
        }
        return NULL;
    }
    CastLicenseSession *session = calloc(1, sizeof *session);
    if (!session) {
        close(fd);
        fail(e, n, "allocate bounded license session");
        return NULL;
    }
    session->fd = fd;
    session->directory_device = directory.st_dev;
    session->directory_inode = directory.st_ino;
    strcpy(session->filename, leaf);
    return session;
}
void cast_license_session_close(CastLicenseSession *session)
{
    if (!session) {
        return;
    }
    close(session->fd);
    memset(session, 0, sizeof *session);
    free(session);
}
int cast_license_session_matches(const CastLicenseSession *session, const char *configured, char *e,
                                 size_t n)
{
    if (!session) {
        return fail(e, n, "license store is not owned by this daemon/session");
    }
    char path[PATH_MAX], leaf[NAME_MAX + 1];
    if (cast_license_store_path(configured, path, sizeof path, e, n)) {
        return -1;
    }
    int dir = parent(path, leaf, sizeof leaf, 0, e, n);
    if (dir < 0) {
        return -1;
    }
    struct stat directory;
    int result = fstat(dir, &directory);
    char lock[NAME_MAX + 1];
    struct stat held, current;
    int length = snprintf(lock, sizeof lock, ".%s.session", leaf);
    bool current_lock = length > 0 && (size_t)length < sizeof lock && !fstat(session->fd, &held) &&
                        !fstatat(dir, lock, &current, AT_SYMLINK_NOFOLLOW) &&
                        held.st_dev == current.st_dev && held.st_ino == current.st_ino;
    close(dir);
    if (result || !current_lock || directory.st_dev != session->directory_device ||
        directory.st_ino != session->directory_inode || strcmp(leaf, session->filename)) {
        return fail(e, n, "license session belongs to a different physical store");
    }
    return 0;
}
static int safe_target(int dir, const char *leaf, char *e, size_t n)
{
    struct stat st;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW)) {
        return errno == ENOENT ? 0 : fail(e, n, "inspect license target: %s", strerror(errno));
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        return fail(e, n, "unsafe existing license target");
    }
    return 0;
}
int cast_license_store_write(const char *path, const unsigned char *b, size_t len, char *e,
                             size_t n)
{
    char leaf[NAME_MAX + 1], tmp[NAME_MAX + 1];
    int dir = parent(path, leaf, sizeof leaf, 1, e, n);
    if (dir < 0) {
        return -1;
    }
    if (safe_target(dir, leaf, e, n)) {
        close(dir);
        return -1;
    }
    int fd = -1;
    for (unsigned i = 0; i < 100; i++) {
        snprintf(tmp, sizeof tmp, ".cast-license-%ld-%u.tmp", (long)getpid(), i);
        fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        close(dir);
        return fail(e, n, "create license snapshot: %s", strerror(errno));
    }
    size_t off = 0;
    int bad = 0;
    while (off < len) {
        ssize_t r = write(fd, b + off, len - off);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            bad = 1;
            break;
        }
        off += (size_t)r;
    }
    if (!bad && fsync(fd)) {
        bad = 1;
    }
    if (close(fd)) {
        bad = 1;
    }
    if (!bad && safe_target(dir, leaf, e, n)) {
        bad = 1;
    }
    if (!bad && renameat(dir, tmp, dir, leaf)) {
        bad = 1;
    }
    if (!bad && fsync(dir)) {
        bad = 1;
    }
    if (bad) {
        unlinkat(dir, tmp, 0);
        close(dir);
        return fail(e, n, "commit license snapshot failed: %s", strerror(errno));
    }
    close(dir);
    return 0;
}
int cast_license_store_remove(const char *path, char *e, size_t n)
{
    char leaf[NAME_MAX + 1];
    int dir = parent(path, leaf, sizeof leaf, 0, e, n);
    if (dir < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (safe_target(dir, leaf, e, n)) {
        close(dir);
        return -1;
    }
    if (unlinkat(dir, leaf, 0) && errno != ENOENT) {
        close(dir);
        return fail(e, n, "remove license: %s", strerror(errno));
    }
    int r = fsync(dir);
    close(dir);
    return r ? fail(e, n, "sync license removal failed") : 0;
}
