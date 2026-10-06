#ifndef CAST_EDITION_EXTENSIONS_H
#define CAST_EDITION_EXTENSIONS_H
#include "pro_extension.h"
/* Compiled, immutable contributions only; no environment/config registration. */
const CastProExtension *cast_edition_extensions(void);
const CastExtensionCapability *cast_extension_capability(const char *);
const CastExtensionSetting *cast_shared_extension_schema(size_t *);
bool cast_extension_section_known(const char *);
const CastExtensionSetting *cast_extension_setting_find(const char *, const char *);
int cast_extension_metadata_validate(const CastExtensionSetting *, const char *, char *, size_t);
#endif
