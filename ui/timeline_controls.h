#ifndef CAST_UI_TIMELINE_CONTROLS_H
#define CAST_UI_TIMELINE_CONTROLS_H
#include "cui/cui.h"
#include "cui/cui_components.h"
#include "../src/project_session.h"
typedef bool (*AppTimelineSubmit)(void *, unsigned, const char *const *);
typedef struct {
    bool connected, blocked, recording, cut, countdown, finalizing;
    float width, height;
} AppTimelineOptions;
void app_timeline_dock(cui_context *, const cui_theme *, const CastProjectSnapshot *,
                       AppTimelineOptions, AppTimelineSubmit, void *);
void app_project_controls(cui_context *, const cui_theme *, const CastProjectSnapshot *,
                          bool blocked, AppTimelineSubmit, void *);
#endif
