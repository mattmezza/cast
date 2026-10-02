#ifndef CAST_HELP_COMMANDS_H
#define CAST_HELP_COMMANDS_H
#include <stddef.h>

/* Local, output-only guides. -1 means another command owns the arguments. */
int help_command(int argc, char **argv, char *error, size_t size);
#endif
