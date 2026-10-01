#ifndef CAST_PLATFORM_BACKEND_H
#define CAST_PLATFORM_BACKEND_H
#include "cast.h"
#ifdef WITH_X11
Platform *x11_open(const Config *, char *, size_t);
void x11_close(Platform *);
Capabilities x11_capabilities(Platform *);
uint64_t x11_source_generation(Platform *);
int x11_capture(Platform *, Frame *, Cursor *, char *, size_t);
int x11_command(Platform *, Config *, int, char **, char *, size_t);
int x11_reconfigure(Platform *, const Config *, char *, size_t);
void x11_events(Platform *, Compositor *, const Config *, bool);
int x11_preview(Platform *, const Frame *, const State *, const Config *, char *, size_t);
void x11_doctor(const Config *, char *, size_t);
#endif
#endif
