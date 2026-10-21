#ifndef CAST_CONFIG_PRESETS_H
#define CAST_CONFIG_PRESETS_H
#include "cast.h"
#define CAST_PRESET_NAME_MAX 42
/* Persist only public framing presets and their cycle order. Other INI bytes
 * remain untouched. Failure leaves both output and the destination unchanged. */
int config_presets_edit(const char *path, const Config *current, const char *action,
                        const char *name, const char *new_name, Config *out, char *error,
                        size_t error_size);
#ifdef CAST_TEST
/* Fail before publication, after writing this many bytes; -1 disables. */
void config_presets_test_write_limit(int64_t);
#endif
#endif
