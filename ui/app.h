/* SPDX-License-Identifier: MIT */
#ifndef CAST_UI_PREVIEW_H
#define CAST_UI_PREVIEW_H
#include "cui/cui_components.h"
#include "live.h"
#include "source_picker.h"
typedef enum { APP_CAMERA, APP_RECORD, APP_STREAM, APP_WORKSPACE_COUNT } AppWorkspace;
typedef enum { APP_OVERLAY, APP_SPLIT, APP_SCREEN, APP_WEBCAM, APP_STAGE, APP_LAYOUT_COUNT } AppLayout;
typedef enum { APP_SYSTEM, APP_LIGHT, APP_DARK } AppAppearance;
/* Local fixture choices or acknowledged daemon state; edits are single queued intents. */
typedef struct AppState {
    AppLive live;
    AppSourcePicker source_picker;
    bool device_request_requested;
    AppDeviceKind device_request_kind;
    bool source_choice_requested;
    AppDeviceKind source_choice_kind;
    char source_choice_value[APP_DEVICE_ID_MAX];
    bool edit_requested;
    AppEdit edit;
    char edit_value[32];
    AppWorkspace workspace;
    AppLayout layout;
    AppAppearance appearance;
    bool system_dark;
    bool show_sources, show_inspector;
    bool settings_initialized;
    unsigned settings_category;
    cui_surface_state settings;
    cui_scroll_state settings_scroll, settings_rail_scroll;
    cui_menu_tree_state menus[6];
    bool menus_initialized, quit_requested;
    cui_surface_state panel_sheet;
    bool panel_sheet_initialized;
    unsigned drawer_kind; /* 1 sources, 2 output */
    cui_id workspace_tab;
    cui_scroll_state source_scroll, inspector_scroll;
    bool source_sections[4];
    bool fit_cover;
    unsigned camera_anchor;
    float camera_size;
} AppState;
void app_init(AppState *);
/* One validated intent; host submits it once and clears edit_requested. */
bool app_request_edit(AppState *, AppEdit, const char *);
bool app_is_dark(const AppState *);
const char *app_workspace_name(AppWorkspace);
const char *app_layout_name(AppLayout);
void app_open_settings(cui_context *, AppState *, cui_id trigger);
void app_settings_frame(cui_context *, AppState *, const cui_theme *, float width, float height);
void app_menu_bar(cui_context *, AppState *, const cui_theme *, float width);
void app_frame(cui_context *, AppState *, float width, float height);
#endif
