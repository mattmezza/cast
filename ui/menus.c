/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "brand.h"
#include <stdio.h>
#include <string.h>

typedef struct { cui_icon_state state; bool initialized; } MenuIconState;
typedef struct { const cui_theme *theme; const char *path; cui_id id; } MenuIcon;
static void menu_icon_collect(void *p) { cui_icon_state_collect(&((MenuIconState *)p)->state); }
static void menu_icon_destroy(void *p) { cui_icon_state_destroy(&((MenuIconState *)p)->state); }
static void menu_icon(cui_context *ui, void *user) {
    MenuIcon *art = user;
    MenuIconState *state = cui_resource_get(ui, art->id, 0x636173746d656e75ULL,
        sizeof *state, menu_icon_collect, menu_icon_destroy);
    if (!state) return;
    if (!state->initialized) {
        cui_icon_state_init(&state->state);
        if (!cui_icon_set_path(&state->state, cui_string(art->path), cast_brand_icon_loader, NULL)) return;
        state->initialized = true;
    }
    cui_icon(ui, art->id, art->theme, &state->state, (cui_icon_options){
        .size = CUI_ICON_CUSTOM, .custom_size = 13, .custom_color = true, .color = art->theme->text});
}
typedef struct { const char *label, *shortcut, *action; bool available; } MenuDef;
static const MenuDef file_menu[] = {
    {"New project", "Ctrl+N", "new", true}, {"Open project…", "Ctrl+O", "open", true}, {0},
    {"Save project", "Ctrl+S", "save", true}, {"Save a copy…", "", "copy", true}, {0},
    {"Export take…", "", "export", true}, {0}, {"Quit cast", "Ctrl+Q", "quit", true}};
static const MenuDef edit_menu[] = {
    {"Undo edit", "Ctrl+Z", "undo", true}, {"Redo", "Ctrl+Shift+Z", "redo", true}, {0},
    {"Rename project…", "", "rename", false}, {"Delete project", "", "delete", false}};
static const MenuDef view_menu[] = {
    {"Left panel", "[", "left", true}, {"Right panel", "]", "right", true},
    {"Timeline", "\\", "timeline", true}, {0}, {"Full-screen stage", "F11", "fullscreen", false},
    {"Open preview window", "", "preview", false}};
static const MenuDef composition_menu[] = {
    {"Overlay", "", "layout-0", true}, {"Split", "", "layout-1", true},
    {"Screen", "", "layout-2", true}, {"Camera", "", "layout-3", true},
    {"Stage", "", "layout-4", true}, {0}, {"Save as preset…", "", "save-preset", false},
    {"Presets…", "", "presets", true}};
static const MenuDef output_menu[] = {
    {"Virtual camera on/off", "", "camera", false}, {"Recording on/off", "R", "record", true},
    {"Cut at playhead", "C", "cut", true}, {"Streaming on/off", "", "stream", false}, {0},
    {"Pause all", "", "pause", false}};
static const MenuDef help_menu[] = {
    {"Keyboard shortcuts", "?", "keys", true}, {"Documentation", "", "docs", true}, {0},
    {"About cast", "", "about", true}};
typedef struct { const cui_theme *theme; const MenuDef *def; bool disabled; } MenuRow;
static void menu_row(cui_context *ui, void *user) {
    MenuRow *row = user;
    cui_color fg = row->theme->text, muted = row->theme->muted;
    if (row->disabled) { fg.a = 102; muted.a = 102; }
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true,
        .gap = 12, .align_y = CUI_ALIGN_CENTER});
    cui_text(ui, cui_string(row->def->label), 12, fg);
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 1}); cui_end_box(ui);
    const char *shortcut=row->def->shortcut;
#ifdef __APPLE__
    char platform_shortcut[64];
    if(!strncmp(shortcut,"Ctrl+",5)) {snprintf(platform_shortcut,sizeof platform_shortcut,"Cmd+%s",shortcut+5);shortcut=platform_shortcut;}
