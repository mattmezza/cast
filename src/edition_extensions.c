#include "edition_extensions.h"
#include "pro_runtime.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool known_id(const char *id)
{
    static const char *const ids[] = {"editable_projects", "cinematic_zoom",
                                      "transcription_subtitles", "speech_teleprompter"};
    if (!id) {
        return false;
    }
    for (unsigned i = 0; i < CAST_FEATURE_COUNT; ++i) {
        if (!strcmp(id, ids[i])) {
            return true;
        }
    }
    return false;
}
const CastProExtension *cast_edition_extensions(void)
{
#ifdef WITH_PRO
    const CastProExtension *p = cast_pro_extension_v2();
    if (!p || p->api_version != CAST_PRO_EXTENSION_API || p->struct_size != sizeof *p ||
        !p->provider_id || strcmp(p->provider_id, "cast-pro") || !p->verify_license ||
        !p->verify_update || p->capability_count > CAST_FEATURE_COUNT || p->command_count > 32 ||
        p->ui_count > 16 || p->lifecycle_count > CAST_FEATURE_COUNT ||
        p->settings_count > CAST_MAX_EXTENSION_SETTINGS ||
        (p->capability_count && !p->capabilities) || (p->command_count && !p->commands) ||
        (p->ui_count && !p->ui) || (p->lifecycle_count && !p->lifecycle) ||
        (p->settings_count && !p->settings)) {
        return NULL;
    }
    for (size_t i = 0; i < p->capability_count; ++i) {
        if (!known_id(p->capabilities[i].id)) {
            return NULL;
        }
        for (size_t j = 0; j < i; ++j) {
            if (!strcmp(p->capabilities[i].id, p->capabilities[j].id)) {
                return NULL;
            }
        }
    }
    for (size_t i = 0; i < p->command_count; ++i) {
        const CastExtensionCommand *c = &p->commands[i];
        if (!c->verb || !*c->verb || strlen(c->verb) > 63 || !known_id(c->capability_id) ||
            !c->execute) {
            return NULL;
        }
        for (size_t j = 0; j < i; ++j) {
            if (!strcmp(c->verb, p->commands[j].verb)) {
                return NULL;
            }
        }
    }
    for (size_t i = 0; i < p->ui_count; ++i) {
        const CastExtensionUi *ui = &p->ui[i];
        if (!known_id(ui->capability_id) || !ui->label || strlen(ui->label) > 128 ||
            !ui->command_verb || ui->location < CAST_UI_APPLICATION ||
            ui->location > CAST_UI_COMPOSE) {
            return NULL;
        }
    }
    for (size_t i = 0; i < p->lifecycle_count; ++i) {
        if (!known_id(p->lifecycle[i].capability_id)) {
            return NULL;
        }
    }
    return p;
#else
    (void)known_id;
    return NULL;
#endif
}
const CastExtensionCapability *cast_extension_capability(const char *id)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p || !id) {
        return NULL;
    }
    for (size_t i = 0; i < p->capability_count; ++i) {
        if (!strcmp(id, p->capabilities[i].id)) {
            return &p->capabilities[i];
        }
    }
    return NULL;
}
bool cast_edition_has_command(const char *verb)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p || !verb) {
        return false;
    }
    for (size_t i = 0; i < p->command_count; ++i) {
        if (!strcmp(verb, p->commands[i].verb)) {
            return true;
        }
    }
    return false;
}
int cast_edition_dispatch(const Config *config, const CastEditionSnapshot *snapshot, int argc,
                          char **argv, char *error, size_t size)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p || argc < 1) {
        return 1;
    }
    for (size_t i = 0; i < p->command_count; ++i) {
        const CastExtensionCommand *c = &p->commands[i];
        if (strcmp(argv[0], c->verb) || (c->matches && !c->matches(argc, argv))) {
            continue;
        }
        bool safe = c->safety_operation || (c->operation_safe && c->operation_safe(argc, argv));
        if (!safe && cast_edition_require(c->capability_id, snapshot, error, size)) {
            return 5;
        }
        return c->execute(config, argc, argv, error, size);
    }
    return 1;
}
bool cast_edition_has_request(int argc, char **argv)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p || argc < 1) {
        return false;
    }
    for (size_t i = 0; i < p->command_count; ++i) {
        const CastExtensionCommand *c = &p->commands[i];
        if (!strcmp(argv[0], c->verb) && (!c->matches || c->matches(argc, argv))) {
            return true;
        }
    }
    return false;
}
void cast_edition_transition(const CastEditionSnapshot *old, CastEditionSnapshot *next, int64_t now)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->lifecycle_count; ++i) {
        const CastExtensionLifecycle *l = &p->lifecycle[i];
        for (unsigned j = 0; j < CAST_FEATURE_COUNT; ++j) {
            if (strcmp(next->features[j].id, l->capability_id)) {
                continue;
            }
            if (old->features[j].entitled != next->features[j].entitled && l->entitlement_changed) {
                int64_t deadline = now <= INT64_MAX - 300 ? now + 300 : INT64_MAX;
                l->entitlement_changed(next->features[j].entitled, deadline);
            }
            const CastExtensionCapability *cap = cast_extension_capability(l->capability_id);
            if (!next->features[j].entitled && cap && cap->active && cap->active()) {
                next->pending_downgrade = true;
            }
        }
    }
}
void cast_edition_shutdown(void)
{
    const CastProExtension *p = cast_edition_extensions();
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->lifecycle_count; ++i) {
        if (p->lifecycle[i].shutdown) {
            p->lifecycle[i].shutdown();
        }
    }
}
bool cast_extension_section_known(const char *section)
{
    size_t count;
    const CastExtensionSetting *shared = cast_shared_extension_schema(&count);
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(section, shared[i].section)) {
            return true;
        }
    }
    const CastProExtension *p = cast_edition_extensions();
    if (!p) {
        return false;
    }
    for (size_t i = 0; i < p->settings_count; ++i) {
        if (p->settings[i].section && !strcmp(section, p->settings[i].section)) {
            return true;
        }
    }
    return false;
}

