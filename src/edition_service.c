#include "edition_service.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LICENSE_QUEUE 8
typedef struct {
    int fd, argc;
    size_t offsets[CAST_MAX_ARGS];
    char arguments[CAST_IPC_MAX];
    Config config;
} LicenseJob;
struct EditionService {
    pthread_t thread, workflow_thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool stop, refresh, working, workflow_working;
    CastLicenseSession *retired_session;
    char path[PATH_MAX];
    LicenseJob jobs[LICENSE_QUEUE], workflow_jobs[LICENSE_QUEUE];
    EditionReply replies[LICENSE_QUEUE];
    unsigned head, count, workflow_head, workflow_count, reply_head, reply_count;
    CastEditionSnapshot snapshot;
    uint64_t checked_at;
};
static void unavailable_snapshot(CastEditionSnapshot *snapshot)
{
    char ignored[1];
    /* NULL ownership exits before filesystem access or cryptographic work. */
    cast_edition_snapshot_in_session(NULL, "", 0, snapshot, ignored, sizeof ignored);
}
static void *license_worker(void *context)
{
    EditionService *s = context;
    CastLicenseSession *session = NULL;
    char session_path[PATH_MAX] = "";
    pthread_mutex_lock(&s->mutex);
    CastEditionSnapshot previous = s->snapshot;
    while (!s->stop) {
        if (!s->count && !s->refresh) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec++;
            pthread_cond_timedwait(&s->wake, &s->mutex, &until);
        }
        if (s->stop) {
            break;
        }
        bool has_job = s->count != 0;
        LicenseJob job;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s", s->path);
        s->working = true;
        s->refresh = false;
        if (has_job) {
            job = s->jobs[s->head];
            s->head = (s->head + 1) % LICENSE_QUEUE;
            s->count--;
        }
        pthread_mutex_unlock(&s->mutex);
        char session_error[CAST_ERR] = "";
        if (session && strcmp(session_path, path)) {
            cast_license_session_close(session);
            session = NULL;
        }
        if (!session) {
            session = cast_license_session_open(path, session_error, sizeof session_error);
            snprintf(session_path, sizeof session_path, "%s", path);
        }
        EditionReply reply = {.fd = -1};
        if (has_job) {
            char *argv[CAST_MAX_ARGS], error[CAST_ERR] = "";
            for (int i = 0; i < job.argc; ++i) {
                argv[i] = job.arguments + job.offsets[i];
            }
            reply.fd = job.fd;
            if (cast_edition_has_request(job.argc, argv)) {
                CastEditionSnapshot current;
                cast_edition_snapshot_in_session(session, job.config.licensing_file,
                                                 (int64_t)time(NULL), &current, error,
                                                 sizeof error);
                reply.result = cast_edition_dispatch(&job.config, &current, job.argc, argv,
                                                     reply.output, sizeof reply.output);
            } else {
                reply.result = cast_edition_command_in_session(
                    session, job.argc, argv, job.config.licensing_file, reply.output,
                    sizeof reply.output, error, sizeof error);
            }
            if (reply.result && error[0]) {
                snprintf(reply.output, sizeof reply.output, "%s", error);
            }
        }
        CastEditionSnapshot snapshot;
        char error[CAST_ERR];
        int64_t now = (int64_t)time(NULL);
        cast_edition_snapshot_in_session(session, path, now, &snapshot, error, sizeof error);
        cast_edition_transition(&previous, &snapshot, now);
        previous = snapshot;
        pthread_mutex_lock(&s->mutex);
        s->working = false;
        if (!strcmp(path, s->path)) {
            s->snapshot = snapshot;
            struct timespec checked;
            clock_gettime(CLOCK_MONOTONIC, &checked);
            s->checked_at = (uint64_t)checked.tv_sec * UINT64_C(1000000000) + checked.tv_nsec;
        } else {
            s->refresh = true;
        }
        if (has_job) {
            if (s->reply_count < LICENSE_QUEUE) {
                unsigned tail = (s->reply_head + s->reply_count++) % LICENSE_QUEUE;
                s->replies[tail] = reply;
            } else {
                close(reply.fd);
            }
        }
    }
    pthread_mutex_unlock(&s->mutex);
    s->retired_session = session;
    return NULL;
}
/* Model/font preparation can take seconds. It has its own bounded command
 * worker so signed-store refresh and removals never wait for that preparation. */
