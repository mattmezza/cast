#ifndef CAST_UPDATE_H
#define CAST_UPDATE_H
#include <stddef.h>
/* argv begins with "update". Returns installer status, or -1 on a launcher error. */
int cast_update(int argc, char **argv, char *error, size_t error_size);
/* The caller supplies daemon/config paths; no runtime trust or channel override. */
int cast_update_with_context(int argc, char **argv, const char *license_path,
                             const char *socket_path, char *error, size_t error_size);
#endif
