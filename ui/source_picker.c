/* SPDX-License-Identifier: MIT */
#include "source_picker.h"
#include "app.h"
#include "brand.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { cui_icon_state state; bool initialized; } PickerIcon;
static void icon_collect(void *p) { cui_icon_state_collect(&((PickerIcon *)p)->state); }
static void icon_destroy(void *p) { cui_icon_state_destroy(&((PickerIcon *)p)->state); }
static void chevron(cui_context *ui, cui_id id, const cui_theme *t, bool open, bool hovered, bool disabled) {
    PickerIcon *icon = cui_resource_get(ui, id, UINT64_C(0x7069636b65726172), sizeof *icon,
                                       icon_collect, icon_destroy);
    if (!icon) return;
    if (!icon->initialized) {
        cui_icon_state_init(&icon->state);
        if (!cui_icon_set_path(&icon->state, cui_string("cast-down"), cast_brand_icon_loader, NULL)) return;
        icon->initialized = true;
    }
    cui_color color = open || hovered ? t->text : t->muted;
    if (disabled) color.a = 140;
    cui_icon(ui, id, t, &icon->state, (cui_icon_options){.size = CUI_ICON_CUSTOM,
        .custom_size = 12, .custom_color = true, .color = color,
        .custom_transform = open, .transform = {.a = -1, .d = -1}});
}
void app_source_picker_close(cui_context *ui, AppState *app) {
    if (app->source_picker.initialized)
        cui_overlay_close(ui, &app->source_picker.popup.overlay);
}
static const char *kind_name(AppDeviceKind kind) {
    switch (kind) {
    case APP_DEVICE_CAMERA: return "camera";
    case APP_DEVICE_MIC: return "mic";
    case APP_DEVICE_DESKTOP: return "desktop";
    default: return "screen";
    }
}
static const char *trigger_name(AppDeviceKind kind) {
    switch (kind) {
    case APP_DEVICE_CAMERA: return "camera-choose";
    case APP_DEVICE_MIC: return "audio-mic-source";
    case APP_DEVICE_DESKTOP: return "audio-desktop-source";
    default: return "source-choose";
    }
}
static bool available(const AppLive *live, AppDeviceKind kind) {
    return live->enabled && live->connected &&
        (kind == APP_DEVICE_MIC || kind == APP_DEVICE_DESKTOP ? live->can_select_audio :
         kind == APP_DEVICE_CAMERA ? live->can_select_camera :
         live->can_select_monitor || live->can_reselect_portal);
}
static void request(AppState *app, AppDeviceKind kind) {
    app->device_request_kind = kind;
    app->device_request_requested = true;
}
static void choose(AppState *app, AppDeviceKind kind, const char *value) {
    app->source_choice_requested = true;
    app->source_choice_kind = kind;
    snprintf(app->source_choice_value, sizeof app->source_choice_value, "%s", value);
}
static cui_id option_id(cui_context *ui, AppDeviceKind kind, unsigned index) {
    char text[64];
    snprintf(text, sizeof text, "source-picker-%s-item-%u",
             kind_name(kind), index);
    return cui_id_from(ui, text);
}
void app_source_picker(cui_context *ui, AppState *app, const cui_theme *t,
                       AppDeviceKind kind, const char *name, const char *label) {
    AppSourcePicker *picker = &app->source_picker;
    if (!picker->initialized) {
        cui_choice_popup_init(&picker->popup, cui_id_from(ui, "source-picker-popup"));
        picker->initialized = true;
    }
    if (picker->popup.overlay.open && (!app->live.connected ||
        picker->generation != app->live.generation || app->settings.overlay.open))
        app_source_picker_close(ui, app);
    cui_id trigger = cui_id_from(ui, name);
    if (picker->popup.overlay.open && picker->kind == kind) {
        cui_rect other = {0};
        size_t n = 0;
        const cui_event *queued = cui_frame_events(ui, &n);
        for (size_t i = 0; i < n; ++i) {
            if (cui_event_consumed(ui, i) || queued[i].type != CUI_EVENT_POINTER_DOWN ||
                queued[i].pointer_button != CUI_POINTER_PRIMARY ||
                cui_event_target(ui, i) != picker->popup.overlay.id) continue;
            const AppDeviceKind kinds[] = {APP_DEVICE_SCREEN, APP_DEVICE_CAMERA, APP_DEVICE_MIC, APP_DEVICE_DESKTOP};
            for (unsigned k = 0; k < sizeof kinds / sizeof *kinds; ++k) {
                AppDeviceKind next = kinds[k];
                if (next == kind) continue;
                other = cui_bounds(ui, cui_id_from(ui, trigger_name(next)));
                if (other.w > 0 && queued[i].x >= other.x && queued[i].x < other.x + other.w &&
                    queued[i].y >= other.y && queued[i].y < other.y + other.h &&
                    available(&app->live, next) && !app->live.pending && !app->live.snapshot_busy) {
                    cui_overlay_close(ui, &picker->popup.overlay);
                    picker->switch_kind = next;
                    break;
                }
            }
        }
    }
    bool own = !picker->popup.overlay.open || picker->kind == kind;
    bool disabled = !available(&app->live, kind) || app->edit_requested ||
        app->live.pending || app->live.snapshot_busy || app->source_choice_requested || app->device_request_requested;
    bool matching = app->live.devices.kind == kind &&
                    app->live.devices.generation == app->live.generation;
    bool loading = !matching || app->live.devices.loading || app->device_request_requested;
    cui_choice_item items[APP_DEVICE_MAX + 3];
    const char *values[APP_DEVICE_MAX + 3] = {0};
    unsigned count = 0;
    cui_id selected = 0;
    if (kind == APP_DEVICE_CAMERA) {
        items[count] = (cui_choice_item){cui_id_from(ui, "source-picker-camera-none"),
                                        cui_string("No camera"), app->live.pending || app->live.snapshot_busy};
        values[count] = "";
        if (!app->live.camera_enabled) selected = items[count].id;
        ++count;
    }
    if (kind == APP_DEVICE_SCREEN && app->live.can_reselect_portal) {
        items[count] = (cui_choice_item){cui_id_from(ui, "source-picker-portal"),
                                        cui_string("Choose screen…"), app->live.pending || app->live.snapshot_busy};
        values[count++] = "";
        loading = false;
    } else if (loading) {
        items[count++] = (cui_choice_item){cui_id_from(ui, "source-picker-status"),
                                           cui_string("Loading…"), true};
    } else if (app->live.devices.error[0]) {
        items[count++] = (cui_choice_item){cui_id_from(ui, "source-picker-status"),
                                           cui_string(app->live.devices.error), true};
        items[count++] = (cui_choice_item){cui_id_from(ui, "source-picker-retry"),
                                           cui_string("Retry"), app->live.pending || app->live.snapshot_busy};
    } else {
        unsigned total = app->live.devices.count;
        if (total > APP_DEVICE_MAX) total = APP_DEVICE_MAX;
        for (unsigned i = 0; i < total; ++i) {
            const AppDeviceChoice *choice = &app->live.devices.choices[i];
            items[count] = (cui_choice_item){option_id(ui, kind, i), cui_string(choice->label),
                                            app->live.pending || app->live.snapshot_busy};
            values[count] = choice->id;
            const char *current = kind == APP_DEVICE_CAMERA ? app->live.camera_id :
                kind == APP_DEVICE_MIC ? app->live.mic_id :
                kind == APP_DEVICE_DESKTOP ? app->live.desktop_id : app->live.screen_id;
            if ((kind != APP_DEVICE_CAMERA || app->live.camera_enabled) && !strcmp(current, choice->id))
                selected = items[count].id;
            ++count;
        }
        if (!total)
            items[count++] = (cui_choice_item){cui_id_from(ui, "source-picker-status"),
                cui_string(kind == APP_DEVICE_CAMERA ? "No cameras found" :
                    kind == APP_DEVICE_MIC ? "No microphones found" :
                    kind == APP_DEVICE_DESKTOP ? "No desktop sources found" : "No displays found"), true};
    }
    bool was_open = picker->popup.overlay.open;
    bool switched = picker->switch_kind == kind;
    if (switched) {
        picker->switch_kind = APP_DEVICE_NONE;
        cui_overlay_open(ui, &picker->popup.overlay, trigger);
    }
    if (own) {
        /* Keep the existing popup mounted while its discovery command is pending. */
        cui_base_choice_popup_trigger(ui, trigger, cui_string(label), was_open ? false : disabled,
                                       items, count, selected, &picker->popup);
        if ((!was_open || switched) && picker->popup.overlay.open) {
            picker->kind = kind;
            picker->generation = app->live.generation;
            picker->items_revision = 0;
            if (!(kind == APP_DEVICE_SCREEN && app->live.can_reselect_portal)) request(app, kind);
        }
    } else {
        (void)cui_interact(ui, trigger, CUI_ROLE_BUTTON, cui_string(label), disabled, false, 0);
    }
    bool open = picker->popup.overlay.open && picker->kind == kind;
    CastDesignTokens tokens = cast_design_tokens(t->dark);
    cui_color bg = disabled && !open ? tokens.control : open ? tokens.control :
                   cui_hovered(ui, trigger) ? tokens.hover : t->background;
    cui_color border = open ? tokens.selected_line : cui_hovered(ui, trigger) ?
                       cui_color_mix_oklab(t->border, t->text, .5f) : t->border;
    cui_color fg = t->text;
    if (disabled && !open) fg.a = 140;
    cui_begin_box(ui, trigger, (cui_box){.width = -1, .height = 36, .row = true, .gap = 8,
        .custom_padding = true, .padding_edges = {10, 10, 0, 0},
        .align_y = CUI_ALIGN_CENTER, .background = bg, .border_width = 1, .border = border});
    cui_text_ellipsis(ui, trigger ^ UINT64_C(0x76616c7565), cui_string(label),
                     (cui_text_ellipsis_options){.width = -1, .font_size = 12.5f, .color = fg});
    chevron(ui, trigger ^ UINT64_C(0x6172726f77), t, open, cui_hovered(ui, trigger), disabled && !open);
    cui_end_box(ui);
    if (!open || !cui_overlay_begin(ui, &picker->popup.overlay)) return;
    size_t event_count = 0;
    const cui_event *events = cui_frame_events(ui, &event_count);
    for (size_t i = 0; i < event_count; ++i) {
        if (!cui_event_consumed(ui, i) && events[i].type == CUI_EVENT_KEY_DOWN &&
            events[i].key == CUI_KEY_ESCAPE && !events[i].repeat &&
            cui_event_within_scope(ui, i, picker->popup.overlay.id)) {
            cui_consume_event(ui, i);
            (void)cui_dismiss_focus_scope(ui, picker->popup.overlay.id, trigger);
            cui_overlay_close(ui, &picker->popup.overlay);
            cui_overlay_end(ui);
            return;
        }
    }
    cui_rect viewport = picker->viewport;
    cui_rect anchor = cui_bounds(ui, trigger);
    float popup_width = fminf(fmaxf(190, anchor.w), fmaxf(0, viewport.w - 16));
    unsigned visible = count < 8 ? count : 8;
    float space = fmaxf(anchor.y - viewport.y - 12,
                        viewport.y + viewport.h - anchor.y - anchor.h - 12);
    if (space < visible * 32 + 8) {
        unsigned fit = space > 40 ? (unsigned)((space - 8) / 32) : 1;
        if (visible > fit) visible = fit;
    }
    cui_rect inset = {viewport.x + 8, viewport.y + 8,
                      fmaxf(0, viewport.w - 16), fmaxf(0, viewport.h - 16)};
    cui_rect panel = cui_popup_place(anchor, (cui_size){popup_width, visible * 32.f + 8},
                                     inset, CUI_POPUP_BELOW, 4).bounds;
    cui_overlay_set_bounds(&picker->popup.overlay, panel);
    cui_begin_portal(ui, picker->popup.overlay.id, (cui_box){.padding = 4, .background = t->surface,
        .border_width = 1, .border = t->border}, panel, 220);
    cui_box_shadow shadow = {.layer = {.offset_y = 16, .sigma = 20,
                                      .color = {16, 17, 19, t->dark ? 184 : 51}}};
    cui_set_box_shadows(ui, &shadow, 1, (cui_corner_radii){0});
    cui_id list = cui_id_from(ui, "source-picker-list");
    uint64_t revision = count;
    for (unsigned i = 0; i < count; ++i) {
        for (const char *p = values[i] ? values[i] : ""; *p; ++p)
            revision = (revision ^ (unsigned char)*p) * UINT64_C(1099511628211);
        revision ^= items[i].id + (items[i].disabled ? 1 : 0);
    }
    if (picker->items_revision != revision) {
        for (size_t i = 0; i < event_count; ++i)
            if (cui_event_within_scope(ui, i, picker->popup.overlay.id))
                (void)cui_cancel_event_activation(ui, i);
        picker->popup.list = (cui_list_state){.active = selected};
        if (!selected)
            for (unsigned i = 0; i < count; ++i)
                if (!items[i].disabled) { picker->popup.list.active = items[i].id; break; }
        cui_focus(ui, picker->popup.list.active);
        picker->items_revision = revision;
    }
    cui_interact(ui, list, CUI_ROLE_LISTBOX, cui_string(kind == APP_DEVICE_CAMERA ? "Cameras" :
        kind == APP_DEVICE_MIC ? "Microphones" : kind == APP_DEVICE_DESKTOP ? "Desktop audio sources" : "Displays"),
                 false, false, 0);
    cui_set_semantic_details(ui, list, (cui_semantic_details){
        .orientation = CUI_ORIENTATION_VERTICAL, .scrollable = count > visible,
        .scroll_y = picker->popup.list.first_visible * 32.f,
        .scroll_max_y = count > visible ? (count - visible) * 32.f : 0});
    for (size_t e = 0; e < event_count; ++e) {
        if (cui_event_consumed(ui, e) || events[e].type != CUI_EVENT_KEY_DOWN ||
            !cui_event_within_scope(ui, e, picker->popup.overlay.id)) continue;
        if (events[e].key != CUI_KEY_UP && events[e].key != CUI_KEY_DOWN) continue;
        cui_id edge = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned n = events[e].key == CUI_KEY_UP ? i : count - 1 - i;
            if (!items[n].disabled) { edge = items[n].id; break; }
        }
        if (edge && picker->popup.list.active == edge) cui_consume_event(ui, e);
    }
    cui_id action = selected;
    bool activated = cui_base_list(ui, items, count, &action, &picker->popup.list,
        (cui_list_options){.dt = .016f, .visible_begin = picker->popup.list.first_visible,
            .visible_count = visible, .height = 32, .scroll_target = list,
            .scroll_scope = picker->popup.overlay.id, .scroll_bounds = cui_bounds(ui, list)});
    cui_begin_box(ui, list, (cui_box){.width = -1, .height = visible * 32.f, .clip = true});
    unsigned begin = (unsigned)picker->popup.list.first_visible;
    for (unsigned i = begin; i < count && i < begin + visible; ++i) {
        bool checked = selected == items[i].id;
        bool active = picker->popup.list.active == items[i].id;
        cui_color row_bg = checked ? tokens.selected :
            !items[i].disabled && (active || cui_hovered(ui, items[i].id)) ? tokens.hover : (cui_color){0};
        cui_begin_box(ui, items[i].id, (cui_box){.width = -1, .height = 32, .row = true, .gap = 8,
            .custom_padding = true, .padding_edges = {8, 8, 0, 0},
            .align_y = CUI_ALIGN_CENTER, .background = row_bg,
            .border_width = active && cui_focused(ui) == items[i].id ? 2 : 0, .border = t->focus});
        cui_begin_box(ui, 0, (cui_box){.width = 12, .height = 12,
            .fixed_width = true, .fixed_height = true, .align_y = CUI_ALIGN_CENTER});
        if (checked) cui_checkmark(ui, 11, t->primary);
        cui_end_box(ui);
        cui_text_ellipsis(ui, items[i].id ^ UINT64_C(0x6c6162656c), items[i].label,
            (cui_text_ellipsis_options){.width = -1, .font_size = 12.5f,
                                       .color = items[i].disabled ? t->muted : t->text});
        cui_end_box(ui);
        cui_set_semantic_details(ui, items[i].id, (cui_semantic_details){.selected = checked, .parent = list});
    }
    cui_end_box(ui);
    cui_end_box(ui);
    if (activated) {
        if (action == cui_id_from(ui, "source-picker-retry")) {
            request(app, kind);
            picker->items_revision = 0;
        } else {
            for (unsigned i = 0; i < count; ++i)
                if (items[i].id == action && values[i]) {
                    choose(app, kind, values[i]);
                    (void)cui_dismiss_focus_scope(ui, picker->popup.overlay.id, trigger);
                    cui_overlay_close(ui, &picker->popup.overlay);
                    break;
                }
        }
    }
    cui_overlay_end(ui);
}
