#ifndef CAST_UPDATE_H
#define CAST_UPDATE_H
#include <stddef.h>
/* argv begins with "update". Returns installer status, or -1 on a launcher error. */
int cast_update(int argc, char **argv, char *error, size_t error_size);
#endif
