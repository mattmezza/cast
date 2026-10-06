#ifndef CAST_COMPOSITOR_RUNTIME_H
#define CAST_COMPOSITOR_RUNTIME_H
#include "pro_runtime.h"
/* Main-thread context only. Source coordinates are local to the selected capture. */
bool compositor_motion_ready(const Compositor *);
int compositor_motion_adopt(Compositor *, void *prepared, char *, size_t);
int compositor_motion_prepare(Compositor *, char *, size_t);
void compositor_runtime_context(Compositor *, const Capabilities *, uint64_t source_generation);
int compositor_motion_event_validate(Compositor *, const CastMotionEvent *, char *, size_t);
int compositor_motion_event(Compositor *, const CastMotionEvent *, char *, size_t);
bool compositor_motion_snapshot(const Compositor *, CastMotionSnapshot *);
void compositor_motion_status(Compositor *, char *, size_t, bool json);
#endif
