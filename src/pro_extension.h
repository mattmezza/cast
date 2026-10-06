#ifndef CAST_PRO_EXTENSION_H
#define CAST_PRO_EXTENSION_H
#include "cast.h"
#include "edition.h"
#define CAST_PRO_EXTENSION_API 2
typedef enum {
    CAST_SETTING_TEXT,
    CAST_SETTING_BOOL,
    CAST_SETTING_INT,
    CAST_SETTING_NUMBER,
    CAST_SETTING_ENUM,
    CAST_SETTING_COLOR,
    CAST_SETTING_PATH
} CastExtensionSettingType;
typedef struct {
    const char *section, *key;
    size_t max_bytes;
    int (*validate)(const char *, char *, size_t);
    const char *default_value;
    CastExtensionSettingType type;
    double minimum, maximum;
    const char *choices, *label, *unit, *group;
    /* Existing panel destination: source, annotations, audio, recording, notes.
     * Metadata only; private algorithms never enter the Community build. */
    const char *page;
    bool advanced, coupled;
} CastExtensionSetting;
typedef struct {
    const char *id;
    bool implemented, compiled, platform_supported;
    /* Cached readiness only: no capture, expensive probes or downloads. */
    bool (*dependency_ready)(void);
    bool (*active)(void);
} CastExtensionCapability;
typedef struct {
    const char *verb, *capability_id;
    bool safety_operation;
    /* Only verified worker requests invoke this callback. The module owns any
     * longer bounded job queue; safety operations remain usable on downgrade. */
    int (*execute)(const Config *, int, char **, char *, size_t);
    /* Classify safety/read-only subcommands of a shared verb before gating it. */
    bool (*operation_safe)(int, char **);
    bool (*matches)(int, char **);
} CastExtensionCommand;
typedef struct {
    const char *capability_id, *label, *command_verb;
    enum {
        CAST_UI_APPLICATION,
        CAST_UI_COMPOSE
    } location;
} CastExtensionUi;
typedef struct {
    const char *capability_id;
    /* Outside timing threads: schedule draining/finalization, never truncate. */
    void (*entitlement_changed)(bool entitled, int64_t grace_deadline);
    void (*shutdown)(void);
} CastExtensionLifecycle;
typedef struct {
    unsigned api_version;
    size_t struct_size;
    const char *provider_id;
    int (*verify_license)(const unsigned char *, size_t, const CastEditionIdentity *, int64_t,
                          CastLicenseInfo *, char *, size_t);
    int (*verify_update)(const unsigned char *, size_t, const char *, const CastEditionIdentity *,
                         CastUpdateMetadata *, char *, size_t);
    const CastExtensionSetting *settings;
    size_t settings_count;
    const CastExtensionCapability *capabilities;
    size_t capability_count;
    const CastExtensionCommand *commands;
    size_t command_count;
    const CastExtensionUi *ui;
    size_t ui_count;
    const CastExtensionLifecycle *lifecycle;
    size_t lifecycle_count;
    int (*validate_config)(const Config *, char *, size_t);
    const struct CastRuntimeHooks *runtime;
    /* Immutable bounded provider-owned tables. Runtime hooks are versioned with
     * this contract, remain optional and perform no dynamic plugin loading. */
} CastProExtension;
/* Linked from a private checkout in Pro only; incompatibility fails explicitly. */
const CastProExtension *cast_pro_extension_v2(void);
#endif
