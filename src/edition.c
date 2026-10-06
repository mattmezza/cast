#include "edition.h"
#include "cast.h"
#include "edition_extensions.h"
#include "license_store.h"
#include "pro_extension.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef CAST_VERSION
#define CAST_VERSION "development"
#endif
#ifndef CAST_CORE_REVISION
#define CAST_CORE_REVISION "working-tree"
#endif
#ifndef CAST_PRIVATE_REVISION
#define CAST_PRIVATE_REVISION "unprovisioned"
#endif
#ifndef CAST_RELEASE_TIMESTAMP
#define CAST_RELEASE_TIMESTAMP 0
#endif
#ifndef CAST_OFFICIAL_RELEASE
#define CAST_OFFICIAL_RELEASE 0
#endif
#ifndef CAST_MEDIA_PROFILE
#define CAST_MEDIA_PROFILE "system"
#endif
#ifndef CAST_BUILD_ID
#define CAST_BUILD_ID "development"
#endif
#ifndef CAST_PLATFORM
#define CAST_PLATFORM "linux"
#endif
#ifdef WITH_PRO
#define EDITION_NAME "pro"
#define IS_PRO true
#else
#define EDITION_NAME "community"
#define IS_PRO false
#endif
static const CastEditionIdentity identity = {.schema = 1,
                                             .extension_api = CAST_PRO_EXTENSION_API,
                                             .pro = IS_PRO,
                                             .official = CAST_OFFICIAL_RELEASE,
                                             .release_timestamp = CAST_RELEASE_TIMESTAMP,
                                             .edition = EDITION_NAME,
                                             .version = CAST_VERSION,
                                             .core_revision = CAST_CORE_REVISION,
                                             .private_revision = CAST_PRIVATE_REVISION,
                                             .platform = CAST_PLATFORM,
                                             .media_profile = CAST_MEDIA_PROFILE,
                                             .build_id = CAST_BUILD_ID};
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
const CastEditionIdentity *cast_edition_identity(void)
{
    return &identity;
}
const char *cast_feature_reason_name(CastFeatureReason r)
{
    static const char *names[] = {"enabled",
                                  "community_build",
                                  "not_implemented",
                                  "unsupported_platform",
                                  "dependency_missing",
                                  "license_missing",
                                  "license_invalid",
                                  "license_not_yet_valid",
                                  "subscription_expired",
                                  "release_not_eligible",
                                  "verifier_unavailable"};
    return (unsigned)r < sizeof names / sizeof *names ? names[r] : "license_invalid";
}
const char *cast_license_state_name(CastLicenseState s)
{
    static const char *names[] = {
        "license_missing",      "license_invalid", "license_not_yet_valid", "subscription_expired",
        "release_not_eligible", "valid",           "verifier_unavailable"};
    return (unsigned)s < sizeof names / sizeof *names ? names[s] : "license_invalid";
}
static const CastProExtension *provider(char *e, size_t n)
{
#ifdef WITH_PRO
    const CastProExtension *p = cast_edition_extensions();
    if (!p || p->api_version != CAST_PRO_EXTENSION_API || p->struct_size != sizeof *p ||
        !p->provider_id || strcmp(p->provider_id, "cast-pro") || !p->verify_license ||
        !p->verify_update) {
        fail(e, n, "incompatible or missing Pro extension API 1");
        return NULL;
    }
    return p;
#else
    (void)e;
    (void)n;
    return NULL;
#endif
}
/* Community contains no cryptography. The fixed installed Pro command must
 * identify this protocol explicitly; exit status alone is never verification. */
