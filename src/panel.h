#ifndef CAST_PANEL_H
#define CAST_PANEL_H
#include "cast.h"

/* The control window is a separate client process; closing it leaves cast running. */
int panel_run(const Config *, char *, size_t);
#endif
