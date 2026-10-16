#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os_compat.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <unistd.h>

int main(void)
{
    int wake_writer;
    int wake = cast_wake_open(&wake_writer);
    assert(wake >= 0 && wake_writer >= 0);
    uint64_t value = 1, received;
    assert(write(wake_writer, &value, sizeof value) == sizeof value);
    struct pollfd pending = {wake, POLLIN, 0};
    assert(poll(&pending, 1, 100) == 1);
    assert(read(wake, &received, sizeof received) == sizeof received);
    assert(received == 1);
    if (wake_writer != wake) {
        close(wake_writer);
    }
    close(wake);

    int handoff = cast_handoff_create("configuration", 14);
    assert(handoff >= 0 && cast_handoff_valid(handoff));
    char bytes[32] = {0};
    assert(pread(handoff, bytes, 14, 0) == 14 && !strcmp(bytes, "configuration"));
    assert(pwrite(handoff, "x", 1, 0) < 0);
    assert(ftruncate(handoff, 0) < 0);
    close(handoff);

    int descriptors[2];
    assert(!cast_pipe_cloexec(descriptors));
    assert(fcntl(descriptors[0], F_GETFD) & FD_CLOEXEC);
    assert(fcntl(descriptors[1], F_GETFD) & FD_CLOEXEC);
    close(descriptors[0]);
    close(descriptors[1]);

    pthread_cond_t condition;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    assert(!cast_cond_monotonic(&condition));
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 10000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&mutex);
    assert(cast_cond_wait_monotonic(&condition, &mutex, &deadline) == ETIMEDOUT);
    pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&condition);

    int sockets[2];
#if defined(__APPLE__) || defined(CAST_TEST_STREAM_IPC)
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
#else
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
#endif
    assert(send(sockets[0], "first", 6, 0) == 6);
    assert(send(sockets[0], "second", 7, 0) == 7);
    assert(recv(sockets[1], bytes, sizeof bytes, 0) == 6 && !strcmp(bytes, "first"));
    assert(recv(sockets[1], bytes, 3, MSG_TRUNC) == 7 && !memcmp(bytes, "sec", 3));
    uid_t uid;
    pid_t pid;
    assert(!cast_peer_identity(sockets[0], &uid, &pid) && uid == getuid() && pid == getpid());

#if defined(__APPLE__) || defined(CAST_TEST_STREAM_IPC)
    uint32_t header = 6;
    assert(write(sockets[0], &header, 2) == 2);
    assert(recv(sockets[1], bytes, sizeof bytes, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    assert(write(sockets[0], (char *)&header + 2, 2) == 2);
    assert(write(sockets[0], "pa", 2) == 2);
    assert(recv(sockets[1], bytes, sizeof bytes, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    assert(write(sockets[0], "rt!", 4) == 4);
    assert(recv(sockets[1], bytes, sizeof bytes, MSG_DONTWAIT) == 6 && !strcmp(bytes, "part!"));
#endif
    int passed = open("/dev/null", O_RDONLY | O_CLOEXEC);
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec vector = {(void *)"descriptor", 11};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof control};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &passed, sizeof passed);
    assert(sendmsg(sockets[0], &message, 0) == 11);
    vector.iov_base = bytes;
    vector.iov_len = sizeof bytes;
    memset(control, 0, sizeof control);
    assert(recvmsg(sockets[1], &message, MSG_CMSG_CLOEXEC) == 11);
    assert(!strcmp(bytes, "descriptor"));
    cmsg = CMSG_FIRSTHDR(&message);
    assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
    int copied;
    memcpy(&copied, CMSG_DATA(cmsg), sizeof copied);
    assert(fcntl(copied, F_GETFD) & FD_CLOEXEC);
    close(passed);
    close(copied);
#if defined(__APPLE__) || defined(CAST_TEST_STREAM_IPC)
    header = 8193;
    assert(write(sockets[0], &header, sizeof header) == sizeof header);
    assert(recv(sockets[1], bytes, sizeof bytes, MSG_DONTWAIT) < 0 && errno == EMSGSIZE);
#endif
    close(sockets[0]);
    close(sockets[1]);
    puts("OS compatibility: wake, immutable handoff, monotonic wait, peer identity, message "
         "boundaries and descriptor passing passed");
    return 0;
}