static int delegate(const char *path, CastLicenseInfo *info, char *e, size_t n)
{
    const char *tools[] = {"/usr/bin/cast-pro", "/usr/local/bin/cast-pro"};
    const char *tool = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (!access(tools[i], X_OK)) {
            tool = tools[i];
            break;
        }
    }
    if (!tool) {
        info->state = CAST_LICENSE_UNAVAILABLE;
        return fail(e, n,
                    "Community has no crypto verifier; install a compatible cast-pro to "
                    "verify/import licenses");
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) {
        return fail(e, n, "start Pro verifier: %s", strerror(errno));
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return fail(e, n, "start Pro verifier failed");
    }
    if (!pid) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
            close(null);
        }
        execl(tool, tool, "license", "verify-internal-v1", path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char reply[128];
    size_t used = 0;
    bool timed_out = false;
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (used < sizeof reply - 1) {
        clock_gettime(CLOCK_MONOTONIC, &current);
        int64_t elapsed =
            (current.tv_sec - start.tv_sec) * 1000 + (current.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= 5000) {
            timed_out = true;
            break;
        }
        struct pollfd pollfd = {.fd = fds[0], .events = POLLIN};
        int ready = poll(&pollfd, 1, (int)(5000 - elapsed));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            timed_out = true;
            break;
        }
        ssize_t r = read(fds[0], reply + used, sizeof reply - 1 - used);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        used += (size_t)r;
    }
    close(fds[0]);
    reply[used] = 0;
    int status;
    pid_t waited;
    while ((waited = waitpid(pid, &status, WNOHANG)) == 0) {
        clock_gettime(CLOCK_MONOTONIC, &current);
        if (current.tv_sec - start.tv_sec >= 5) {
            timed_out = true;
            break;
        }
        poll(NULL, 0, 10);
    }
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        info->state = CAST_LICENSE_UNAVAILABLE;
        return fail(e, n, "installed Pro verifier timed out");
    }
    unsigned state, perpetual, platform;
    long long issued, before, expires, updates;
    int consumed = 0;
    char masked[32];
    int fields = sscanf(
        reply, "CAST-PRO-VERIFY:1:%u:%u:%u:%lld:%lld:%lld:%lld:%31[A-Za-z0-9_.*-]\n%n", &state,
        &perpetual, &platform, &issued, &before, &expires, &updates, masked, &consumed);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || fields != 8 || !consumed ||
        reply[consumed] || state < CAST_LICENSE_NOT_YET_VALID || state > CAST_LICENSE_VALID ||
        perpetual > 1 || platform > 1 || issued < 0 || before < 0 || expires < 0 || updates < 0) {
        info->state = CAST_LICENSE_INVALID;
        return fail(e, n, "installed Pro verifier did not accept this license/protocol");
    }
    info->state = (CastLicenseState)state;
    info->perpetual = perpetual != 0;
    info->platform_supported = platform != 0;
    info->issued_at = issued;
    info->not_before = before;
    info->expires_at = expires;
    info->updates_until = updates;
    snprintf(info->masked_id, sizeof info->masked_id, "%s", masked);
    info->seats = 1;
    info->devices = 2;
    return 0;
}
static int verify(const unsigned char *b, size_t len, int64_t now, CastLicenseInfo *info, char *e,
                  size_t n)
{
    memset(info, 0, sizeof *info);
    info->state = CAST_LICENSE_INVALID;
    const CastProExtension *p = provider(e, n);
    if (!p) {
        info->state = CAST_LICENSE_UNAVAILABLE;
        return fail(e, n, "Pro verifier unavailable");
    }
    int result = p->verify_license(b, len, &identity, now, info, e, n);
    if (result) {
        memset(info, 0, sizeof *info);
        info->state = CAST_LICENSE_INVALID;
    }
    return result;
}
int cast_license_inspect(const char *path, int64_t now, CastLicenseInfo *info, char *e, size_t n)
{
    unsigned char b[CAST_LICENSE_MAX];
    size_t len = 0;
    memset(info, 0, sizeof *info);
    int r = cast_license_store_read(path, b, sizeof b, &len, e, n);
    if (r == 1) {
        info->state = CAST_LICENSE_MISSING;
        return 0;
    }
    if (r < 0) {
        info->state = CAST_LICENSE_INVALID;
        return -1;
    }
    if (!identity.pro) {
        return delegate(path, info, e, n);
    }
    return verify(b, len, now, info, e, n);
}
static bool license_grants(const CastLicenseInfo *license, const char *id)
{
    if (license->state != CAST_LICENSE_VALID || !license->platform_supported) {
        return false;
    }
    for (unsigned i = 0; i < license->feature_count && i < CAST_LICENSE_FEATURE_MAX; i++) {
        if (!strcmp(license->features[i], id)) {
            return true;
        }
    }
    return false;
}
static CastFeatureReason license_reason(CastLicenseState state)
{
    switch (state) {
    case CAST_LICENSE_MISSING:
        return CAST_REASON_LICENSE_MISSING;
    case CAST_LICENSE_NOT_YET_VALID:
        return CAST_REASON_LICENSE_NOT_YET_VALID;
    case CAST_LICENSE_EXPIRED:
        return CAST_REASON_SUBSCRIPTION_EXPIRED;
    case CAST_LICENSE_INELIGIBLE:
        return CAST_REASON_RELEASE_NOT_ELIGIBLE;
    case CAST_LICENSE_UNAVAILABLE:
        return CAST_REASON_VERIFIER_UNAVAILABLE;
    default:
        return CAST_REASON_LICENSE_INVALID;
    }
}
static void feature_snapshot(CastFeatureInfo *feature, const CastLicenseInfo *license)
{
    const CastExtensionCapability *compiled = cast_extension_capability(feature->id);
    feature->implemented = compiled && compiled->implemented;
    feature->compiled = compiled && compiled->compiled;
    feature->platform_supported = !compiled || compiled->platform_supported;
    feature->dependency_ready =
        compiled && compiled->dependency_ready && compiled->dependency_ready();
    feature->entitled = feature->implemented && feature->compiled && feature->platform_supported &&
                        feature->dependency_ready && license_grants(license, feature->id);
    feature->active =
        feature->implemented && feature->compiled && compiled->active && compiled->active();
    if (!identity.pro) {
        feature->reason = CAST_REASON_COMMUNITY_BUILD;
    } else if (!feature->implemented) {
        feature->reason = CAST_REASON_NOT_IMPLEMENTED;
    } else if (!feature->compiled) {
        feature->reason = CAST_REASON_DEPENDENCY_MISSING;
    } else if (!feature->platform_supported) {
        feature->reason = CAST_REASON_UNSUPPORTED_PLATFORM;
    } else if (!feature->dependency_ready) {
        feature->reason = CAST_REASON_DEPENDENCY_MISSING;
    } else if (license->state != CAST_LICENSE_VALID) {
        feature->reason = license_reason(license->state);
    } else if (!feature->entitled) {
        feature->reason = CAST_REASON_LICENSE_INVALID;
    } else {
        feature->reason = CAST_REASON_ENABLED;
    }
}
static void populate_features(CastEditionSnapshot *snapshot)
{
    static const char *const ids[] = {"editable_projects", "cinematic_zoom",
                                      "transcription_subtitles", "speech_teleprompter"};
    static const char *const names[] = {"Editable projects", "Cinematic zoom",
                                        "Transcription and subtitles", "Speech teleprompter"};
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; i++) {
        snprintf(snapshot->features[i].id, sizeof snapshot->features[i].id, "%s", ids[i]);
        snprintf(snapshot->features[i].name, sizeof snapshot->features[i].name, "%s", names[i]);
        feature_snapshot(&snapshot->features[i], &snapshot->license);
    }
}
static void initialize_snapshot(CastEditionSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->schema = CAST_EDITION_SCHEMA;
    snapshot->identity = identity;
    snapshot->license.state = CAST_LICENSE_UNAVAILABLE;
    populate_features(snapshot);
}
int cast_edition_snapshot(const char *configured, int64_t now, CastEditionSnapshot *snapshot,
                          char *error, size_t size)
{
    initialize_snapshot(snapshot);
    char path[4096];
    if (cast_license_store_path(configured, path, sizeof path, error, size)) {
        return -1;
    }
    cast_license_inspect(path, now, &snapshot->license, error, size);
    populate_features(snapshot);
    /* Invalid/missing licenses are successfully reportable read-only statuses. */
    return 0;
}
int cast_edition_snapshot_in_session(CastLicenseSession *session, const char *path, int64_t now,
                                     CastEditionSnapshot *snapshot, char *error, size_t size)
{
    if (cast_license_session_matches(session, path, error, size)) {
        initialize_snapshot(snapshot);
        return -1;
    }
    int result = cast_edition_snapshot(path, now, snapshot, error, size);
    if (!result) {
        snapshot->license_session_owned = true;
    }
    return result;
}
int cast_edition_require(const char *id, const CastEditionSnapshot *snapshot, char *error,
                         size_t size)
{
    int64_t now = (int64_t)time(NULL);
    if (snapshot && snapshot->license.state == CAST_LICENSE_VALID &&
        (now < snapshot->license.not_before ||
         (!snapshot->license.perpetual && snapshot->license.expires_at > 0 &&
          now >= snapshot->license.expires_at))) {
        snprintf(error, size, "license validity changed; refresh the authoritative snapshot");
        return -1;
    }
    if (!id || !snapshot || snapshot->schema != CAST_EDITION_SCHEMA ||
        snapshot->identity.pro != identity.pro ||
        strcmp(snapshot->identity.build_id, identity.build_id)) {
        return fail(error, size, "invalid or mismatched capability snapshot");
    }
    static const char *const known[] = {"editable_projects", "cinematic_zoom",
                                        "transcription_subtitles", "speech_teleprompter"};
    bool found = false;
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; i++) {
        if (!strcmp(id, known[i])) {
            found = true;
        }
    }
    if (!found) {
        return fail(error, size, "unknown capability ID");
    }
    CastFeatureInfo current = {0};
    snprintf(current.id, sizeof current.id, "%s", id);
    /* Registry/dependency truth is rechecked independently of client-authored flags. */
    feature_snapshot(&current, &snapshot->license);
    if (current.reason != CAST_REASON_ENABLED) {
        return fail(error, size, "%s: %s", id, cast_feature_reason_name(current.reason));
    }
    if (!snapshot->license_session_owned) {
        return fail(error, size, "%s: license store is not owned by this daemon/session", id);
    }
    return 0;
}
static int validate_schema(const CastExtensionSetting *settings, size_t count, const char *section,
                           const char *key, const char *value, char *error, size_t size)
{
    if (count > CAST_MAX_EXTENSION_SETTINGS || (count && !settings)) {
        return fail(error, size, "invalid extension schema");
    }
    for (size_t i = 0; i < count; i++) {
        const CastExtensionSetting *setting = &settings[i];
        if (!setting->section || !setting->key || !setting->max_bytes ||
            setting->max_bytes > sizeof(((ConfigExtensionValue *)0)->value)) {
            return fail(error, size, "invalid extension schema entry");
        }
        if (!strcmp(section, setting->section) && !strcmp(key, setting->key)) {
            if (strlen(value) >= setting->max_bytes) {
                return fail(error, size, "extension setting exceeds limit");
            }
            return cast_extension_metadata_validate(setting, value, error, size);
        }
    }
    return 1;
}
int cast_edition_validate_setting(const char *section, const char *key, const char *value,
                                  char *error, size_t size)
{
    if (!section || !key || !value) {
        return fail(error, size, "invalid extension setting");
    }
    size_t count = 0;
    const CastExtensionSetting *shared = cast_shared_extension_schema(&count);
    int public_result = validate_schema(shared, count, section, key, value, error, size);
    if (public_result < 0 || !identity.pro) {
        return public_result;
    }
    const CastProExtension *extension = provider(error, size);
    if (!extension) {
        return -1;
    }
    int private_result = validate_schema(extension->settings, extension->settings_count, section,
                                         key, value, error, size);
    return private_result == 1 ? public_result : private_result;
}
int cast_edition_grant(const char *id, const CastEditionSnapshot *s, uint64_t generation,
                       int64_t now, unsigned seconds, CastFeatureGrant *g, char *e, size_t n)
{
    memset(g, 0, sizeof *g);
    if (now < 0 || !seconds || seconds > 300 || now > INT64_MAX - seconds) {
        return fail(e, n, "invalid bounded capability lifetime");
    }
    if (cast_edition_require(id, s, e, n)) {
        return -1;
    }
    if (strlen(id) >= sizeof g->feature_id) {
        return fail(e, n, "capability ID exceeds limit");
    }
    strcpy(g->feature_id, id);
    g->generation = generation;
    g->deadline = now + seconds;
    g->active = true;
    return 0;
}
bool cast_edition_grant_active(const CastFeatureGrant *g, uint64_t generation, int64_t now)
{
    return g && g->active && g->generation == generation && now >= 0 && now < g->deadline;
}
void cast_edition_grant_finish(CastFeatureGrant *g)
{
    if (g) {
        memset(g, 0, sizeof *g);
    }
}
int cast_license_mutate_in_session(CastLicenseSession *session, CastLicenseAction action,
                                   const char *source, const char *configured, int64_t now, char *e,
                                   size_t n)
{
    if (cast_license_session_matches(session, configured, e, n)) {
        return -3;
    }
    char path[4096];
    if (cast_license_store_path(configured, path, sizeof path, e, n)) {
        return -1;
    }
    int lock = cast_license_store_lock(path, e, n);
    if (lock < 0) {
        return -3;
    }
    int result = 0;
    CastLicenseInfo info;
    if (action == CAST_LICENSE_REMOVE) {
        result = cast_license_store_remove(path, e, n);
    } else if (action == CAST_LICENSE_RELOAD) {
        result = cast_license_inspect(path, now, &info, e, n);
    } else if (action == CAST_LICENSE_IMPORT && source) {
        unsigned char b[CAST_LICENSE_MAX];
        size_t len = 0;
        result = cast_license_store_read(source, b, sizeof b, &len, e, n);
        if (result == 1) {
            result = fail(e, n, "license import source is missing");
        }
        if (!result && identity.pro) {
            result = verify(b, len, now, &info, e, n);
        }
        if (!result && !identity.pro) {
            /* Verify the exact copied snapshot: never verify a mutable source and
             * then import different bytes. Private temporary sibling is removed. */
            char snapshot[4096];
            int k = snprintf(snapshot, sizeof snapshot, "%s.verify-%ld", path, (long)getpid());
            if (k < 0 || (size_t)k >= sizeof snapshot) {
                result = fail(e, n, "verification snapshot path too long");
            } else if (cast_license_store_write(snapshot, b, len, e, n)) {
                result = -1;
            } else {
                result = delegate(snapshot, &info, e, n);
                char ignored[128];
                cast_license_store_remove(snapshot, ignored, sizeof ignored);
            }
        }
        if (!result) {
            result = cast_license_store_write(path, b, len, e, n);
        }
    } else {
        result = fail(e, n, "invalid license mutation");
    }
    close(lock);
    return result;
}
int cast_license_mutate(CastLicenseAction action, const char *source, const char *configured,
                        int64_t now, char *e, size_t n)
{
    CastLicenseSession *session = cast_license_session_open(configured, e, n);
    if (!session) {
        return -3;
    }
    int result = cast_license_mutate_in_session(session, action, source, configured, now, e, n);
    cast_license_session_close(session);
    return result;
}
static int license_exit(CastLicenseState state)
{
    return state == CAST_LICENSE_INVALID ? 3
           : state == CAST_LICENSE_INELIGIBLE || state == CAST_LICENSE_EXPIRED ||
                   state == CAST_LICENSE_NOT_YET_VALID
               ? 4
           : state == CAST_LICENSE_UNAVAILABLE ? 5
                                               : 0;
}
static int edition_command(CastLicenseSession *session, int argc, char **argv, const char *path,
                           bool scoped, bool mutate, char *out, size_t on, char *e, size_t n)
{
    if (on) {
        out[0] = 0;
    }
    if (argc < 1) {
        return 2;
    }
    bool json = argc > 1 && !strcmp(argv[argc - 1], "--json");
    int count = argc - (json ? 1 : 0);
    int64_t now = (int64_t)time(NULL);
    CastEditionSnapshot s;
    if (!strcmp(argv[0], "edition") || !strcmp(argv[0], "features")) {
        if (count != 1) {
            fail(e, n, "expected edition/features [--json]");
            return 2;
        }
        int snapshot_result = scoped
                                  ? cast_edition_snapshot_in_session(session, path, now, &s, e, n)
                                  : cast_edition_snapshot(path, now, &s, e, n);
        if (snapshot_result && !scoped) {
            return 5;
        }
        if (!strcmp(argv[0], "edition")) {
            snprintf(
                out, on,
                json ? "{\"schema\":1,\"edition\":\"%s\",\"version\":\"%s\",\"official\":%s,"
                       "\"extension_api\":2,\"release_timestamp\":%lld,\"core_revision\":\"%s\","
                       "\"private_revision\":\"%s\",\"platform\":\"%s\",\"media_profile\":\"%s\","
                       "\"build_id\":\"%s\"}\n"
                     : "Cast %s %s (%s; API 2; release %lld; core %s; private %s; %s/%s)\n",
                identity.edition, identity.version,
                identity.official ? (json ? "true" : "official")
                                  : (json ? "false" : "NONPRODUCTION"),
                (long long)identity.release_timestamp, identity.core_revision,
                identity.private_revision, identity.platform, identity.media_profile,
                identity.build_id);
        } else {
            size_t used = 0;
            int r = snprintf(out, on, json ? "{\"schema\":1,\"features\":[" : "");
            if (r > 0) {
                used = (size_t)r;
            }
            for (unsigned i = 0; i < CAST_FEATURE_COUNT && used < on; i++) {
                const CastFeatureInfo *f = &s.features[i];
                if (json) {
                    r = snprintf(
                        out + used, on - used,
                        "%s{\"id\":\"%s\",\"implemented\":%s,\"compiled\":%s,"
                        "\"platform_supported\":%s,\"dependency_ready\":%s,\"entitled\":%s,"
                        "\"active\":%s,\"reason\":\"%s\"}",
                        i ? "," : "", f->id, f->implemented ? "true" : "false",
                        f->compiled ? "true" : "false", f->platform_supported ? "true" : "false",
                        f->dependency_ready ? "true" : "false", f->entitled ? "true" : "false",
                        f->active ? "true" : "false", cast_feature_reason_name(f->reason));
                } else {
                    r = snprintf(out + used, on - used, "%s: %s (%s)\n", f->id,
                                 cast_feature_reason_name(f->reason),
                                 f->implemented ? "implemented" : "planned, not yet available");
                }
                if (r > 0) {
                    used += (size_t)r;
                }
            }
            if (json && used < on) {
                snprintf(out + used, on - used, "]}\n");
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "license") || count < 2) {
        fail(e, n, "expected license status|inspect|import|reload|remove");
        return 2;
    }
    const char *verb = argv[1];
    CastLicenseInfo info;
    if (!strcmp(verb, "verify-internal-v1")) {
        if (!identity.pro || argc != 3) {
            return 5;
        }
        int r = cast_license_inspect(argv[2], now, &info, e, n);
        if (r || info.state == CAST_LICENSE_MISSING || info.state == CAST_LICENSE_INVALID ||
            info.state == CAST_LICENSE_UNAVAILABLE) {
            return 3;
        }
        snprintf(out, on, "CAST-PRO-VERIFY:1:%u:%u:%u:%lld:%lld:%lld:%lld:%s\n",
                 (unsigned)info.state, info.perpetual, info.platform_supported,
                 (long long)info.issued_at, (long long)info.not_before, (long long)info.expires_at,
                 (long long)info.updates_until, info.masked_id);
        return 0;
    }
    if (!strcmp(verb, "status") || !strcmp(verb, "inspect")) {
        bool inspect = !strcmp(verb, "inspect");
        if (count != (inspect ? 3 : 2)) {
            fail(e, n, "expected license status [--json] or inspect FILE [--json]");
            return 2;
        }
        int r;
        if (inspect) {
            r = cast_license_inspect(argv[2], now, &info, e, n);
        } else {
            r = scoped ? cast_edition_snapshot_in_session(session, path, now, &s, e, n)
                       : cast_edition_snapshot(path, now, &s, e, n);
            if (r && !scoped) {
                return 5;
            }
            info = s.license;
        }
        snprintf(out, on,
                 json ? "{\"schema\":1,\"edition\":\"%s\",\"state\":\"%s\",\"kind\":\"%s\","
                        "\"masked_id\":\"%s\",\"not_before\":%lld,\"expires_at\":%lld,\"updates_"
                        "until\":%lld,\"pending_downgrade\":false}\n"
                      : "Cast %s license: %s; kind %s; ID %s; not-before %lld; expires %lld; "
                        "updates-until %lld\n",
                 identity.edition, cast_license_state_name(info.state),
                 info.state == CAST_LICENSE_MISSING || info.state == CAST_LICENSE_UNAVAILABLE ||
                         info.state == CAST_LICENSE_INVALID
                     ? "none"
                 : info.perpetual ? "perpetual"
                                  : "subscription",
                 info.masked_id, (long long)info.not_before, (long long)info.expires_at,
                 (long long)info.updates_until);
        return inspect ? (r == -2 ? 2 : license_exit(info.state)) : 0;
    }
    CastLicenseAction action;
    if (!strcmp(verb, "import") && count == 3) {
        action = CAST_LICENSE_IMPORT;
    } else if (!strcmp(verb, "reload") && count == 2) {
        action = CAST_LICENSE_RELOAD;
    } else if (!strcmp(verb, "remove") && count == 2) {
        action = CAST_LICENSE_REMOVE;
    } else {
        fail(e, n, "invalid license command arguments");
        return 2;
    }
    if (json) {
        fail(e, n, "--json is supported for read-only license commands");
        return 2;
    }
    if (!mutate) {
        fail(e, n,
             "license mutation requires authenticated daemon IPC or exclusive absent-daemon "
             "session lock");
        return 5;
    }
    if (scoped && cast_license_session_matches(session, path, e, n)) {
        return 5;
    }
    int result =
        scoped
            ? cast_license_mutate_in_session(
                  session, action, action == CAST_LICENSE_IMPORT ? argv[2] : NULL, path, now, e, n)
            : cast_license_mutate(action, action == CAST_LICENSE_IMPORT ? argv[2] : NULL, path, now,
                                  e, n);
    if (result) {
        return result == -2 ? 2 : result == -3 ? 5 : 3;
    }
    snprintf(out, on, "License %s acknowledged. %s\n", verb,
             identity.pro ? "All premium workflows are currently planned/unimplemented."
                          : "Community remains Community; importing cannot add absent Pro code.");
    return 0;
}
int cast_edition_command(int argc, char **argv, const char *path, bool mutate, char *out, size_t on,
                         char *e, size_t n)
{
    return edition_command(NULL, argc, argv, path, false, mutate, out, on, e, n);
}
int cast_edition_command_in_session(CastLicenseSession *session, int argc, char **argv,
                                    const char *path, char *out, size_t on, char *e, size_t n)
{
    if (on) {
        out[0] = 0;
    }
    return edition_command(session, argc, argv, path, true, true, out, on, e, n);
}
int cast_edition_verify_update(const unsigned char *b, size_t len, const char *binary,
                               const char *path, int64_t now, CastUpdateMetadata *m, char *e,
                               size_t n)
{
    memset(m, 0, sizeof *m);
    const CastProExtension *p = provider(e, n);
    if (!p) {
        return fail(e, n, "authenticated Pro update verifier unavailable");
    }
    if (p->verify_update(b, len, binary, &identity, m, e, n)) {
        memset(m, 0, sizeof *m);
        return -1;
    }
    char resolved[4096];
    CastLicenseInfo info;
    if (cast_license_store_path(path, resolved, sizeof resolved, e, n)) {
        return -1;
    }
    if (cast_license_inspect(resolved, now, &info, e, n) || info.state == CAST_LICENSE_MISSING ||
        info.state == CAST_LICENSE_INVALID || info.state == CAST_LICENSE_UNAVAILABLE ||
        !info.platform_supported) {
        return fail(e, n, "update requires a valid eligible license");
    }
    if (info.state == CAST_LICENSE_NOT_YET_VALID || info.state == CAST_LICENSE_EXPIRED ||
        (info.perpetual && m->release_timestamp > info.updates_until)) {
        fail(e, n, "release_not_eligible: update is outside the signed license terms");
        return 4;
    }
    return 0;
}
