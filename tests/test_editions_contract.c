/* MIT test-only fake provider. This is not a license verifier or Pro module. */
#include "edition.h"
#include "license_store.h"
#include "pro_extension.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int setting_validator(const char *value, char *error, size_t size)
{
    if (!strcmp(value, "dormant")) {
        return 0;
    }
    snprintf(error, size, "test setting requires dormant");
    return -1;
}
static int fake_license(const unsigned char *bytes, size_t length, const CastEditionIdentity *id,
                        int64_t now, CastLicenseInfo *info, char *error, size_t size)
{
    (void)id;
    (void)now;
    memset(info, 0, sizeof *info);
    if (length != 13 || memcmp(bytes, "test-accepted", 13)) {
        info->state = CAST_LICENSE_INVALID;
        snprintf(error, size, "test fake rejected bytes");
        return -1;
    }
    info->state = CAST_LICENSE_VALID;
    info->perpetual = info->platform_supported = true;
    info->updates_until = 300;
    info->feature_count = 1;
    strcpy(info->features[0], "editable_projects");
    return 0;
}
static int fake_update(const unsigned char *bytes, size_t length, const char *binary,
                       const CastEditionIdentity *id, CastUpdateMetadata *metadata, char *error,
                       size_t size)
{
    (void)bytes;
    (void)length;
    (void)binary;
    (void)id;
    memset(metadata, 0xff, sizeof *metadata);
    snprintf(error, size, "test fake has no update verifier");
    return -1;
}
static const CastExtensionSetting setting = {
    .section = "future_test", .key = "mode", .max_bytes = 16, .validate = setting_validator};
static CastProExtension fake = {.api_version = CAST_PRO_EXTENSION_API,
                                .struct_size = sizeof fake,
                                .provider_id = "cast-pro",
                                .verify_license = fake_license,
                                .verify_update = fake_update,
                                .settings = &setting,
                                .settings_count = 1};
const CastProExtension *cast_pro_extension_v2(void)
{
    return &fake;
}
static bool dependency_ready = true;
static bool fake_active;
static bool ready_callback(void)
{
    return dependency_ready;
}
static bool active_callback(void)
{
    return fake_active;
}
static CastExtensionCapability capability = {.id = "editable_projects",
                                             .implemented = true,
                                             .compiled = true,
                                             .platform_supported = true,
                                             .dependency_ready = ready_callback,
                                             .active = active_callback};
static unsigned command_calls, safety_calls, transition_calls, shutdown_calls;
static bool transition_entitled;
static int64_t transition_deadline;
static int execute_callback(const Config *config, int argc, char **argv, char *error, size_t size)
{
    (void)error;
    (void)size;
    assert(config);
    assert(!strcmp(config_extension_value(config, "future_test", "mode"), "dormant"));
    assert(argc == 1);
    if (!strcmp(argv[0], "test-finalize")) {
        safety_calls++;
    } else {
        assert(!strcmp(argv[0], "test-start"));
        command_calls++;
    }
    return 0;
}
static void transition_callback(bool entitled, int64_t deadline)
{
    transition_calls++;
    transition_entitled = entitled;
    transition_deadline = deadline;
}
static void shutdown_callback(void)
{
    shutdown_calls++;
}
static const CastExtensionCommand commands[] = {
    {.verb = "test-start", .capability_id = "editable_projects", .execute = execute_callback},
    {.verb = "test-finalize",
     .capability_id = "editable_projects",
     .safety_operation = true,
     .execute = execute_callback}};
static const CastExtensionLifecycle lifecycle = {.capability_id = "editable_projects",
                                                 .entitlement_changed = transition_callback,
                                                 .shutdown = shutdown_callback};
