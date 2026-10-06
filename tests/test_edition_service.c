#include "edition_service.h"
#include "license_store.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* No daemon listener is needed: each fd models ownership of an accepted peer. */
static EditionReply request(EditionService *service, const Config *config, int argc, char **argv)
{
    char error[CAST_ERR];
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(!edition_service_submit(service, fd, argc, argv, config, error, sizeof error));
    EditionReply reply;
    for (unsigned i = 0; i < 5000; ++i) {
        if (edition_service_reply(service, &reply)) {
            assert(reply.fd == fd);
            assert(fcntl(fd, F_GETFD) >= 0);
            close(fd);
            return reply;
        }
        usleep(1000);
    }
    assert(!"worker reply timeout");
    return (EditionReply){0};
}
static void await_owner(EditionService *service)
{
    CastEditionSnapshot snapshot;
    for (unsigned i = 0; i < 5000; ++i) {
        edition_service_snapshot(service, &snapshot);
        if (snapshot.license_session_owned) {
            assert(snapshot.schema == CAST_EDITION_SCHEMA);
            assert(!snapshot.identity.pro);
            assert(snapshot.features[0].id[0]);
            return;
        }
        usleep(1000);
    }
    assert(!"license ownership timeout");
}
static void check_bounds(EditionService *service, const Config *config)
{
    char error[CAST_ERR], oversized[CAST_IPC_MAX + 1];
    memset(oversized, 'a', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = 0;
    char *large[] = {oversized};
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(edition_service_submit(service, fd, 1, large, config, error, sizeof error));
    assert(strstr(error, "bound"));
    assert(fcntl(fd, F_GETFD) >= 0); /* Rejected requests retain caller ownership. */
    assert(edition_service_submit(service, fd, 0, large, config, error, sizeof error));
    assert(
        edition_service_submit(service, fd, CAST_MAX_ARGS + 1, large, config, error, sizeof error));
    assert(edition_service_submit(service, fd, 1, large, NULL, error, sizeof error));
    close(fd);
    char *edition[] = {"edition", "--json"};
    unsigned accepted = 0;
    for (; accepted < 20; ++accepted) {
        fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        assert(fd >= 0);
        if (edition_service_submit(service, fd, 2, edition, config, error, sizeof error)) {
            assert(strstr(error, "busy"));
            assert(fcntl(fd, F_GETFD) >= 0);
            close(fd);
            break;
        }
    }
    assert(accepted > 0 && accepted <= 8);
    assert(edition_service_busy(service));
    unsigned received = 0;
    for (unsigned i = 0; i < 5000 && received < accepted; ++i) {
        EditionReply reply;
        if (edition_service_reply(service, &reply)) {
            assert(!reply.result && strstr(reply.output, "\"edition\":\"community\""));
            close(reply.fd);
            ++received;
        } else {
            usleep(1000);
        }
    }
    assert(received == accepted);
    assert(!request(service, config, 2, edition).result);
}
int main(void)
{
    char directory[] = "/tmp/cast-license-worker-XXXXXX", error[CAST_ERR];
    assert(mkdtemp(directory));
    char path[PATH_MAX], alias[PATH_MAX], other[PATH_MAX];
    assert(snprintf(path, sizeof path, "%s/license.json", directory) > 0);
    assert(snprintf(alias, sizeof alias, "%s/./license.json", directory) > 0);
    assert(snprintf(other, sizeof other, "%s/other.json", directory) > 0);
    Config config;
    config_defaults(&config);
    snprintf(config.licensing_file, sizeof config.licensing_file, "%s", path);
    EditionService *first = edition_service_open(path, error, sizeof error);
    assert(first);
    await_owner(first);
    check_bounds(first, &config);
    const unsigned char payload[] = "opaque existing snapshot";
    assert(!cast_license_store_write(path, payload, sizeof payload, error, sizeof error));
    EditionService *second = edition_service_open(alias, error, sizeof error);
    assert(second);
    Config alias_config = config;
    snprintf(alias_config.licensing_file, sizeof alias_config.licensing_file, "%s", alias);
    char *remove[] = {"license", "remove"};
    EditionReply reply = request(second, &alias_config, 2, remove);
    assert(reply.result == 5);
    assert(!access(path, F_OK));
    CastEditionSnapshot snapshot;
    edition_service_snapshot(second, &snapshot);
    assert(!snapshot.license_session_owned);
    char *edition[] = {"edition", "--json"};
    reply = request(second, &alias_config, 2, edition);
    assert(!reply.result && strstr(reply.output, "community"));
    /* Mutation is complete before the worker acknowledges success. */
    reply = request(first, &config, 2, remove);
    assert(!reply.result && access(path, F_OK));
    edition_service_close(first);
    await_owner(second);
    assert(!cast_license_store_write(path, payload, sizeof payload, error, sizeof error));
    assert(!request(second, &alias_config, 2, remove).result);
    assert(access(path, F_OK));
    CastLicenseSession *blocked_path = cast_license_session_open(other, error, sizeof error);
    assert(blocked_path);
    edition_service_path(second, other);
    edition_service_snapshot(second, &snapshot);
    assert(!snapshot.license_session_owned && snapshot.license.state == CAST_LICENSE_UNAVAILABLE);
    cast_license_session_close(blocked_path);
    await_owner(second);
    /* The newly owned path cannot authorize a mutation of the previous store. */
    assert(request(second, &alias_config, 2, remove).result == 5);
    snprintf(config.licensing_file, sizeof config.licensing_file, "%s", other);
    assert(!cast_license_store_write(other, payload, sizeof payload, error, sizeof error));
    assert(!request(second, &config, 2, remove).result);
    assert(access(other, F_OK));
    edition_service_close(second);
    CastLicenseSession *released = cast_license_session_open(path, error, sizeof error);
    assert(released);
    cast_license_session_close(released);
    const char *locks[] = {".license.json.lock", ".license.json.session", ".other.json.lock",
                           ".other.json.session"};
    for (unsigned i = 0; i < sizeof locks / sizeof locks[0]; ++i) {
        char lock[PATH_MAX];
        snprintf(lock, sizeof lock, "%s/%s", directory, locks[i]);
        assert(!unlink(lock));
    }
    assert(!rmdir(directory));
    puts("edition worker: bounded requests, ownership, mutation acknowledgements and path changes "
         "pass");
    return 0;
}
