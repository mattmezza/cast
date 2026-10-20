#ifndef CAST_WORKSPACE_H
#define CAST_WORKSPACE_H

#include "../src/cast.h"

/* argv[0] is "workspace"; remaining arguments are UI preview options. */
int cast_workspace_run(const Config *config, int argc, char **argv);

extern const unsigned char cast_ui_regular_start[], cast_ui_regular_end[];
extern const unsigned char cast_ui_bold_start[], cast_ui_bold_end[];

extern const unsigned char cast_ui_medium_start[], cast_ui_medium_end[];
extern const unsigned char cast_ui_semibold_start[], cast_ui_semibold_end[];

extern const unsigned char cast_ui_mono_regular_start[], cast_ui_mono_regular_end[];

extern const unsigned char cast_ui_mono_medium_start[], cast_ui_mono_medium_end[];

extern const unsigned char cast_ui_mono_semibold_start[], cast_ui_mono_semibold_end[];

#endif
