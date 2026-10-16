#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CAST_OS_COMPAT_IMPLEMENTATION
#include "os_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#else
#include <sys/eventfd.h>
#endif
#ifdef __APPLE__
static int descriptor_flags(int fd, bool nonblocking)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        (nonblocking && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#ifdef __APPLE__
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return fd;
}
#endif
int cast_pipe_cloexec(int descriptors[2])
{
#ifdef __APPLE__
    if (pipe(descriptors)) {
        return -1;
    }
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC)) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    return 0;
#else
    return pipe2(descriptors, O_CLOEXEC);
#endif
}
int cast_socket(bool nonblocking)
{
#ifdef __APPLE__
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    return fd < 0 ? fd : descriptor_flags(fd, nonblocking);
#else
#ifdef CAST_TEST_STREAM_IPC
    int type = SOCK_STREAM;
#else
    int type = SOCK_SEQPACKET;
#endif
    return socket(AF_UNIX, type | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0);
#endif
}
int cast_accept(int fd)
{
#ifdef __APPLE__
    int accepted = accept(fd, NULL, NULL);
    return accepted < 0 ? accepted : descriptor_flags(accepted, true);
#else
    return accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
}
int cast_peer_identity(int fd, uid_t *uid, pid_t *pid)
{
#ifdef __APPLE__
    gid_t group;
    if (getpeereid(fd, uid, &group)) {
        return -1;
    }
    socklen_t size = sizeof *pid;
    return getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, pid, &size);
#else
    struct ucred peer;
    socklen_t size = sizeof peer;
    int result = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size);
    if (!result) {
        *uid = peer.uid;
        *pid = peer.pid;
    }
    return result;
#endif
}
const char *cast_executable_path(void)
{
#ifdef __APPLE__
    static _Thread_local char path[PATH_MAX];
    uint32_t size = sizeof path;
    return _NSGetExecutablePath(path, &size) ? "" : path;
#else
    return "/proc/self/exe";
#endif
}
int cast_spawn_self(pid_t *pid, const posix_spawn_file_actions_t *actions, char *const arguments[],
                    char *const environment[])
{
#ifdef __APPLE__
    /* Darwin closes every descriptor except explicit spawn actions. This is the
     * equivalent of Linux's addclosefrom action without a racy descriptor walk. */
    posix_spawnattr_t attributes;
    int result = posix_spawnattr_init(&attributes);
    if (result) {
        return result;
    }
    result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (!result) {
        result =
            posix_spawn(pid, cast_executable_path(), actions, &attributes, arguments, environment);
    }
    posix_spawnattr_destroy(&attributes);
    return result;
#else
    return posix_spawn(pid, cast_executable_path(), actions, NULL, arguments, environment);
#endif
}
int cast_cond_monotonic(pthread_cond_t *condition)
{
#ifdef __APPLE__
    return pthread_cond_init(condition, NULL);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int result = pthread_cond_init(condition, &attr);
    pthread_condattr_destroy(&attr);
    return result;
#endif
}
int cast_cond_wait_monotonic(pthread_cond_t *condition, pthread_mutex_t *mutex,
                             const struct timespec *deadline)
{
#ifdef __APPLE__
    struct timespec now, relative;
    clock_gettime(CLOCK_MONOTONIC, &now);
    relative.tv_sec = deadline->tv_sec - now.tv_sec;
    relative.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (relative.tv_nsec < 0) {
        relative.tv_sec--;
        relative.tv_nsec += 1000000000;
    }
    if (relative.tv_sec < 0) {
        return ETIMEDOUT;
    }
    return pthread_cond_timedwait_relative_np(condition, mutex, &relative);
#else
    return pthread_cond_timedwait(condition, mutex, deadline);
#endif
}
#ifdef __APPLE__
static int private_file(int *reader)
{
    char path[] = "/tmp/cast-memory-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    *reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int saved = errno;
    unlink(path);
    if (*reader < 0) {
        close(fd);
        errno = saved;
        return -1;
    }
    return descriptor_flags(fd, false);
}
#endif
int cast_preview_files(size_t size, int *writer, int *reader)
{
#ifdef __APPLE__
    *writer = private_file(reader);
    if (*writer < 0) {
        return -1;
    }
    if (ftruncate(*writer, (off_t)size)) {
        close(*writer);
        close(*reader);
        *writer = *reader = -1;
        return -1;
    }
    return 0;
#else
    *writer = memfd_create("cast-panel-preview", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    *reader = -1;
    return *writer < 0 ? -1 : ftruncate(*writer, (off_t)size);
#endif
}
int cast_handoff_create(const void *data, size_t size)
{
#ifdef __APPLE__
    int reader = -1;
    int fd = private_file(&reader);
#else
    int fd = memfd_create("cast-app-config", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    if (fd < 0) {
        return -1;
    }
    int result = ftruncate(fd, (off_t)size) || pwrite(fd, data, size, 0) != (ssize_t)size;
#ifndef __APPLE__
    if (!result) {
        result = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    }
#endif
    if (result) {
        close(fd);
#ifdef __APPLE__
        close(reader);
#endif
        return -1;
    }
#ifdef __APPLE__
    close(fd);
    return reader;
#else
    return fd;
#endif
}
bool cast_handoff_valid(int fd)
{
#ifdef __APPLE__
    struct stat st;
    return fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 0077) && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY;
#else
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    return seals >= 0 && (seals & required) == required;
#endif
}
bool cast_preview_valid(int fd)
{
#ifdef __APPLE__
    return cast_handoff_valid(fd);
#else
    int required = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    return seals >= 0 && (seals & required) == required;
#endif
}
int cast_wake_open(int *writer)
{
#ifdef __APPLE__
    int descriptors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors)) {
        return -1;
    }
    int reader = descriptor_flags(descriptors[0], true);
    *writer = descriptor_flags(descriptors[1], true);
    if (reader < 0 || *writer < 0) {
        if (reader >= 0) {
            close(reader);
        }
        if (*writer >= 0) {
            close(*writer);
        }
        return -1;
    }
    return reader;
