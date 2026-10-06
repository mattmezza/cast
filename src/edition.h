#ifndef CAST_EDITION_H
#define CAST_EDITION_H
#include "cast.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CAST_EDITION_SCHEMA 1
#define CAST_FEATURE_COUNT 4
#define CAST_LICENSE_MAX 16384
#define CAST_LICENSE_FEATURE_MAX 64
#define CAST_EDITION_TEXT 129
typedef enum {
    CAST_REASON_ENABLED,
    CAST_REASON_COMMUNITY_BUILD,
    CAST_REASON_NOT_IMPLEMENTED,
    CAST_REASON_UNSUPPORTED_PLATFORM,
    CAST_REASON_DEPENDENCY_MISSING,
    CAST_REASON_LICENSE_MISSING,
    CAST_REASON_LICENSE_INVALID,
    CAST_REASON_LICENSE_NOT_YET_VALID,
    CAST_REASON_SUBSCRIPTION_EXPIRED,
    CAST_REASON_RELEASE_NOT_ELIGIBLE,
    CAST_REASON_VERIFIER_UNAVAILABLE
} CastFeatureReason;
typedef enum {
    CAST_LICENSE_MISSING,
    CAST_LICENSE_INVALID,
    CAST_LICENSE_NOT_YET_VALID,
    CAST_LICENSE_EXPIRED,
    CAST_LICENSE_INELIGIBLE,
    CAST_LICENSE_VALID,
    CAST_LICENSE_UNAVAILABLE
} CastLicenseState;
typedef struct {
    unsigned schema, extension_api;
    bool pro, official;
    int64_t release_timestamp;
    char edition[16], version[32], core_revision[65], private_revision[65], platform[32],
        media_profile[32], build_id[65];
} CastEditionIdentity;
typedef struct {
    CastLicenseState state;
    bool perpetual, platform_supported;
    int64_t issued_at, not_before, expires_at, updates_until;
    unsigned devices, seats, feature_count;
    char masked_id[32], key_id[CAST_EDITION_TEXT];
    char features[CAST_LICENSE_FEATURE_MAX][CAST_EDITION_TEXT];
} CastLicenseInfo;
typedef struct {
    char id[32], name[64];
    bool implemented, compiled, platform_supported, dependency_ready, entitled, active;
    CastFeatureReason reason;
} CastFeatureInfo;
typedef struct {
    unsigned schema;
    CastEditionIdentity identity;
    CastLicenseInfo license;
    CastFeatureInfo features[CAST_FEATURE_COUNT];
    bool pending_downgrade, license_session_owned;
} CastEditionSnapshot;
typedef struct {
    unsigned schema, extension_api;
    int64_t release_timestamp;
    char product[32], version[32], platform[32], media_profile[32];
    char core_revision[65], private_revision[65], sha256[65];
} CastUpdateMetadata;
typedef enum {
    CAST_LICENSE_IMPORT,
    CAST_LICENSE_RELOAD,
    CAST_LICENSE_REMOVE
} CastLicenseAction;
typedef struct CastLicenseSession CastLicenseSession;
/* One daemon/standalone writer owns a physical per-user store at a time,
 * independently of edition and socket names. Locks survive atomic replacement.
 * Open/close run on the licensing worker; no lockfile is deleted on close. */
CastLicenseSession *cast_license_session_open(const char *license_path, char *, size_t);
void cast_license_session_close(CastLicenseSession *);
int cast_license_session_matches(const CastLicenseSession *, const char *license_path, char *,
                                 size_t);
/* Functions run on the command/worker thread, never a realtime thread. Snapshots
 * are values owned by caller; no license payload or signature is exposed. */
const CastEditionIdentity *cast_edition_identity(void);
const char *cast_feature_reason_name(CastFeatureReason);
const char *cast_license_state_name(CastLicenseState);
int cast_edition_snapshot(const char *license_path, int64_t now, CastEditionSnapshot *, char *,
                          size_t);
int cast_edition_snapshot_in_session(CastLicenseSession *, const char *license_path, int64_t now,
                                     CastEditionSnapshot *, char *, size_t);
int cast_edition_require(const char *feature_id, const CastEditionSnapshot *, char *, size_t);
/* Returns 1 for unknown keys, 0 for a registered valid extension setting, -1
 * for invalid values/provider. Caller stores values in its config transaction. */
int cast_edition_validate_setting(const char *section, const char *key, const char *value, char *,
                                  size_t);
typedef struct {
    char feature_id[32];
    uint64_t generation;
    int64_t deadline;
    bool active;
} CastFeatureGrant;
/* Existing grants can finish to the deadline after a downgrade. New jobs always
 * require a fresh snapshot. No implemented module may exceed the 5 minute grace. */
int cast_edition_grant(const char *, const CastEditionSnapshot *, uint64_t, int64_t, unsigned,
                       CastFeatureGrant *, char *, size_t);
bool cast_edition_grant_active(const CastFeatureGrant *, uint64_t, int64_t);
void cast_edition_grant_finish(CastFeatureGrant *);
int cast_license_inspect(const char *path, int64_t now, CastLicenseInfo *, char *, size_t);
int cast_license_mutate(CastLicenseAction, const char *source, const char *license_path,
                        int64_t now, char *, size_t);
int cast_license_mutate_in_session(CastLicenseSession *, CastLicenseAction, const char *source,
                                   const char *license_path, int64_t now, char *, size_t);
/* argv begins with edition/features/license. Returns 0, malformed=2, invalid=3,
 * valid-but-ineligible=4, capability/I/O=5. allow_mutation requires the caller to
 * hold its daemon session lock or to execute inside authenticated daemon IPC. */
int cast_edition_command(int argc, char **argv, const char *license_path, bool allow_mutation,
                         char *output, size_t output_size, char *error, size_t error_size);
/* Authenticated daemon requests use its already-owned store session. A session
 * for a different directory/basename cannot authorize this command. */
int cast_edition_command_in_session(CastLicenseSession *, int argc, char **argv,
                                    const char *license_path, char *output, size_t output_size,
                                    char *error, size_t error_size);
/* Authenticates exact envelope payload and artifact hash, then validates edition,
 * platform/profile/API and release license eligibility. No network or install. */
int cast_edition_verify_update(const unsigned char *manifest, size_t length,
                               const char *binary_path, const char *license_path, int64_t now,
                               CastUpdateMetadata *, char *, size_t);
bool cast_edition_has_command(const char *);
bool cast_edition_has_request(int, char **);
/* 1 means unregistered; other values are the handler result. */
int cast_edition_dispatch(const Config *, const CastEditionSnapshot *, int, char **, char *,
                          size_t);
void cast_edition_transition(const CastEditionSnapshot *, CastEditionSnapshot *, int64_t);
void cast_edition_shutdown(void);
#endif
