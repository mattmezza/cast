#include "edition.h"
#include "ipc_identity.h"
#include "license_store.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void check_acknowledgements(void)
{
    /* License failures must retain their explanatory daemon message in both
     * CLI and panel clients, rather than becoming protocol errors. */
    char reply[] = "0 license diagnostic";
    for (char result = '0'; result <= '5'; ++result) {
        reply[0] = result;
        assert(cast_command_reply_valid(reply, sizeof reply - 1));
    }
    reply[0] = '6';
    assert(!cast_command_reply_valid(reply, sizeof reply - 1));
    assert(!cast_command_reply_valid("0", 1));
    assert(!cast_command_reply_valid("3:invalid", 9));
    assert(!cast_command_reply_valid("0 ", CAST_IPC_MAX));
}
static void check_store_sessions(void)
{
    char directory[] = "/tmp/cast-session-XXXXXX";
    assert(mkdtemp(directory));
    const char *previous_xdg = getenv("XDG_DATA_HOME");
    char *saved_xdg = previous_xdg ? strdup(previous_xdg) : NULL;
    char previous_directory[4096];
    assert(getcwd(previous_directory, sizeof previous_directory));
    assert(!setenv("XDG_DATA_HOME", directory, 1));
    char path[4096], alias[4096], error[256], output[4096];
    assert(!cast_license_store_path(NULL, path, sizeof path, error, sizeof error));
    /* One physical store, regardless of Community/Pro daemon/custom socket. */
    CastLicenseSession *community_daemon = cast_license_session_open(NULL, error, sizeof error);
    assert(community_daemon);
    assert(!cast_license_session_open(path, error, sizeof error));
    assert(strstr(error, "owned by another daemon/session"));
    snprintf(alias, sizeof alias, "%s/cast/../cast/./license.json", directory);
    assert(!cast_license_session_open(alias, error, sizeof error));
    assert(!cast_license_session_matches(community_daemon, alias, error, sizeof error));
    assert(!chdir(directory));
    assert(!cast_license_session_open("cast/license.json", error, sizeof error));
    assert(
        !cast_license_session_matches(community_daemon, "cast/license.json", error, sizeof error));
    assert(!chdir(previous_directory));
    const unsigned char previous[] = "accepted opaque snapshot";
    assert(!cast_license_store_write(path, previous, sizeof previous, error, sizeof error));
    assert(cast_license_mutate(CAST_LICENSE_REMOVE, NULL, path, 100, error, sizeof error));
    char *remove[] = {"license", "remove"};
    assert(cast_edition_command(2, remove, path, true, output, sizeof output, error,
                                sizeof error) == 5);
    unsigned char readback[64];
    size_t length;
    assert(!cast_license_store_read(path, readback, sizeof readback, &length, error, sizeof error));
    assert(length == sizeof previous && !memcmp(readback, previous, length));
    /* A different process cannot bypass the global store owner either. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        CastLicenseSession *pro_daemon = cast_license_session_open(alias, error, sizeof error);
        if (pro_daemon) {
            _exit(1);
        }
        if (!cast_license_mutate(CAST_LICENSE_REMOVE, NULL, path, 100, error, sizeof error)) {
            _exit(2);
        }
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char other[4096];
    snprintf(other, sizeof other, "%s/cast/another-license.json", directory);
    assert(cast_edition_command_in_session(community_daemon, 2, remove, other, output,
                                           sizeof output, error, sizeof error) == 5);
    CastEditionSnapshot snapshot;
    assert(!cast_edition_snapshot_in_session(community_daemon, path, 100, &snapshot, error,
                                             sizeof error));
    assert(snapshot.license_session_owned);
    assert(!cast_edition_command_in_session(community_daemon, 2, remove, path, output,
                                            sizeof output, error, sizeof error));
    assert(cast_license_store_read(path, readback, sizeof readback, &length, error, sizeof error) ==
           1);
    cast_license_session_close(community_daemon);
    CastLicenseSession *pro_daemon = cast_license_session_open(alias, error, sizeof error);
    assert(pro_daemon);
    cast_license_session_close(pro_daemon);
    /* Standalone mutation works only after the owning daemon/session stops. */
    assert(!cast_license_mutate(CAST_LICENSE_REMOVE, NULL, path, 100, error, sizeof error));
    char lock[4096];
    snprintf(lock, sizeof lock, "%s/cast/.license.json.lock", directory);
    assert(!unlink(lock));
    snprintf(lock, sizeof lock, "%s/cast/.license.json.session", directory);
    assert(!unlink(lock));
    snprintf(lock, sizeof lock, "%s/cast", directory);
    assert(!rmdir(lock));
    assert(!rmdir(directory));
    if (saved_xdg) {
        assert(!setenv("XDG_DATA_HOME", saved_xdg, 1));
        free(saved_xdg);
    } else {
        assert(!unsetenv("XDG_DATA_HOME"));
    }
}