#endif
    cui_text_ex(ui, cui_string(shortcut), 10, muted, 0, (cui_text_style){.family=1,.monospace=true});
    cui_end_box(ui);
}
static void dispatch(cui_context *ui, AppState *app, const MenuDef *def, cui_id trigger) {
    if (!strcmp(def->action, "left")) app->show_sources = !app->show_sources;
    else if (!strcmp(def->action, "right")) app->show_inspector = !app->show_inspector;
    else if (!strcmp(def->action,"timeline")) app->show_timeline[app->workspace]=!app->show_timeline[app->workspace];
    else if (!strcmp(def->action,"undo") || !strcmp(def->action,"redo")) {
        const char *args[]={"timeline",def->action};app_request_command(app,2,args);
    }
    else if (!strncmp(def->action, "layout-", 7)) {
        const char *layouts[] = {"overlay", "split", "screen", "camera", "stage"};
        unsigned index = (unsigned)(def->action[7] - '0');
        if (index < APP_LAYOUT_COUNT) app_request_edit(app, APP_EDIT_LAYOUT, layouts[index]);
    }
    else if (!strcmp(def->action,"new")) {
        const char *args[]={"project","new"};app_timeline_submit(app,2,args);
    } else if (!strcmp(def->action,"open") || !strcmp(def->action,"copy") || !strcmp(def->action,"export")) {
        const char *args[]={"file",!strcmp(def->action,"copy")?"save":def->action};app_timeline_submit(app,2,args);
    } else if (!strcmp(def->action,"save")) {
        const char *args[]={app->live.project.path[0]?"project":"file","save"};app_timeline_submit(app,2,args);
    }
    else if (!strcmp(def->action, "record"))
        app_request_edit(app, APP_EDIT_RECORD_ACTION,
            app->live.recording || app->live.record_countdown_active ? "stop" : "start");
    else if (!strcmp(def->action, "cut"))
        app_request_edit(app, APP_EDIT_RECORD_ACTION, "cut");
    else if (!strcmp(def->action, "quit")) app->quit_requested = true;
    else {
        app->settings_category = !strcmp(def->action, "presets") ? 5 : 9;
        app_open_settings(ui, app, trigger);
    }
    cui_invalidate(ui);
}
void app_menu_bar(cui_context *ui, AppState *app, const cui_theme *t, float width) {
    static const char *names[] = {"File", "Edit", "View", "Composition", "Output", "Help"};
    static const char *ids[] = {"menu-file", "menu-edit", "menu-view", "menu-composition", "menu-output", "menu-help"};
    const MenuDef *defs[] = {file_menu, edit_menu, view_menu, composition_menu, output_menu, help_menu};
    const size_t counts[] = {9, 5, 6, 8, 6, 4};
    if (!app->menus_initialized) {
        for (unsigned i = 0; i < 6; ++i) {
            char popup[40]; snprintf(popup, sizeof popup, "menu-popup-%u", i);
            cui_menu_tree_init(&app->menus[i], cui_id_from(ui, popup));
        }
        app->menus_initialized = true;
    }
    cui_begin_box(ui, cui_id_from(ui, "menubar"), (cui_box){.width = -1, .height = 28,
        .row = true, .gap = 1, .custom_padding = true, .padding_edges = {6, 10, 2, 2},
        .align_y = CUI_ALIGN_CENTER, .background = t->background, .clip = true});
    for (unsigned m = 0; m < 6; ++m) {
        cui_id trigger = cui_id_from(ui, ids[m]), action = 0;
        cui_menu_entry entries[9] = {0}; MenuRow rows[9] = {0};
        for (size_t i = 0; i < counts[m]; ++i) {
            const MenuDef *def = &defs[m][i];
            char id[64]; snprintf(id, sizeof id, "%s-item-%zu", ids[m], i);
            bool disabled = !def->available || (m == 3 && i < 5 && (app->edit_requested || app->live.pending || (app->live.enabled && !app->live.connected))) ||
                (m == 2 && ((i == 0 && width < 1000) || (i == 1 && width < 800)));
            if(m==0 && i<7 && def->label) {
                disabled=!app->live.connected || app->live.pending || app->live.project.busy || app->extension_requested || app->file_busy;
                if(i==0 || i==1)disabled=disabled || app->live.recording || app->live.stream.active;
                if(i==3 || i==4)disabled=disabled || !app->live.project.open;
                if(i==6)disabled=disabled || !app->live.project.clip_count || app->live.project.recording || app->live.project.exporting || app->live.project.missing_count;
            }
            if(m==1 && i<2)disabled=!app->live.connected || app->live.pending || app->live.project.busy || app->live.project.recording;
            if (m == 4 && (i == 1 || i == 2)) {
                disabled = !app->live.enabled || !app->live.connected || app->live.pending ||
                    app->live.snapshot_busy || app->edit_requested || app->live.record_finalizing ||
                    (i == 2 && (!app->live.recording || app->live.record_cut || app->live.record_countdown_active));
            }
            rows[i] = (MenuRow){t, def, disabled};
            entries[i] = (cui_menu_entry){.id = cui_id_from(ui, id),
                .label = cui_string(def->label ? def->label : ""), .disabled = disabled,
                .kind = def->label ? CUI_MENU_ACTION : CUI_MENU_SEPARATOR,
                .render = def->label ? menu_row : NULL, .render_user = &rows[i]};
        }
        bool was_open = app->menus[m].overlay.open;
        bool activated = cui_base_menu_tree(ui, trigger, cui_string(names[m]), entries, counts[m],
            &action, &app->menus[m], app->settings.overlay.open || app->panel_sheet.overlay.open, .016f, false);
        if (!was_open && app->menus[m].overlay.open)
            for (unsigned j = 0; j < 6; ++j) if (j != m) cui_overlay_close(ui, &app->menus[j].overlay);
        bool hovered = cui_hovered(ui, trigger);
        cui_begin_box(ui, trigger, (cui_box){.height = 24, .custom_padding = true,
            .padding_edges = {width < 600 ? 5 : 8, width < 600 ? 5 : 8, 0, 0}, .align_y = CUI_ALIGN_CENTER,
            .background = hovered || app->menus[m].overlay.open ? cast_design_tokens(t->dark).hover : t->background});
        cui_text(ui, cui_string(names[m]), 12.5f, hovered || app->menus[m].overlay.open ? t->text : cast_design_tokens(t->dark).secondary);
        cui_end_box(ui);
        float height = cui_bounds(ui, cui_id_from(ui, "root")).h;
        cui_rect anchor = cui_bounds(ui, trigger); anchor.y += 2;
        cui_theme popup_theme = *t; popup_theme.popover = t->surface; popup_theme.font_size = 12;
        (void)cui_menu_popup_paint(ui, &popup_theme, cui_string(names[m]), entries, counts[m],
            &app->menus[m], anchor, (cui_menu_tree_options){.density = 28.f / 26,
                .popup = {.viewport = {0,0,width,height > 0 ? height : 820}, .width = 224},
                .size = CUI_MENU_MEDIUM});
        if (activated) for (size_t i = 0; i < counts[m]; ++i)
            if (entries[i].id == action && defs[m][i].label) dispatch(ui, app, &defs[m][i], trigger);
    }
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 1}); cui_end_box(ui);
    MenuIcon prompter = {t,"cast-teleprompter",cui_id_from(ui,"menu-prompter-art")};
    MenuIcon settings = {t,"cast-gear",cui_id_from(ui,"menu-settings-art")};
    if (width >= 600)
        if (cui_button_ex(ui, cui_id_from(ui, "menu-prompter"), t, cui_string("Prompter"),
            (cui_button_options){.variant = CUI_BUTTON_GHOST, .height = 24, .font_size = 11.5f, .leading_content = menu_icon, .leading_user = &prompter, .toggle_semantics = true, .toggled = app->live.notes_window_open, .open = app->live.notes_window_open, .disabled = !app->live.notes_available || !app->live.connected || app->live.pending}))
            app_request_edit(app, APP_EDIT_NOTES_ACTION, app->live.notes_window_open ? "close" : "open");
    if (cui_button_ex(ui, cui_id_from(ui, "settings-open"), t, cui_string("Settings"),
        (cui_button_options){.variant = CUI_BUTTON_GHOST, .height = 24, .font_size = 11.5f,
            .leading_content = menu_icon, .leading_user = &settings}))
        app_open_settings(ui, app, cui_id_from(ui, "settings-open"));
    cui_end_box(ui);
}
