/* MIT: isolated legacy compositor/backend fixtures intentionally have no provider.
 * Actual Pro hook integration is tested in the private motion compositor suite. */
#include "pro_runtime.h"
const CastRuntimeHooks *cast_runtime_hooks(void)
{
    return NULL;
}
const CastRuntimeHost *cast_runtime_host(void)
{
    static const CastRuntimeHost host = {0};
    return &host;
}
const char *config_extension_value(const Config *config, const char *section, const char *key)
{
    (void)config;
    (void)section;
    (void)key;
    return NULL;
}
