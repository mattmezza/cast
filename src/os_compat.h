#ifndef CAST_OS_COMPAT_H
#define CAST_OS_COMPAT_H
#include <pthread.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
int cast_socket(bool nonblocking);
int cast_pipe_cloexec(int descriptors[2]);
int cast_accept(int fd);
int cast_peer_identity(int fd, uid_t *uid, pid_t *pid);
const char *cast_executable_path(void);
int cast_spawn_self(pid_t *pid, const posix_spawn_file_actions_t *actions, char *const arguments[],
                    char *const environment[]);
int cast_cond_monotonic(pthread_cond_t *condition);
int cast_cond_wait_monotonic(pthread_cond_t *condition, pthread_mutex_t *mutex,
                             const struct timespec *deadline);
int cast_preview_files(size_t size, int *writer, int *reader);
int cast_handoff_create(const void *data, size_t size);
bool cast_handoff_valid(int fd);
bool cast_preview_valid(int fd);
int cast_wake_open(int *writer);
#ifdef __APPLE__
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif
#define st_ctim st_ctimespec
#define st_mtim st_mtimespec
#endif
#if defined(__APPLE__) || defined(CAST_TEST_STREAM_IPC)
ssize_t cast_ipc_send(int fd, const void *buffer, size_t size, int flags);
ssize_t cast_ipc_recv(int fd, void *buffer, size_t size, int flags);
ssize_t cast_ipc_sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t cast_ipc_recvmsg(int fd, struct msghdr *message, int flags);
#ifndef CAST_OS_COMPAT_IMPLEMENTATION
#define send cast_ipc_send
#define recv cast_ipc_recv
#define sendmsg cast_ipc_sendmsg
#define recvmsg cast_ipc_recvmsg
#endif
#endif
#endif