const CastExtensionSetting *cast_extension_setting_find(const char *section, const char *key)
{
    size_t count;
    const CastExtensionSetting *schema = cast_shared_extension_schema(&count);
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(section, schema[i].section) && !strcmp(key, schema[i].key)) {
            return &schema[i];
        }
    }
    const CastProExtension *provider = cast_edition_extensions();
    if (provider) {
        for (size_t i = 0; i < provider->settings_count; ++i) {
            const CastExtensionSetting *setting = &provider->settings[i];
            if (!strcmp(section, setting->section) && !strcmp(key, setting->key)) {
                return setting;
            }
        }
    }
    return NULL;
}

int cast_extension_metadata_validate(const CastExtensionSetting *setting, const char *value,
                                     char *error, size_t size)
{
    bool valid = setting && value && setting->max_bytes && strlen(value) < setting->max_bytes;
    if (valid && setting->type == CAST_SETTING_BOOL) {
        valid = !strcmp(value, "true") || !strcmp(value, "false") || !strcmp(value, "on") ||
                !strcmp(value, "off") || !strcmp(value, "1") || !strcmp(value, "0");
    } else if (valid &&
               (setting->type == CAST_SETTING_INT || setting->type == CAST_SETTING_NUMBER)) {
        char *end;
        errno = 0;
        double number = strtod(value, &end);
        valid = end != value && !*end && !errno && isfinite(number) && number >= setting->minimum &&
                number <= setting->maximum &&
                (setting->type != CAST_SETTING_INT || trunc(number) == number);
    } else if (valid && setting->type == CAST_SETTING_COLOR) {
        valid = strlen(value) == 7 && value[0] == '#';
        for (size_t i = 1; valid && i < 7; ++i) {
            valid = (value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
                    (value[i] >= 'A' && value[i] <= 'F');
        }
    } else if (valid && setting->type == CAST_SETTING_ENUM) {
        valid = false;
        const char *option = setting->choices;
        while (option && *option) {
            const char *comma = strchr(option, ',');
            size_t length = comma ? (size_t)(comma - option) : strlen(option);
            if (strlen(value) == length && !memcmp(value, option, length)) {
                valid = true;
                break;
            }
            option = comma ? comma + 1 : NULL;
        }
    }
    if (!valid) {
        snprintf(error, size, "invalid %s.%s value", setting ? setting->section : "extension",
                 setting ? setting->key : "setting");
        return -1;
    }
    return setting->validate ? setting->validate(value, error, size) : 0;
}

static CastRuntimeHost runtime_host;
const CastRuntimeHooks *cast_runtime_hooks(void)
{
    const CastProExtension *provider = cast_edition_extensions();
    return provider ? provider->runtime : NULL;
}
void cast_runtime_set_host(const CastRuntimeHost *host)
{
    /* Installed before private workers start, cleared after they stop. */
    runtime_host = host ? *host : (CastRuntimeHost){0};
}
const CastRuntimeHost *cast_runtime_host(void)
{
    return &runtime_host;
}
