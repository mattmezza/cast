/* MIT test double: verifies worker ordering, not a production license verifier. */
#include "edition_service.h"
#include "license_store.h"
#include "pro_extension.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t held_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t held_wake = PTHREAD_COND_INITIALIZER;
static bool entered, release_command;
static atomic_uint revoked;

static int verify(const unsigned char *data, size_t bytes, const CastEditionIdentity *identity,
                  int64_t now, CastLicenseInfo *info, char *error, size_t size)
{
    (void)identity;
    (void)now;
    memset(info, 0, sizeof *info);
    if (bytes != 8 || memcmp(data, "accepted", 8)) {
        info->state = CAST_LICENSE_INVALID;
        snprintf(error, size, "test-only bytes rejected");
        return -1;
    }
    info->state = CAST_LICENSE_VALID;
    info->perpetual = info->platform_supported = true;
    info->feature_count = 1;
    strcpy(info->features[0], "cinematic_zoom");
    return 0;
}
static bool ready(void)
{
    return true;
}
static int verify_update(const unsigned char *data, size_t bytes, const char *binary,
                         const CastEditionIdentity *identity, CastUpdateMetadata *metadata,
                         char *error, size_t size)
{
    (void)data;
    (void)bytes;
    (void)binary;
    (void)identity;
    (void)metadata;
    snprintf(error, size, "test double has no update verifier");
    return -1;
}
static int held_command(const Config *config, int argc, char **argv, char *output, size_t size)
{
    (void)config;
    assert(argc == 1 && !strcmp(argv[0], "test-held-preparation"));
    pthread_mutex_lock(&held_lock);
    entered = true;
    pthread_cond_broadcast(&held_wake);
    while (!release_command) {
        pthread_cond_wait(&held_wake, &held_lock);
    }
    pthread_mutex_unlock(&held_lock);
    snprintf(output, size, "test-only preparation completed");
    return 0;
}
static void entitlement_changed(bool entitled, int64_t deadline)
{
    (void)deadline;
    if (!entitled) {
        atomic_fetch_add(&revoked, 1);
    }
}
static const CastExtensionCapability capabilities[] = {{.id = "cinematic_zoom",
                                                        .implemented = true,
                                                        .compiled = true,
                                                        .platform_supported = true,
                                                        .dependency_ready = ready}};
static const CastExtensionCommand commands[] = {
    {.verb = "test-held-preparation", .capability_id = "cinematic_zoom", .execute = held_command}};
static const CastExtensionLifecycle lifecycle[] = {
    {.capability_id = "cinematic_zoom", .entitlement_changed = entitlement_changed}};
static const CastProExtension provider = {.api_version = CAST_PRO_EXTENSION_API,
                                          .struct_size = sizeof provider,
                                          .provider_id = "cast-pro",
                                          .verify_license = verify,
                                          .verify_update = verify_update,
                                          .capabilities = capabilities,
                                          .capability_count = 1,
                                          .commands = commands,
                                          .command_count = 1,
                                          .lifecycle = lifecycle,
                                          .lifecycle_count = 1};
const CastProExtension *cast_pro_extension_v2(void)
{
    return &provider;
}
static int submit(EditionService *service, const Config *config, int argc, char **argv)
{
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    char error[256];
    assert(fd >= 0);
    assert(!edition_service_submit(service, fd, argc, argv, config, error, sizeof error));
    return fd;
}
static EditionReply await_reply(EditionService *service, int expected)
{
    for (unsigned i = 0; i < 3000; ++i) {
        EditionReply reply;
        if (edition_service_reply(service, &reply)) {
            assert(reply.fd == expected);
            close(reply.fd);
            return reply;
        }
        usleep(1000);
    }
    assert(!"license mutation waited behind model preparation");
    return (EditionReply){0};
}
int main(void)
{
    char directory[] = "/tmp/cast-workflow-worker-XXXXXX", error[256];
    assert(mkdtemp(directory));
    Config config;
    config_defaults(&config);
    snprintf(config.licensing_file, sizeof config.licensing_file, "%s/license.json", directory);
    assert(!cast_license_store_write(config.licensing_file, (const unsigned char *)"accepted", 8,
                                     error, sizeof error));
    EditionService *service = edition_service_open(config.licensing_file, error, sizeof error);
    assert(service);
    CastEditionSnapshot snapshot;
    for (unsigned i = 0; i < 3000; ++i) {
        edition_service_snapshot(service, &snapshot);
        if (snapshot.license.state == CAST_LICENSE_VALID) {
            break;
        }
        usleep(1000);
    }
    assert(snapshot.license.state == CAST_LICENSE_VALID);
    char *held[] = {"test-held-preparation"};
    int held_fd = submit(service, &config, 1, held);
    pthread_mutex_lock(&held_lock);
    while (!entered) {
        pthread_cond_wait(&held_wake, &held_lock);
    }
    pthread_mutex_unlock(&held_lock);
    uint64_t checked = edition_service_checked_at(service);
    usleep(1200000);
    assert(edition_service_checked_at(service) > checked);
    char *remove[] = {"license", "remove"};
    int remove_fd = submit(service, &config, 2, remove);
    EditionReply reply = await_reply(service, remove_fd);
    assert(!reply.result);
    assert(access(config.licensing_file, F_OK));
    edition_service_snapshot(service, &snapshot);
    assert(snapshot.license.state == CAST_LICENSE_MISSING);
    assert(atomic_load(&revoked));
    pthread_mutex_lock(&held_lock);
    release_command = true;
    pthread_cond_broadcast(&held_wake);
    pthread_mutex_unlock(&held_lock);
    assert(!await_reply(service, held_fd).result);
    int denied_fd = submit(service, &config, 1, held);
    assert(await_reply(service, denied_fd).result == 5);
    edition_service_close(service);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.license.json.lock", directory);
    assert(!unlink(path));
    snprintf(path, sizeof path, "%s/.license.json.session", directory);
    assert(!unlink(path));
    assert(!rmdir(directory));
    puts("workflow worker: held preparation does not delay refresh/removal; revoked new work "
         "denied");
    return 0;
}
