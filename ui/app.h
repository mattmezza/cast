/* SPDX-License-Identifier: MIT */
#ifndef CAST_UI_PREVIEW_H
#define CAST_UI_PREVIEW_H
#include "cui/cui_components.h"
#include "live.h"
#include "source_picker.h"
#include "file_dialog.h"
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
    bool extension_requested, extension_command;
    unsigned extension_argc;
    char extension_args[8][4096];
    uint64_t extension_generation;
    bool file_requested, file_busy;
    FileDialogRequest file_request;
    FileDialogResult file_result;
    bool file_result_ready;
    bool edit_requested;
    AppEdit edit;
    char edit_value[4096];
    AppWorkspace workspace;
    AppLayout layout;
    AppAppearance appearance;
    bool system_dark;
    bool show_sources, show_inspector;
    bool show_timeline[APP_WORKSPACE_COUNT];
    bool settings_initialized;
    unsigned settings_category;
    cui_surface_state settings;
    cui_surface_state discard_dialog;
    bool discard_initialized, discard_requested;
    unsigned discard_argc;
    char discard_args[3][4096];
    uint64_t discard_generation;
    cui_choice_popup recording_selects[3];
    bool recording_select_initialized[3];
    uint64_t recording_select_generation;
    cui_overlay zoom_popup;
    bool zoom_popup_initialized;
    cui_scroll_state settings_scroll, settings_rail_scroll;
    cui_menu_tree_state menus[6];
    bool menus_initialized, quit_requested;
    cui_surface_state panel_sheet;
    bool panel_sheet_initialized;
    unsigned drawer_kind; /* 1 sources, 2 output */
    cui_id workspace_tab, text_input_owner;
    cui_scroll_state source_scroll, inspector_scroll;
    bool source_sections[4];
    bool fit_cover;
    unsigned camera_anchor;
    float camera_size;
} AppState;
void app_init(AppState *);
/* One validated intent; host submits it once and clears edit_requested. */
bool app_request_edit(AppState *, AppEdit, const char *);
/* Bounded session intents. Values remain acknowledged snapshot data until daemon acceptance. */
bool app_stream_submit(void *, AppStreamIntent, const char *);
bool app_timeline_submit(void *, unsigned, const char *const *);
bool app_request_setting(AppState *, const char *key, const char *value);
bool app_request_command(AppState *, unsigned argc, const char *const *argv);
bool app_request_file(AppState *, const FileDialogRequest *);
bool app_take_file_result(AppState *, const char *target, FileDialogResult *);
const char *app_setting(const AppState *, const char *section, const char *key);
bool app_is_dark(const AppState *);
const char *app_workspace_name(AppWorkspace);
const char *app_layout_name(AppLayout);
void app_open_settings(cui_context *, AppState *, cui_id trigger);
void app_settings_frame(cui_context *, AppState *, const cui_theme *, float width, float height);
void app_menu_bar(cui_context *, AppState *, const cui_theme *, float width);
void app_frame(cui_context *, AppState *, float width, float height);
#endif
