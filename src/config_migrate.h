#ifndef CAST_CONFIG_MIGRATE_H
#define CAST_CONFIG_MIGRATE_H
#include <stddef.h>
/* Explicit dry-run by default. Existing configuration is never changed by startup. */
int cast_config_migrate(int, char **, const char *, char *, size_t);
#endif
