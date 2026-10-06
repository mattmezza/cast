#ifndef CAST_IPC_IDENTITY_H
#define CAST_IPC_IDENTITY_H
#include "edition.h"
#include <stddef.h>
#include <string.h>

/* Internal struct protocols are deliberately build-specific. A caller using an
 * explicit socket may not bypass the separate edition/application identities. */
#define CAST_COMMAND_HEADER_SIZE (6u + sizeof(CastEditionIdentity))
static inline size_t cast_command_header_write(void *buffer)
{
    memcpy(buffer, "CAST2\0", 6);
    memcpy((char *)buffer + 6, cast_edition_identity(), sizeof(CastEditionIdentity));
    return CAST_COMMAND_HEADER_SIZE;
}
static inline bool cast_identity_matches(const CastEditionIdentity *other)
{
    const CastEditionIdentity *own = cast_edition_identity();
    const unsigned char *bytes = (const unsigned char *)other;
    if (bytes[offsetof(CastEditionIdentity, pro)] > 1 ||
        bytes[offsetof(CastEditionIdentity, official)] > 1) {
        return false;
    }
    return other->schema == own->schema && other->extension_api == own->extension_api &&
           other->pro == own->pro && other->official == own->official &&
           other->release_timestamp == own->release_timestamp &&
           !memcmp(other->edition, own->edition, sizeof own->edition) &&
           !memcmp(other->version, own->version, sizeof own->version) &&
           !memcmp(other->core_revision, own->core_revision, sizeof own->core_revision) &&
           !memcmp(other->private_revision, own->private_revision, sizeof own->private_revision) &&
           !memcmp(other->platform, own->platform, sizeof own->platform) &&
           !memcmp(other->media_profile, own->media_profile, sizeof own->media_profile) &&
           !memcmp(other->build_id, own->build_id, sizeof own->build_id);
}
static inline bool cast_command_header_valid(const void *buffer, size_t size)
{
    if (size < CAST_COMMAND_HEADER_SIZE || memcmp(buffer, "CAST2\0", 6)) {
        return false;
    }
    CastEditionIdentity identity;
    memcpy(&identity, (const char *)buffer + 6, sizeof identity);
    return cast_identity_matches(&identity);
}
static inline bool cast_command_reply_valid(const char *buffer, size_t size)
{
    return size >= 2 && size < CAST_IPC_MAX && buffer[0] >= '0' && buffer[0] <= '5' &&
           buffer[1] == ' ';
}
/* Model/file preparation is deliberately worker-only and may take longer than
 * an ordinary command. Connection establishment still uses ipc_timeout_ms. */
static inline bool cast_settings_privacy_only(int argc, const char *const *argv)
{
    if (argc < 3 || !(argc & 1) || strcmp(argv[0], "settings")) {
        return false;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        bool disable =
            (!strcmp(argv[i], "transcription.enabled") || !strcmp(argv[i], "subtitles.virtual") ||
             !strcmp(argv[i], "subtitles.record") || !strcmp(argv[i], "subtitles.stream")) &&
            !strcmp(argv[i + 1], "false");
        disable |= !strcmp(argv[i], "subtitles.sidecar") && !strcmp(argv[i + 1], "none");
        if (!disable) {
            return false;
        }
    }
    return true;
}
static inline int cast_command_reply_timeout(int argc, const char *const *argv, int ordinary)
{
    if (argc < 1 || cast_settings_privacy_only(argc, argv)) {
        return ordinary;
    }
    bool preparation = !strcmp(argv[0], "config") && argc > 1 && !strcmp(argv[1], "reload");
    preparation |= !strcmp(argv[0], "notes") && argc > 1 &&
                   (!strcmp(argv[1], "open") || !strcmp(argv[1], "load") ||
                    !strcmp(argv[1], "start") || !strcmp(argv[1], "mode"));
    preparation |= !strcmp(argv[0], "transcription") && argc > 1 &&
                   (!strcmp(argv[1], "on") || !strcmp(argv[1], "model") || !strcmp(argv[1], "job"));
    preparation |= !strcmp(argv[0], "subtitles") && argc > 1 &&
                   (!strcmp(argv[1], "on") || !strcmp(argv[1], "lane") ||
                    (argc == 3 && !strcmp(argv[2], "on")));
    if (!strcmp(argv[0], "settings")) {
        for (int i = 1; i + 1 < argc; i += 2) {
            preparation |= !strncmp(argv[i], "transcription.", 14) ||
                           !strncmp(argv[i], "subtitles.", 10) || !strncmp(argv[i], "notes.", 6);
        }
    }
    return preparation && ordinary < 125000 ? 125000 : ordinary;
}
static inline bool cast_command_privacy_priority(int argc, const char *const *argv)
{
    if (cast_settings_privacy_only(argc, argv)) {
        return true;
    }
    if (argc == 1 && !strcmp(argv[0], "pause")) {
        return true;
    }
    if (argc < 2) {
        return false;
    }
    if ((!strcmp(argv[0], "virtual") || !strcmp(argv[0], "record") || !strcmp(argv[0], "stream")) &&
        (!strcmp(argv[1], "pause") || !strcmp(argv[1], "stop") || !strcmp(argv[1], "freeze") ||
         !strcmp(argv[1], "cut") || !strcmp(argv[1], "cancel") ||
         (argc == 3 && !strcmp(argv[1], "blur") && !strcmp(argv[2], "on")))) {
        return true;
    }
    if ((!strcmp(argv[0], "notes") && (!strcmp(argv[1], "pause") || !strcmp(argv[1], "close"))) ||
        (!strcmp(argv[0], "transcription") && !strcmp(argv[1], "off")) ||
        (!strcmp(argv[0], "pause") && !strcmp(argv[1], "all"))) {
        return true;
    }
    if (!strcmp(argv[0], "subtitles") &&
        (!strcmp(argv[1], "off") ||
         (argc >= 4 && !strcmp(argv[1], "lane") && !strcmp(argv[3], "off")) ||
         (argc == 3 &&
          (!strcmp(argv[1], "virtual") || !strcmp(argv[1], "record") ||
           !strcmp(argv[1], "stream")) &&
          !strcmp(argv[2], "off")) ||
         (argc == 3 && !strcmp(argv[1], "sidecar") && !strcmp(argv[2], "none")))) {
        return true;
    }
    return !strcmp(argv[0], "transcription") && argc >= 3 &&
           ((!strcmp(argv[1], "sidecar") && !strcmp(argv[2], "none")) ||
            (!strcmp(argv[1], "job") && !strcmp(argv[2], "cancel")));
}
#endif