#else
    *writer = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return *writer;
#endif
}
#if defined(__APPLE__) || defined(CAST_TEST_STREAM_IPC)
/* The wire prefix preserves message boundaries on Darwin's Unix stream sockets.
 * Peek first so a partial nonblocking message never consumes protocol state. */
static ssize_t frame_available(int fd, int flags, uint32_t *length)
{
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        uint32_t header;
        ssize_t count = recv(fd, &header, sizeof header, MSG_PEEK | MSG_DONTWAIT);
        if (!count) {
            return 0;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
        int available = 0;
        if (count == sizeof header) {
            if (header > 8192U) {
                errno = EMSGSIZE;
                return -1;
            }
            if (ioctl(fd, FIONREAD, &available)) {
                return -1;
            }
            if ((size_t)available >= sizeof header + header) {
                *length = header;
                return 1;
            }
        }
        if (flags & MSG_DONTWAIT) {
            errno = EAGAIN;
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec >= 5) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (count > 0) {
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 1000) <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (p.revents & POLLHUP) {
            errno = ECONNRESET;
            return -1;
        }
    }
}
ssize_t cast_ipc_sendmsg(int fd, const struct msghdr *message, int flags)
{
    size_t length = 0;
    for (size_t i = 0; i < (size_t)message->msg_iovlen; i++) {
        length += message->msg_iov[i].iov_len;
    }
    if (length > 8192U) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t header = (uint32_t)length;
    struct iovec *vectors = calloc((size_t)message->msg_iovlen + 1, sizeof *vectors);
    if (!vectors) {
        return -1;
    }
    vectors[0] = (struct iovec){&header, sizeof header};
    memcpy(vectors + 1, message->msg_iov, message->msg_iovlen * sizeof *vectors);
    struct msghdr framed = *message;
    framed.msg_iov = vectors;
    framed.msg_iovlen++;
    size_t remaining = sizeof header + length;
    bool ancillary = true;
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (remaining) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec >= 5) {
            free(vectors);
            shutdown(fd, SHUT_RDWR);
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t sent = sendmsg(fd, &framed, flags & ~MSG_DONTWAIT);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) > 0) {
                continue;
            }
        }
        if (sent <= 0) {
            free(vectors);
            shutdown(fd, SHUT_RDWR);
            return -1;
        }
        remaining -= (size_t)sent;
        if (ancillary) {
            framed.msg_control = NULL;
            framed.msg_controllen = 0;
            ancillary = false;
        }
        size_t consumed = (size_t)sent;
        while (framed.msg_iovlen && consumed >= framed.msg_iov[0].iov_len) {
            consumed -= framed.msg_iov[0].iov_len;
            framed.msg_iov++;
            framed.msg_iovlen--;
        }
        if (framed.msg_iovlen) {
            framed.msg_iov[0].iov_base = (char *)framed.msg_iov[0].iov_base + consumed;
            framed.msg_iov[0].iov_len -= consumed;
        }
    }
    free(vectors);
    return (ssize_t)length;
}
ssize_t cast_ipc_recvmsg(int fd, struct msghdr *message, int flags)
{
    uint32_t length;
    ssize_t ready = frame_available(fd, flags, &length);
    if (ready <= 0) {
        return ready;
    }
    uint8_t *bytes = malloc((size_t)length + sizeof length);
    if (!bytes) {
        return -1;
    }
    struct iovec vector = {bytes, (size_t)length + sizeof length};
    struct msghdr framed = *message;
    framed.msg_iov = &vector;
    framed.msg_iovlen = 1;
    ssize_t received = recvmsg(fd, &framed, flags & ~(MSG_TRUNC | MSG_PEEK));
    if (received != (ssize_t)vector.iov_len) {
        if (received > 0) {
            for (struct cmsghdr *control = CMSG_FIRSTHDR(&framed); control;
                 control = CMSG_NXTHDR(&framed, control)) {
                if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_RIGHTS) {
                    size_t count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    int *descriptors = (int *)CMSG_DATA(control);
                    for (size_t i = 0; i < count; i++) {
                        close(descriptors[i]);
                    }
                }
            }
        }
        free(bytes);
        errno = EIO;
        return -1;
    }
    size_t copied = 0;
    for (size_t i = 0; i < (size_t)message->msg_iovlen; i++) {
        size_t count = message->msg_iov[i].iov_len;
        if (count > length - copied) {
            count = length - copied;
        }
        memcpy(message->msg_iov[i].iov_base, bytes + sizeof length + copied, count);
        copied += count;
    }
    for (struct cmsghdr *control = CMSG_FIRSTHDR(&framed); control;
         control = CMSG_NXTHDR(&framed, control)) {
        if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_RIGHTS) {
            size_t count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *descriptors = (int *)CMSG_DATA(control);
            for (size_t i = 0; i < count; i++) {
                fcntl(descriptors[i], F_SETFD, FD_CLOEXEC);
            }
        }
    }
    message->msg_controllen = framed.msg_controllen;
    message->msg_flags = framed.msg_flags | (copied < length ? MSG_TRUNC : 0);
    free(bytes);
    return (ssize_t)((flags & MSG_TRUNC) ? length : copied);
}
ssize_t cast_ipc_send(int fd, const void *buffer, size_t size, int flags)
{
    struct iovec vector = {(void *)buffer, size};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    return cast_ipc_sendmsg(fd, &message, flags);
}
ssize_t cast_ipc_recv(int fd, void *buffer, size_t size, int flags)
{
    if (flags & MSG_PEEK) {
        uint32_t length;
        ssize_t result = frame_available(fd, flags, &length);
        return result > 0 ? (ssize_t)(length < size ? length : size) : result;
    }
    struct iovec vector = {buffer, size};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    return cast_ipc_recvmsg(fd, &message, flags);
}
#endif