static void *workflow_worker(void *context)
{
    EditionService *s = context;
    pthread_mutex_lock(&s->mutex);
    while (!s->stop) {
        while (!s->workflow_count && !s->stop) {
            pthread_cond_wait(&s->wake, &s->mutex);
        }
        if (s->stop) {
            break;
        }
        LicenseJob job = s->workflow_jobs[s->workflow_head];
        s->workflow_head = (s->workflow_head + 1) % LICENSE_QUEUE;
        --s->workflow_count;
        s->workflow_working = true;
        CastEditionSnapshot snapshot = s->snapshot;
        uint64_t checked_at = s->checked_at;
        bool same_path = !strcmp(job.config.licensing_file, s->path);
        pthread_mutex_unlock(&s->mutex);
        char *argv[CAST_MAX_ARGS];
        for (int i = 0; i < job.argc; ++i) {
            argv[i] = job.arguments + job.offsets[i];
        }
        EditionReply reply = {.fd = job.fd};
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t timestamp = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
        if (!same_path || !checked_at || timestamp < checked_at ||
            timestamp - checked_at > UINT64_C(2000000000)) {
            reply.result = 5;
            snprintf(reply.output, sizeof reply.output,
                     "authoritative edition state refreshing; retry after acknowledgement");
        } else {
            reply.result = cast_edition_dispatch(&job.config, &snapshot, job.argc, argv,
                                                 reply.output, sizeof reply.output);
        }
        pthread_mutex_lock(&s->mutex);
        s->workflow_working = false;
        if (s->reply_count < LICENSE_QUEUE) {
            s->replies[(s->reply_head + s->reply_count++) % LICENSE_QUEUE] = reply;
        } else {
            close(reply.fd);
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
EditionService *edition_service_open(const char *path, char *error, size_t size)
{
    EditionService *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(error, size, "cannot allocate license worker");
        return NULL;
    }
    snprintf(s->path, sizeof s->path, "%s", path);
    unavailable_snapshot(&s->snapshot);
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->refresh = true;
    int result = pthread_create(&s->thread, NULL, license_worker, s);
    if (result) {
        snprintf(error, size, "cannot start license worker: %s", strerror(result));
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    result = pthread_create(&s->workflow_thread, NULL, workflow_worker, s);
    if (result) {
        snprintf(error, size, "cannot start workflow command worker: %s", strerror(result));
        pthread_mutex_lock(&s->mutex);
        s->stop = true;
        pthread_cond_broadcast(&s->wake);
        pthread_mutex_unlock(&s->mutex);
        pthread_join(s->thread, NULL);
        cast_license_session_close(s->retired_session);
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    return s;
}
int edition_service_submit(EditionService *s, int fd, int argc, char **argv, const Config *config,
                           char *error, size_t size)
{
    if (!s || !config || argc < 1 || argc > CAST_MAX_ARGS) {
        snprintf(error, size, "license worker unavailable or malformed request");
        return -1;
    }
    LicenseJob job = {.fd = fd, .argc = argc, .config = *config};
    size_t offset = 0;
    for (int i = 0; i < argc; ++i) {
        size_t length = strlen(argv[i]) + 1;
        if (length > sizeof job.arguments - offset) {
            snprintf(error, size, "license request exceeds its bound");
            return -1;
        }
        job.offsets[i] = offset;
        memcpy(job.arguments + offset, argv[i], length);
        offset += length;
    }
    pthread_mutex_lock(&s->mutex);
    if (s->count + s->workflow_count + s->reply_count + s->working + s->workflow_working >=
            LICENSE_QUEUE - 1 ||
        s->stop) {
        pthread_mutex_unlock(&s->mutex);
        snprintf(error, size, "license worker busy; retry after the pending request");
        return -1;
    }
    if (cast_edition_has_request(argc, argv)) {
        s->workflow_jobs[(s->workflow_head + s->workflow_count++) % LICENSE_QUEUE] = job;
    } else {
        s->jobs[(s->head + s->count++) % LICENSE_QUEUE] = job;
    }
    pthread_cond_broadcast(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}
bool edition_service_busy(EditionService *s)
{
    if (!s) {
        return false;
    }
    pthread_mutex_lock(&s->mutex);
    bool busy =
        s->count || s->workflow_count || s->reply_count || s->working || s->workflow_working;
    pthread_mutex_unlock(&s->mutex);
    return busy;
}
bool edition_service_reply(EditionService *s, EditionReply *reply)
{
    if (!s) {
        return false;
    }
    pthread_mutex_lock(&s->mutex);
    bool ready = s->reply_count != 0;
    if (ready) {
        *reply = s->replies[s->reply_head];
        s->reply_head = (s->reply_head + 1) % LICENSE_QUEUE;
        s->reply_count--;
    }
    pthread_mutex_unlock(&s->mutex);
    return ready;
}
void edition_service_snapshot(EditionService *s, CastEditionSnapshot *snapshot)
{
    if (!s) {
        unavailable_snapshot(snapshot);
        return;
    }
    pthread_mutex_lock(&s->mutex);
    *snapshot = s->snapshot;
    pthread_mutex_unlock(&s->mutex);
}
void edition_service_path(EditionService *s, const char *path)
{
    if (!s) {
        return;
    }
    pthread_mutex_lock(&s->mutex);
    if (strcmp(s->path, path)) {
        snprintf(s->path, sizeof s->path, "%s", path);
        unavailable_snapshot(&s->snapshot);
        s->refresh = true;
        pthread_cond_broadcast(&s->wake);
    }
    pthread_mutex_unlock(&s->mutex);
}
uint64_t edition_service_checked_at(EditionService *s)
{
    if (!s) {
        return 0;
    }
    pthread_mutex_lock(&s->mutex);
    uint64_t checked_at = s->checked_at;
    pthread_mutex_unlock(&s->mutex);
    return checked_at;
}
void edition_service_close(EditionService *s)
{
    if (!s) {
        return;
    }
    pthread_mutex_lock(&s->mutex);
    s->stop = true;
    pthread_cond_broadcast(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->workflow_thread, NULL);
    pthread_join(s->thread, NULL);
    cast_edition_shutdown();
    cast_license_session_close(s->retired_session);
    for (unsigned i = 0; i < s->count; ++i) {
        close(s->jobs[(s->head + i) % LICENSE_QUEUE].fd);
    }
    for (unsigned i = 0; i < s->workflow_count; ++i) {
        close(s->workflow_jobs[(s->workflow_head + i) % LICENSE_QUEUE].fd);
    }
    for (unsigned i = 0; i < s->reply_count; ++i) {
        close(s->replies[(s->reply_head + i) % LICENSE_QUEUE].fd);
    }
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}