int main(void)
{
    check_acknowledgements();
    check_store_sessions();
    char temp[] = "/tmp/cast-editions-XXXXXX";
    assert(mkdtemp(temp));
    char path[4096], error[256], output[4096];
    snprintf(path, sizeof path, "%s/license.json", temp);
    CastEditionSnapshot snapshot;
    assert(!cast_edition_snapshot(path, 100, &snapshot, error, sizeof error));
    assert(snapshot.schema == 1 && !snapshot.identity.pro);
    assert(snapshot.license.state == CAST_LICENSE_MISSING);
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; i++) {
        assert(!snapshot.features[i].implemented && !snapshot.features[i].compiled);
        assert(!snapshot.features[i].entitled && !snapshot.features[i].active);
        assert(snapshot.features[i].reason == CAST_REASON_COMMUNITY_BUILD);
        assert(cast_edition_require(snapshot.features[i].id, &snapshot, error, sizeof error));
        CastFeatureGrant grant;
        assert(cast_edition_grant(snapshot.features[i].id, &snapshot, 1, 100, 300, &grant, error,
                                  sizeof error));
        assert(!grant.active);
    }
    assert(cast_edition_require("unknown", &snapshot, error, sizeof error));
    assert(cast_edition_validate_setting("future", "feature", "true", error, sizeof error) == 1);
    char *features[] = {"features", "--json"};
    assert(!cast_edition_command(2, features, path, false, output, sizeof output, error,
                                 sizeof error));
    assert(strstr(output, "\"implemented\":false") && strstr(output, "community_build"));
    char *remove[] = {"license", "remove"};
    assert(cast_edition_command(2, remove, path, false, output, sizeof output, error,
                                sizeof error) == 5);
    int lock = cast_license_store_lock(path, error, sizeof error);
    assert(lock >= 0);
    assert(cast_license_store_lock(path, error, sizeof error) < 0);
    const unsigned char old[] = "accepted snapshot";
    assert(!cast_license_store_write(path, old, sizeof old, error, sizeof error));
    unsigned char buffer[64];
    size_t length;
    assert(!cast_license_store_read(path, buffer, sizeof buffer, &length, error, sizeof error));
    assert(length == sizeof old && !memcmp(buffer, old, length));
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    const unsigned char oversized[65] = {0};
    assert(!cast_license_store_write(path, oversized, sizeof oversized, error, sizeof error));
    assert(cast_license_store_read(path, buffer, sizeof buffer, &length, error, sizeof error));
    assert(!cast_license_store_remove(path, error, sizeof error));
    assert(!symlink("/dev/null", path));
    assert(cast_license_store_write(path, old, sizeof old, error, sizeof error));
    assert(cast_license_store_read(path, buffer, sizeof buffer, &length, error, sizeof error));
    assert(cast_license_store_remove(path, error, sizeof error));
    assert(!unlink(path));
    char link[4096], nested[4096];
    snprintf(link, sizeof link, "%s/link", temp);
    assert(!symlink(temp, link));
    snprintf(nested, sizeof nested, "%s/link/license.json", temp);
    assert(cast_license_store_lock(nested, error, sizeof error) < 0);
    assert(!unlink(link));
    close(lock);
    char lockpath[4096];
    snprintf(lockpath, sizeof lockpath, "%s/.license.json.lock", temp);
    assert(!unlink(lockpath));
    assert(!rmdir(temp));
    puts("Community edition, fail-closed gates and secure storage tests passed");
    return 0;
}
