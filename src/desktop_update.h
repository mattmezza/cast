#ifndef CAST_DESKTOP_UPDATE_H
#define CAST_DESKTOP_UPDATE_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#define CAST_UPDATE_PATH_MAX 4096
#define CAST_UPDATE_MANIFEST_MAX 8192
/* Success is zero, errors -1. Check also returns 1 when no newer version exists,
 * or 2 when the platform channel has not yet been published (HTTP 404).
 * All work is synchronous: callers must use a worker and gate install on idle recording.
 * The caller must check Pro release eligibility against released_at before staging.
 */
typedef struct {
    char edition[16], platform[32], version[32], binary[128];
    char archive_url[2048], archive_sha256[65];
    uint64_t archive_size;
    int64_t released_at;
    unsigned char manifest[CAST_UPDATE_MANIFEST_MAX + 1];
    size_t manifest_size;
} CastDesktopUpdate;
typedef struct { char directory[CAST_UPDATE_PATH_MAX], payload[CAST_UPDATE_PATH_MAX]; } CastDesktopUpdateStage;
/* Set per-worker cancellation flag; its lifetime must cover all worker calls.
 * Pass NULL to clear before the flag is destroyed. */
void cast_update_cancel_scope(const atomic_bool *flag);
int cast_update_parse_manifest(const unsigned char *, size_t, const char *edition,
    const char *platform, const char *current_version, CastDesktopUpdate *, char *, size_t);
int cast_update_check(const char *edition, const char *platform, const char *current_version,
    CastDesktopUpdate *, char *, size_t);
int cast_update_stage(const CastDesktopUpdate *, CastDesktopUpdateStage *, char *, size_t);
int cast_update_stage_verify(CastDesktopUpdateStage *, const char *edition,
    const char *platform, const char *current_version, CastDesktopUpdate *, char *, size_t);
/* Testable extraction boundary. Requires an empty, private, user-owned directory.
 * Archives contain exactly one payload/ tree. All links must resolve within it. */
int cast_update_extract(const char *archive_path, const char *directory,
    const char *binary, char *, size_t);
/* Replace a user-owned complete runtime folder/app bundle. target must exist and
 * be an absolute canonical directory in a user-owned writable parent. Do not use
 * this on a loose executable or a shared bin directory. Run in a separate process
 * after the old recorder/daemon has exited. Linux: target is runtime root; macOS:
 * target is .app and payload contains Contents/. Returns only after atomic swap. Previous installation is retained in a sibling
 * .cast-update-previous-* directory for rollback.
 */
int cast_update_install(const char *payload, const char *target, char *, size_t);
void cast_update_stage_cleanup(CastDesktopUpdateStage *);
#endif
