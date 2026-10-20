/* SPDX-License-Identifier: MIT */
#ifndef CAST_SOURCE_PICKER_H
#define CAST_SOURCE_PICKER_H
#include "cui/cui_components.h"
#include "live.h"
struct AppState;
typedef struct {
    cui_choice_popup popup;
    cui_rect viewport;
    AppDeviceKind kind, switch_kind;
    bool initialized;
    uint64_t generation, items_revision;
} AppSourcePicker;
void app_source_picker(cui_context *, struct AppState *, const cui_theme *,
                       AppDeviceKind, const char *id, const char *label);
void app_source_picker_close(cui_context *, struct AppState *);
#endif
