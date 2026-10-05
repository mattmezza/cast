#ifndef CAST_PANEL_H
#define CAST_PANEL_H
#include "cast.h"

/* The control window is a separate client process; closing it leaves cast running. */
int panel_run(const Config *, char *, size_t);
/* Application launch owns a restartable daemon; attach-only panel remains available. */
int panel_run_application(const Config *, int, const char *const *, bool, char *, size_t);
#endif