static void write_config(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(!fclose(file));
}
static void check_config(Config *config, const char *directory)
{
    char path[4096], error[256];
    snprintf(path, sizeof path, "%s/config.ini", directory);
    config_defaults(config);
    assert(!config_validate(config, error, sizeof error));
    assert(!config_set_value(config, "future_test.mode", "dormant", error, sizeof error));
    assert(!strcmp(config_extension_value(config, "future_test", "mode"), "dormant"));
    assert(!config_validate(config, error, sizeof error));
    Config previous = *config;
    assert(config_set_value(config, "future_test.mode", "invalid", error, sizeof error));
    assert(!memcmp(config, &previous, sizeof previous));
    assert(config_set_value(config, "future_test.unknown", "dormant", error, sizeof error));
    assert(config_set_value(config, "record.typo", "dormant", error, sizeof error));
    assert(!memcmp(config, &previous, sizeof previous));
    write_config(path, "[future_test]\nmode=dormant\n");
    assert(!config_load(config, path, true, error, sizeof error));
    assert(!strcmp(config_extension_value(config, "future_test", "mode"), "dormant"));
    unsigned future_index = 0;
    while (future_index < config->extension_setting_count &&
           strcmp(config->extension_settings[future_index].section, "future_test")) {
        future_index++;
    }
    assert(future_index < config->extension_setting_count);
    assert(!strcmp(config->extension_settings[future_index].key, "mode"));
    assert(!strcmp(config->extension_settings[future_index].value, "dormant"));
    previous = *config;
    const char *invalid[] = {"[future_test]\nmode=invalid\n", "[future_test]\nunknown=dormant\n",
                             "[record]\ntypo=1\n", "[unregistered]\nmode=dormant\n",
                             "[future_test]\nmode=dormant\nmode=dormant\n"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        write_config(path, invalid[i]);
        assert(config_load(config, path, true, error, sizeof error));
        assert(!memcmp(config, &previous, sizeof previous));
    }
    assert(config->extension_setting_count < CAST_MAX_EXTENSION_SETTINGS);
    config->extension_settings[config->extension_setting_count++] =
        config->extension_settings[future_index];
    assert(config_validate(config, error, sizeof error));
    *config = previous;
    strcpy(config->extension_settings[future_index].value, "invalid");
    assert(config_validate(config, error, sizeof error));
    *config = previous;
    assert(!unlink(path));
}
int main(void)
{
    char directory[] = "/tmp/cast-contract-XXXXXX";
    assert(mkdtemp(directory));
    char path[4096], error[256];
    snprintf(path, sizeof path, "%s/license.json", directory);
    assert(cast_edition_identity()->pro);
    Config config;
    check_config(&config, directory);
    assert(!cast_edition_validate_setting("future_test", "mode", "dormant", error, sizeof error));
    assert(cast_edition_validate_setting("future_test", "mode", "invalid", error, sizeof error) <
           0);
    assert(cast_edition_validate_setting("future_test", "mode", "0123456789012345", error,
                                         sizeof error) < 0);
    assert(cast_edition_validate_setting("future_test", "unknown", "dormant", error,
                                         sizeof error) == 1);
    fake.settings_count = CAST_MAX_EXTENSION_SETTINGS + 1;
    assert(cast_edition_validate_setting("future_test", "mode", "dormant", error, sizeof error) <
           0);
    fake.settings_count = 1;
    assert(!cast_license_store_write(path, (const unsigned char *)"test-accepted", 13, error,
                                     sizeof error));
    CastEditionSnapshot snapshot;
    assert(!cast_edition_snapshot(path, 100, &snapshot, error, sizeof error));
    assert(snapshot.license.state == CAST_LICENSE_VALID);
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; i++) {
        assert(snapshot.features[i].reason == CAST_REASON_NOT_IMPLEMENTED);
        /* A bypassing caller cannot fake compiled code by authoring a snapshot. */
        snapshot.features[i].implemented = snapshot.features[i].compiled = true;
        snapshot.features[i].platform_supported = snapshot.features[i].dependency_ready = true;
        snapshot.features[i].entitled = snapshot.features[i].active = true;
        snapshot.features[i].reason = CAST_REASON_ENABLED;
        assert(cast_edition_require(snapshot.features[i].id, &snapshot, error, sizeof error));
        CastFeatureGrant grant;
        assert(cast_edition_grant(snapshot.features[i].id, &snapshot, 1, 100, 300, &grant, error,
                                  sizeof error));
        assert(!grant.active);
    }
    CastFeatureGrant existing = {.active = true, .generation = 10, .deadline = 400};
    assert(cast_edition_grant_active(&existing, 10, 399));
    assert(!cast_edition_grant_active(&existing, 11, 399));
    assert(!cast_edition_grant_active(&existing, 10, 400));
    assert(!cast_edition_grant_active(&existing, 10, -1));
    cast_edition_grant_finish(&existing);
    assert(!existing.active);
    /* Registered test-only module: precedence and ownership are independently
     * enforced, and a valid license claim alone cannot start a disconnected job. */
    fake.commands = commands;
    fake.command_count = 2;
    fake.lifecycle = &lifecycle;
    fake.lifecycle_count = 1;
    fake.capabilities = &capability;
    fake.capability_count = 1;
    assert(!cast_edition_snapshot(path, 100, &snapshot, error, sizeof error));
    assert(snapshot.features[0].reason == CAST_REASON_ENABLED);
    assert(cast_edition_require("editable_projects", &snapshot, error, sizeof error));
    char *start[] = {"test-start"}, *finalize[] = {"test-finalize"}, *unknown[] = {"unknown"};
    assert(cast_edition_has_command("test-start"));
    assert(!cast_edition_has_command("unknown"));
    assert(cast_edition_dispatch(&config, &snapshot, 1, start, error, sizeof error) == 5);
    assert(!command_calls);
    assert(!cast_edition_dispatch(&config, &snapshot, 1, finalize, error, sizeof error));
    assert(safety_calls == 1);
    assert(cast_edition_dispatch(&config, &snapshot, 1, unknown, error, sizeof error) == 1);
    CastLicenseSession *session = cast_license_session_open(path, error, sizeof error);
    assert(session);
    assert(!cast_edition_snapshot_in_session(session, path, 100, &snapshot, error, sizeof error));
    assert(!cast_edition_require("editable_projects", &snapshot, error, sizeof error));
    assert(!cast_edition_dispatch(&config, &snapshot, 1, start, error, sizeof error));
    assert(command_calls == 1);
    CastEditionSnapshot unavailable;
    assert(cast_edition_snapshot_in_session(NULL, path, 100, &unavailable, error, sizeof error));
    assert(unavailable.license.state == CAST_LICENSE_UNAVAILABLE);
    assert(!unavailable.license_session_owned);
    assert(!strcmp(unavailable.features[0].id, "editable_projects"));
    assert(!strcmp(unavailable.features[0].name, "Editable projects"));
    assert(unavailable.features[0].reason == CAST_REASON_VERIFIER_UNAVAILABLE);
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; ++i) {
        assert(unavailable.features[i].id[0] && unavailable.features[i].name[0]);
        assert(!unavailable.features[i].entitled);
    }
    assert(cast_edition_dispatch(&config, &unavailable, 1, start, error, sizeof error) == 5);
    assert(!cast_edition_dispatch(&config, &unavailable, 1, finalize, error, sizeof error));
    assert(command_calls == 1 && safety_calls == 2);
    cast_edition_transition(&unavailable, &snapshot, 100);
    assert(transition_calls == 1 && transition_entitled && transition_deadline == 400);
    fake_active = true;
    cast_edition_transition(&snapshot, &unavailable, 150);
    assert(transition_calls == 2 && !transition_entitled && transition_deadline == 450);
    assert(unavailable.pending_downgrade);
    cast_edition_transition(&unavailable, &unavailable, 151);
    assert(transition_calls == 2);
    cast_edition_shutdown();
    assert(shutdown_calls == 1);
    fake_active = false;
    CastFeatureGrant granted;
    assert(!cast_edition_grant("editable_projects", &snapshot, 10, 100, 300, &granted, error,
                               sizeof error));
    snapshot.license.state = CAST_LICENSE_EXPIRED;
    assert(cast_edition_require("editable_projects", &snapshot, error, sizeof error));
    assert(cast_edition_grant_active(&granted, 10, 399));
    assert(!cast_edition_grant_active(&granted, 10, 400));
    dependency_ready = false;
    assert(!cast_edition_snapshot_in_session(session, path, 100, &snapshot, error, sizeof error));
    assert(snapshot.features[0].reason == CAST_REASON_DEPENDENCY_MISSING);
    capability.platform_supported = false;
    assert(!cast_edition_snapshot_in_session(session, path, 100, &snapshot, error, sizeof error));
    assert(snapshot.features[0].reason == CAST_REASON_UNSUPPORTED_PLATFORM);
    capability.implemented = false;
    assert(!cast_edition_snapshot_in_session(session, path, 100, &snapshot, error, sizeof error));
    assert(snapshot.features[0].reason == CAST_REASON_NOT_IMPLEMENTED);
    cast_license_session_close(session);
    fake.commands = NULL;
    fake.command_count = 0;
    fake.lifecycle = NULL;
    fake.lifecycle_count = 0;
    fake.capabilities = NULL;
    fake.capability_count = 0;
    fake.api_version = CAST_PRO_EXTENSION_API + 1;
    assert(!cast_edition_snapshot(path, 100, &snapshot, error, sizeof error));
    assert(snapshot.license.state == CAST_LICENSE_UNAVAILABLE);
    assert(cast_edition_validate_setting("future_test", "mode", "dormant", error, sizeof error) <
           0);
    fake.api_version = CAST_PRO_EXTENSION_API;
    fake.struct_size--;
    assert(cast_edition_validate_setting("future_test", "mode", "dormant", error, sizeof error) <
           0);
    fake.struct_size = sizeof fake;
    CastUpdateMetadata metadata;
    memset(&metadata, 0xff, sizeof metadata);
    assert(cast_edition_verify_update(NULL, 0, path, path, 100, &metadata, error, sizeof error));
    assert(metadata.release_timestamp == 0);
    assert(!cast_license_store_remove(path, error, sizeof error));
    char session_path[4096];
    snprintf(session_path, sizeof session_path, "%s/.license.json.session", directory);
    assert(!unlink(session_path));
    assert(!rmdir(directory));
    puts("Fake-provider API, transactional config, dispatch, lifecycle and bounded grant tests "
         "passed");
    return 0;
}
