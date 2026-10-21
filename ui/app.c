/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "cast_theme.h"
#include "brand.h"
#include "cui/cui_styled_slider_events.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Context-owned masks keep borrowed draw pixels alive through renderer completion. */
typedef struct { cui_icon_state state; bool initialized; char path[48]; } AppIcon;
static void icon_collect(void *p) { cui_icon_state_collect(&((AppIcon *)p)->state); }
static void icon_destroy(void *p) { cui_icon_state_destroy(&((AppIcon *)p)->state); }
static const char *brand_icon_name(const char *name) {
    static const struct { const char *from, *to; } aliases[] = {
        {"settings", "gear"}, {"panel-left", "panel-l"}, {"panel-right", "panel-r"},
        {"chevron-down", "down"}, {"chevron-right", "chev"}, {"network", "stream"},
        {"frame", "monitor"}, {"circle-user", "cam"}, {"file", "rec"},
    };
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; ++i)
        if (!strcmp(name, aliases[i].from)) return aliases[i].to;
    return name;
}
static void app_icon(cui_context *ui, cui_id id, const cui_theme *t, cui_str name,
                     cui_icon_options options) {
    char source[48], raw[32];
    snprintf(raw, sizeof raw, "%.*s", (int)name.length, name.data);
    snprintf(source, sizeof source, "cast-%s", brand_icon_name(raw));
    AppIcon *icon = cui_resource_get(ui, id, UINT64_C(0x6361737469636f6e), sizeof *icon,
                                     icon_collect, icon_destroy);
    if (!icon) return;
    if (!icon->initialized) { cui_icon_state_init(&icon->state); icon->initialized = true; }
    if (strcmp(icon->path, source)) {
        if (!cui_icon_set_path(&icon->state, cui_string(source), cast_brand_icon_loader, NULL)) return;
        snprintf(icon->path, sizeof icon->path, "%s", source);
    }
    cui_icon(ui, id, t, &icon->state, options);
}
typedef struct { cui_id id; const cui_theme *theme; const char *name; float size; } ButtonIcon;
static void button_icon(cui_context *ui, void *user) {
    ButtonIcon *icon = user;
    app_icon(ui, icon->id, icon->theme, cui_string(icon->name),
             (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = icon->size,
                 .custom_color = true, .color = icon->theme->text});
}

static const char *composition_layouts[] = {"overlay", "split", "screen", "camera", "stage"};
static const char *composition_anchors[] = {
    "top-left", "top", "top-right", "left", NULL, "right", "bottom-left", "bottom", "bottom-right"};
static bool composition_disabled(const AppState *s) {
    return s->edit_requested || s->live.pending || s->live.snapshot_busy || (s->live.enabled && !s->live.connected);
}
bool app_request_edit(AppState *s, AppEdit edit, const char *value) {
    if (!s || !value || !value[0] || strlen(value) >= sizeof s->edit_value || s->extension_requested || composition_disabled(s)) return false;
    int index = -1;
    float size = 0;
    if (edit == APP_EDIT_LAYOUT) {
        for (unsigned i = 0; i < APP_LAYOUT_COUNT; ++i)
            if (!strcmp(value, composition_layouts[i])) index = (int)i;
        if (index < 0) return false;
    } else if (edit == APP_EDIT_FIT) {
        if (strcmp(value, "contain") && strcmp(value, "cover")) return false;
    } else if (edit == APP_EDIT_ANCHOR) {
        for (unsigned i = 0; i < 9; ++i)
            if (composition_anchors[i] && !strcmp(value, composition_anchors[i])) index = (int)i;
        if (index < 0) return false;
    } else if (edit == APP_EDIT_SIZE) {
        bool dot = false;
        for (const char *p = value; *p; ++p) {
            if (*p == '.' && !dot) dot = true;
            else if (*p < '0' || *p > '9') return false;
        }
        char *end = NULL;
        size = strtof(value, &end);
        if (!end || *end || !isfinite(size) || size < 1 || size > 100) return false;
    } else if (edit == APP_EDIT_BACKGROUND_SOURCE) {
        if (strcmp(value,"screen") && strcmp(value,"camera")) return false;
    } else if (edit == APP_EDIT_SCREEN_BACKGROUND || edit == APP_EDIT_CAMERA_BACKGROUND) {
        if (strcmp(value,"blurred") && strcmp(value,"gradient") && strcmp(value,"solid")) return false;
    } else if (edit >= APP_EDIT_SCREEN_BACKGROUND_COLOR && edit <= APP_EDIT_GRADIENT_TO) {
        if (strlen(value)!=7 || value[0]!='#') return false;
        for (unsigned i=1;i<7;++i) if (!isxdigit((unsigned char)value[i])) return false;
    } else if (edit == APP_EDIT_GRADIENT_VIA_ENABLED || edit == APP_EDIT_ZOOM_FOLLOW) {
        if (strcmp(value,"true") && strcmp(value,"false")) return false;
        if (edit==APP_EDIT_ZOOM_FOLLOW && s->live.enabled && !s->live.can_zoom_follow) return false;
    } else if (edit == APP_EDIT_GRADIENT_ANGLE || edit == APP_EDIT_GRADIENT_WAYPOINT ||
               edit == APP_EDIT_ZOOM_FACTOR || edit == APP_EDIT_ZOOM_MIN || edit == APP_EDIT_ZOOM_MAX) {
        bool decimal=false;
        for(const char *p=value;*p;++p) {
            if(*p=='.'&&!decimal)decimal=true;
            else if(*p<'0'||*p>'9')return false;
        }
        char *end=NULL; size=strtof(value,&end);
        if (!end || *end || !isfinite(size)) return false;
        if (edit==APP_EDIT_GRADIENT_ANGLE) { if(size<0||size>360)return false; }
        else if(edit==APP_EDIT_GRADIENT_WAYPOINT) { if(size<1||size>99)return false; }
        else if(size<1||size>20)return false;
        if(s->live.enabled && edit==APP_EDIT_ZOOM_FACTOR && (size<s->live.zoom_min||size>s->live.zoom_max))return false;
        if(s->live.enabled && edit==APP_EDIT_ZOOM_MIN && (size>s->live.zoom_factor||size>s->live.zoom_max))return false;
        if(s->live.enabled && edit==APP_EDIT_ZOOM_MAX && (size<s->live.zoom_factor||size<s->live.zoom_min))return false;
    } else if(edit==APP_EDIT_RECORD_ACTION) {
        if(!s->live.enabled||!s->live.connected)return false;
        if(strcmp(value,"start")&&strcmp(value,"stop")&&strcmp(value,"pause")&&strcmp(value,"resume")&&strcmp(value,"cut")&&strcmp(value,"cancel"))return false;
        if(s->live.record_finalizing)return false;
        if(!strcmp(value,"start")&&(s->live.recording||s->live.record_countdown_active))return false;
        if(!strcmp(value,"cancel")&&!s->live.record_countdown_active)return false;
        if(!strcmp(value,"stop")&&!s->live.recording&&!s->live.record_countdown_active)return false;
        if((!strcmp(value,"pause")||!strcmp(value,"resume")||!strcmp(value,"cut"))&&s->live.record_countdown_active)return false;
        if((!strcmp(value,"pause")||!strcmp(value,"cut"))&&!s->live.recording)return false;
        if((!strcmp(value,"pause")||!strcmp(value,"cut"))&&(s->live.record_paused||s->live.record_cut))return false;
        if(!strcmp(value,"resume")&&(!s->live.recording||(!s->live.record_paused&&!s->live.record_cut)))return false;
    } else if(edit==APP_EDIT_RECORD_DIRECTORY) {
        for(const unsigned char *p=(const unsigned char *)value;*p;++p)if(*p<32||*p==127)return false;
    } else if(edit==APP_EDIT_RECORD_CONTAINER) {
        if(strcmp(value,"matroska")&&strcmp(value,"mp4")&&strcmp(value,"mov")&&strcmp(value,"webm"))return false;
    } else if(edit==APP_EDIT_RECORD_VIDEO_CODEC) {
        if(strcmp(value,"auto")&&strcmp(value,"libx264")&&strcmp(value,"libx265")&&strcmp(value,"libvpx-vp9")&&strcmp(value,"libaom-av1"))return false;
    } else if(edit==APP_EDIT_RECORD_AUDIO_CODEC) {
        if(strcmp(value,"aac")&&strcmp(value,"libopus")&&strcmp(value,"pcm_s16le")&&strcmp(value,"flac"))return false;
    } else if(edit==APP_EDIT_RECORD_CRF||edit==APP_EDIT_RECORD_COUNTDOWN||edit==APP_EDIT_AUDIO_MIC_GAIN||edit==APP_EDIT_AUDIO_DESKTOP_GAIN) {
        bool dot=false;
        for(const char *p=value;*p;++p) {
            if(*p=='.'&&!dot&&(edit==APP_EDIT_AUDIO_MIC_GAIN||edit==APP_EDIT_AUDIO_DESKTOP_GAIN))dot=true;
            else if(*p<'0'||*p>'9')return false;
        }
        char *end=NULL;size=strtof(value,&end);
        float max=edit==APP_EDIT_RECORD_CRF?51:edit==APP_EDIT_RECORD_COUNTDOWN?60:400;
        if(!end||*end||!isfinite(size)||size<0||size>max)return false;
    } else if(edit==APP_EDIT_AUDIO_MIC||edit==APP_EDIT_AUDIO_DESKTOP||edit==APP_EDIT_AUDIO_VIRTUAL) {
        if(strcmp(value,"true")&&strcmp(value,"false"))return false;
        if(edit==APP_EDIT_AUDIO_VIRTUAL&&!s->live.can_virtual_audio)return false;
    } else if(edit==APP_EDIT_NOTES_ACTION) {
        if(!s->live.enabled||!s->live.connected||!s->live.notes_available)return false;
        if(strcmp(value,"open")&&strcmp(value,"close"))return false;
    } else return false;
    if(edit>=APP_EDIT_RECORD_CONTAINER&&edit<=APP_EDIT_RECORD_DIRECTORY &&
       (s->live.recording||s->live.record_countdown_active||s->live.record_finalizing))return false;
    if (!s->live.enabled) {
        if (edit == APP_EDIT_LAYOUT) s->layout = (AppLayout)index;
        else if (edit == APP_EDIT_FIT) s->fit_cover = !strcmp(value, "cover");
        else if (edit == APP_EDIT_ANCHOR) s->camera_anchor = (unsigned)index;
        else if (edit==APP_EDIT_SIZE) s->camera_size = size;
        else if (edit==APP_EDIT_SCREEN_BACKGROUND) snprintf(s->live.screen_background,16,"%.15s",value);
        else if (edit==APP_EDIT_ZOOM_FACTOR) s->live.zoom_factor=size;
        else if(edit==APP_EDIT_BACKGROUND_SOURCE) snprintf(s->live.background_source,16,"%.15s",value);
        else if(edit==APP_EDIT_CAMERA_BACKGROUND) snprintf(s->live.camera_background,16,"%.15s",value);
        else if(edit>=APP_EDIT_SCREEN_BACKGROUND_COLOR && edit<=APP_EDIT_GRADIENT_TO) {
            uint32_t color=(uint32_t)strtoul(value+1,NULL,16);
            if(edit==APP_EDIT_SCREEN_BACKGROUND_COLOR)s->live.screen_background_color=color;
            else if(edit==APP_EDIT_CAMERA_BACKGROUND_COLOR)s->live.camera_background_color=color;
            else if(edit==APP_EDIT_GRADIENT_FROM)s->live.gradient_from=color;
            else if(edit==APP_EDIT_GRADIENT_VIA)s->live.gradient_via=color;
            else s->live.gradient_to=color;
        } else if(edit==APP_EDIT_GRADIENT_VIA_ENABLED)s->live.gradient_via_enabled=!strcmp(value,"true");
        else if(edit==APP_EDIT_GRADIENT_ANGLE)s->live.gradient_angle=size;
        else if(edit==APP_EDIT_GRADIENT_WAYPOINT)s->live.gradient_waypoint=size;
        else if(edit==APP_EDIT_ZOOM_MIN)s->live.zoom_min=size;
        else if(edit==APP_EDIT_ZOOM_MAX)s->live.zoom_max=size;
        else if(edit==APP_EDIT_ZOOM_FOLLOW)s->live.zoom_follow=!strcmp(value,"true");
        else return false;
        return true;
    }
    s->edit = edit;
    snprintf(s->edit_value, sizeof s->edit_value, "%s", value);
    s->edit_requested = true;
    return true;
}

static const char *workspace_ids[] = {"workspace-camera", "workspace-record", "workspace-stream"};
static const char *layout_ids[] = {"layout-overlay", "layout-split", "layout-screen", "layout-camera", "layout-stage"};

void app_init(AppState *s) {
    memset(s, 0, sizeof *s);
    s->appearance = APP_SYSTEM;
    s->system_dark = true;
    s->show_sources = s->show_inspector = true;
    s->source_sections[0] = s->source_sections[1] = true;
    s->camera_anchor = 8;
    s->camera_size = 26;
    snprintf(s->live.screen_background,16,"blurred");
    s->live.zoom_factor=s->live.zoom_min=1; s->live.zoom_max=2;
    s->live.gradient_from=0xf16c33;s->live.gradient_to=0x25262a;
    s->live.gradient_angle=135;
}
bool app_is_dark(const AppState *s) {
    return s->appearance == APP_DARK || (s->appearance == APP_SYSTEM && s->system_dark);
}
const char *app_workspace_name(AppWorkspace w) {
    const char *names[] = {"Virtual camera", "Record", "Stream"};
    return w < APP_WORKSPACE_COUNT ? names[w] : names[0];
}
const char *app_layout_name(AppLayout l) {
    const char *names[] = {"Overlay", "Split", "Screen", "Camera", "Stage"};
    return l < APP_LAYOUT_COUNT ? names[l] : names[0];
}
static const char *output_state(const AppState *s, unsigned lane) {
    const AppLive *l = &s->live;
    if (!l->connected) return "Not connected";
    if(lane==APP_RECORD) {
        if(l->record_finalizing)return "Finalising";
        if(l->record_countdown_active)return "Countdown";
        if(l->record_cut)return "Cut";
    }
    bool active = lane == 0 ? l->virtual_enabled : lane == 1 ? l->recording : l->stream_active;
    bool paused = lane == 0 ? l->virtual_paused : lane == 1 ? l->record_paused : l->stream_paused;
    bool frozen = lane == 0 ? l->virtual_frozen : lane == 1 ? l->record_frozen : l->stream_frozen;
    bool blurred = lane == 0 ? l->virtual_blurred : lane == 1 ? l->record_blurred : l->stream_blurred;
    return !active ? "Stopped" : paused ? "Paused" : frozen ? "Frozen" : blurred ? "Blurred" : "Running";
}
static void sync_live(AppState *s) {
    if (!s->live.connected) return;
    const char *layouts[] = {"overlay", "split", "screen", "camera", "stage"};
    const char *anchors[] = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"};
    for (unsigned i = 0; i < APP_LAYOUT_COUNT; ++i)
        if (!strcmp(s->live.layout, layouts[i])) s->layout = (AppLayout)i;
    for (unsigned i = 0; i < 9; ++i)
        if (!strcmp(s->live.anchor, anchors[i])) s->camera_anchor = i;
    if (!strcmp(s->live.anchor, "top-center")) s->camera_anchor = 1;
    if (!strcmp(s->live.anchor, "center-left")) s->camera_anchor = 3;
    if (!strcmp(s->live.anchor, "center-right")) s->camera_anchor = 5;
    if (!strcmp(s->live.anchor, "bottom-center")) s->camera_anchor = 7;
    s->fit_cover = !strcmp(s->live.fit, "cover");
    s->camera_size = s->live.camera_width_percent;
}
static void text(cui_context *ui, const cui_theme *t, const char *s) {
    cui_text(ui, cui_string(s), 12, t->muted);
}
static void title(cui_context *ui, const cui_theme *t, const char *s, float size) {
    cui_text_ex(ui, cui_string(s), size, t->text, 0,
                (cui_text_style){.weight = CUI_TEXT_WEIGHT_SEMIBOLD});
}
static void fill(cui_context *ui) {
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 1});
    cui_end_box(ui);
}
static void rule(cui_context *ui, const cui_theme *t) {
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 1, .background = t->border});
    cui_end_box(ui);
}
typedef struct {const cui_theme *theme; const char *label; bool selected;} CompactLabel;
static void compact_label(cui_context *ui,void *user) {
    CompactLabel *label=user;
    cui_text_ex(ui,cui_string(label->label),10.5f,label->selected?label->theme->text:cast_design_tokens(label->theme->dark).secondary,0,
        (cui_text_style){.family=1,.monospace=true});
}
static bool button(cui_context *ui, const cui_theme *t, const char *id, const char *label,
                   const char *icon, float width, float height, bool selected, bool disabled) {
    cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
    bool preset=!strncmp(id,"zoom-preset-",12)||!strncmp(id,"zoom-pop-preset-",16);
    bool background=!strncmp(id,"background-",11);
    colors.normal = preset || background || !strncmp(id,"fit-",4) || !strncmp(id,"camera-size-",12) ? t->background : t->secondary;
    colors.hover = cast_design_tokens(t->dark).hover;
    colors.active = selected ? t->selection : t->secondary_hover;
    if (selected) colors.border = cast_design_tokens(t->dark).selected_line;
    CompactLabel label_content={t,label,selected};
    ButtonIcon artwork = {cui_id_from(ui, id) ^ UINT64_C(0x7185e28e9fb974db), t, icon, 14};
    return cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(label),
        (cui_button_options){.variant = CUI_BUTTON_SECONDARY,
            .width = width, .height = height, .font_size = background?11.5f:12, .icon_size = 14,
            .compact=preset, .icon_only=preset, .content=preset?compact_label:NULL,.content_user=&label_content,
            .leading_content = icon ? button_icon : NULL, .leading_user = &artwork,
            .custom_colors = true, .colors = colors,
            .selected = selected, .disabled = disabled,
            .toggle_semantics = selected, .toggled = selected});
}
static bool icon_button(cui_context *ui, const cui_theme *t, const char *id,
                        const char *label, const char *icon, bool selected) {
    cui_theme_button_colors c = t->buttons[CUI_THEME_BUTTON_SECONDARY];
    c.normal = selected ? t->selection : (cui_color){0}; c.hover = cast_design_tokens(t->dark).hover;
    c.active = c.hover; c.border = selected ? cast_design_tokens(t->dark).selected_line : (cui_color){0};
    c.foreground = cast_design_tokens(t->dark).secondary;
    ButtonIcon artwork = {cui_id_from(ui, id) ^ UINT64_C(0x7185e28e9fb974db), t, icon, 16};
    return cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(label),
        (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .icon_only = true,
            .content = button_icon, .content_user = &artwork,
            .width = 28, .height = 28, .icon_size = 16,
            .custom_colors = true, .colors = c});
}
static void badge(cui_context *ui, const cui_theme *t, const char *label) {
    cui_begin_box(ui, 0, (cui_box){.padding = 5, .border_width = 1,
        .border = t->border, .background = t->background, .align_y = CUI_ALIGN_CENTER});
    cui_text(ui, cui_string(label), 10, t->muted);
    cui_end_box(ui);
}
static void open_panel(cui_context *ui, AppState *s, unsigned kind, cui_id trigger) {
    if (!s->panel_sheet_initialized) {
        cui_surface_init(&s->panel_sheet, cui_id_from(ui, "panel-drawer"), CUI_SURFACE_SHEET);
        s->panel_sheet_initialized = true;
    }
    s->drawer_kind = kind;
    cui_surface_open(ui, &s->panel_sheet, trigger);
}
static void toggle_panel(cui_context *ui, AppState *s, unsigned kind, float width, cui_id trigger) {
    if (width < (kind == 1 ? 1000 : 800)) open_panel(ui, s, kind, trigger);
    else if (kind == 1) s->show_sources = !s->show_sources;
    else s->show_inspector = !s->show_inspector;
    cui_invalidate(ui);
}
static void shortcuts(cui_context *ui, AppState *s, float width) {
    /* Bare character shortcuts must yield to the focused text editor. Semantics
     * are rebuilt later in this frame, so use these stable editor identities. */
    const char *editors[]={"background-from","background-via","background-to","background-fill"};
    for(unsigned i=0;i<sizeof editors/sizeof editors[0];++i)
        if(cui_focused(ui)==cui_id_from(ui,editors[i]))return;
    size_t count = 0;
    const cui_event *events = cui_frame_events(ui, &count);
    for (size_t i = 0; i < count; ++i) {
        const cui_event *e = &events[i];
        if (cui_event_consumed(ui, i) || e->type != CUI_EVENT_KEY_DOWN || e->repeat ||
            e->ime_composing || e->prefer_character_input) continue;
        if (s->settings.overlay.open || s->panel_sheet.overlay.open || s->source_picker.popup.overlay.open || s->zoom_popup.open) continue;
        bool handled = true;
        if (e->modifiers) continue;
        if(e->key==CUI_KEY_T) {
            if(!app_request_edit(s,APP_EDIT_NOTES_ACTION,s->live.notes_window_open?"close":"open"))continue;
        } else if(e->key==CUI_KEY_R) {
            if(!app_request_edit(s,APP_EDIT_RECORD_ACTION,s->live.record_countdown_active?"cancel":s->live.recording?"stop":"start"))continue;
        } else if(e->key==CUI_KEY_C) {
            if(!app_request_edit(s,APP_EDIT_RECORD_ACTION,"cut"))continue;
        } else if (e->key >= CUI_KEY_1 && e->key <= CUI_KEY_3) {
            s->workspace = (AppWorkspace)(e->key - CUI_KEY_1);
            s->workspace_tab = cui_id_from(ui, workspace_ids[s->workspace]);
        } else if (e->key == CUI_KEY_LEFT_BRACKET) {
            toggle_panel(ui, s, 1, width, cui_id_from(ui, "toggle-sources"));
        } else if (e->key == CUI_KEY_RIGHT_BRACKET) {
            toggle_panel(ui, s, 2, width, cui_id_from(ui, "toggle-inspector"));
        } else handled = false;
        if (handled) { cui_consume_event(ui, i); cui_invalidate(ui); }
    }
}
static void tabs(cui_context *ui, AppState *s, const cui_theme *t, bool narrow) {
    cui_choice_item choices[3];
    const char *icons[] = {"cam", "rec", "stream"};
    for (unsigned i = 0; i < 3; ++i)
        choices[i] = (cui_choice_item){cui_id_from(ui, workspace_ids[i]),
            cui_string(narrow && i == 0 ? "Camera" : app_workspace_name((AppWorkspace)i)), false};
    if (!s->workspace_tab) s->workspace_tab = choices[s->workspace].id;
    /* Keep CUI's roving Tab/arrow-key reducer; the reference uses a distinct
     * border, hover surface and inset underline absent from stock segmented tabs. */
    if (cui_base_tabs(ui, choices, 3, &s->workspace_tab, (cui_tabs_options){0})) {
        for (unsigned i = 0; i < 3; ++i)
            if (s->workspace_tab == choices[i].id) s->workspace = (AppWorkspace)i;
        cui_invalidate(ui);
    }
    cui_begin_box(ui, cui_id_from(ui, "workspace-tabs"),
        (cui_box){.height = 30, .fixed_height = true, .row = true, .gap = 0,
            .border_width = 1, .border = t->border, .background = t->background});
    cui_interact(ui, cui_id_from(ui, "workspace-tabs"), CUI_ROLE_TABLIST, cui_string("Workspace"), false, false, 0);
    cui_set_semantic_details(ui, cui_id_from(ui, "workspace-tabs"), (cui_semantic_details){.no_focus = true});
    for (unsigned i = 0; i < 3; ++i) {
        bool active = choices[i].id == s->workspace_tab, hovered = cui_hovered(ui, choices[i].id);
        cui_color foreground = active || hovered ? t->text : t->muted;
        cui_begin_box(ui, choices[i].id, (cui_box){.height = 28, .row = true, .gap = 7,
            .padding_edges = {.left = 16, .right = 16}, .custom_padding = true,
            .align_y = CUI_ALIGN_CENTER,
            .background = hovered ? cast_design_tokens(t->dark).hover : active ? t->selection : (cui_color){0},
            .border = t->border, .custom_border_edges = true,
            .border_edges = {.right = i < 2 ? 1 : 0}});
        cui_set_semantic_details(ui, choices[i].id, (cui_semantic_details){
            .selected = active, .parent = cui_id_from(ui, "workspace-tabs")});
        app_icon(ui, choices[i].id ^ UINT64_C(0x636173746d6f6465), t, cui_string(icons[i]),
            (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = 14,
                .custom_color = true, .color = foreground});
        cui_text_ex(ui, choices[i].label, 12.5f, foreground, 0,
                    (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM});
        /* An explicit local overlay paints the complete tab width after layout. */
        if (active) {
            float label_width = cui_measure_text(ui, choices[i].label, 12.5f).width;
            cui_begin_absolute_box(ui, choices[i].id ^ UINT64_C(0x756e6465726c696e),
                (cui_box){.background = t->primary}, (cui_rect){0, 26, 32 + 14 + 7 + label_width, 2});
            cui_end_box(ui);
        }
        cui_end_box(ui);
    }
    cui_end_box(ui);
}
static void chrome(cui_context *ui, AppState *s, const cui_theme *t, float width) {
    bool narrow = width < 800;
    cui_begin_box(ui, cui_id_from(ui, "titlebar"),
        (cui_box){.width = -1, .height = 38, .row = true, .gap = 9, .padding = 8,
            .align_y = CUI_ALIGN_CENTER, .background = t->surface, .clip = true});
    cast_brand_logo(ui, cui_id_from(ui, "brand-logo"), 18);
    title(ui, t, "cast", 13.5f);
    if (width >= 1150) text(ui, t, "Untitled composition");
    fill(ui);
    if (!narrow) tabs(ui, s, t, false);
    fill(ui);
    if (width >= 1050) button(ui, t, "pause-all", "Pause all", "pause", 0, 28, false, true);
    if (width >= 680) badge(ui, t, s->live.connected ? "Connected" : "Not connected");
    badge(ui, t, s->live.enabled ? (s->live.connected ? (s->live.pro ? "Pro" : "Community") : "Read only") : "Preview");
    cui_end_box(ui);
    rule(ui, t);
    if (narrow) {
        cui_begin_box(ui, cui_id_from(ui, "mobile-tabs"),
            (cui_box){.width = -1, .height = 38, .row = true, .padding = 4, .align_x = CUI_ALIGN_CENTER});
        tabs(ui, s, t, true);
        cui_end_box(ui);
    }
    app_menu_bar(ui, s, t, width);
    rule(ui, t);
}
typedef struct { const cui_theme *theme; AppLayout layout; bool selected; } LayoutThumb;
static void layout_thumb(cui_context *ui, void *user) {
    const LayoutThumb *thumb = user;
    cui_color screen = thumb->selected ? thumb->theme->text : thumb->theme->muted;
    screen.a = thumb->selected ? 230 : 191;
    const cui_color camera = thumb->theme->primary;
    cui_begin_box(ui, 0, (cui_box){.width = 28, .height = 16,
                                  .fixed_width = true, .fixed_height = true});
    if (thumb->layout != APP_WEBCAM) {
        cui_rect r = thumb->layout == APP_SPLIT ? (cui_rect){0, 0, 17, 16} :
                     thumb->layout == APP_STAGE ? (cui_rect){0, 2, 17, 12} :
                     (cui_rect){0, 0, 28, 16};
        cui_begin_absolute_box(ui, 0, (cui_box){.background = screen}, r);
        cui_end_box(ui);
    }
    if (thumb->layout != APP_SCREEN) {
        cui_rect r = thumb->layout == APP_SPLIT ? (cui_rect){18, 0, 10, 16} :
                     thumb->layout == APP_WEBCAM ? (cui_rect){0, 0, 28, 16} :
                     thumb->layout == APP_STAGE ? (cui_rect){19, 3, 9, 10} :
                     (cui_rect){20, 8, 8, 8};
        float radius = thumb->layout == APP_OVERLAY || thumb->layout == APP_STAGE ? 5 : 0;
        cui_begin_absolute_box(ui, 0, (cui_box){.background = camera, .radius = radius}, r);
        cui_end_box(ui);
    }
    cui_end_box(ui);
}
static void layout_picker_ex(cui_context *ui, AppState *s, const cui_theme *t, bool stagebar) {
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 4});
    for (unsigned i = 0; i < APP_LAYOUT_COUNT; ++i) {
        LayoutThumb thumb = {t, (AppLayout)i, s->layout == (AppLayout)i};
        cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
        colors.normal = t->background;
        colors.hover = cast_design_tokens(t->dark).hover;
        colors.active = t->selection;
        colors.border = thumb.selected ? cast_design_tokens(t->dark).selected_line : t->border;
        char thumb_id[48];
        snprintf(thumb_id, sizeof thumb_id, "%s%s", stagebar ? "stagebar-" : "", layout_ids[i]);
        if (cui_button_ex(ui, cui_id_from(ui, thumb_id), t,
            cui_string(app_layout_name((AppLayout)i)),
            (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = -1, .height = 30,
                .icon_only = true, .content = layout_thumb, .content_user = &thumb,
                .selected = thumb.selected, .toggle_semantics = true, .toggled = thumb.selected,
                .disabled = composition_disabled(s), .custom_colors = true, .colors = colors})) {
            app_request_edit(s, APP_EDIT_LAYOUT, composition_layouts[i]);
            cui_invalidate(ui);
        }
    }
    cui_end_box(ui);
}
static void layout_picker(cui_context *ui, AppState *s, const cui_theme *t) {
    layout_picker_ex(ui, s, t, false);
}
typedef struct { const cui_theme *theme; const char *label; bool open; } SectionStyle;
static void section_body(cui_context *ui, void *user) {
    const SectionStyle *style = user;
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 7,
        .padding_edges = {.left = 10, .right = 10}, .custom_padding = true,
        .align_y = CUI_ALIGN_CENTER});
    app_icon(ui, cui_id_from(ui, style->label) ^ UINT64_C(0x6361726574), style->theme,
        cui_string(style->open ? "down" : "chev"), (cui_icon_options){
            .size = CUI_ICON_CUSTOM, .custom_size = 12, .custom_color = true,
            .color = style->theme->muted});
    char caps[64]; size_t n = strlen(style->label);
    for (size_t i = 0; i <= n && i < sizeof caps; ++i)
        caps[i] = style->label[i] >= 'a' && style->label[i] <= 'z' ? style->label[i] - 32 : style->label[i];
    cui_text_ex(ui, cui_string(caps), 10, style->theme->muted, 0,
                (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM});
    cui_end_box(ui);
}
static bool section(cui_context *ui, AppState *s, const cui_theme *t,
                    unsigned index, const char *name) {
    const char *ids[] = {"section-composition", "section-camera", "section-look", "section-presets"};
    SectionStyle style = {t, name, s->source_sections[index]};
    cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
    colors.normal = (cui_color){0}; colors.hover = cast_design_tokens(t->dark).hover;
    colors.active = colors.hover; colors.border = (cui_color){0};
    if (cui_button_ex(ui, cui_id_from(ui, ids[index]), t, cui_string(name),
        (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = -1, .height = 32,
            .icon_only = true, .content = section_body, .content_user = &style,
            .custom_colors = true, .colors = colors})) {
        s->source_sections[index] = !s->source_sections[index];
        cui_invalidate(ui);
    }
    return s->source_sections[index];
}
typedef struct { const cui_theme *theme; bool selected; } AnchorDot;
static void anchor_dot(cui_context *ui, void *user) {
    const AnchorDot *dot = user;
    cui_begin_box(ui, 0, (cui_box){.width = 7, .height = 7,
        .fixed_width = true, .fixed_height = true,
        .background = dot->selected ? dot->theme->primary : dot->theme->muted});
    cui_end_box(ui);
}
typedef struct { cui_edit *edit; cui_input_state state; char buffer[16]; uint32_t acknowledged; bool initialized, connected, was_focused; uint64_t generation; } LookColor;
static void look_color_destroy(void *p) { cui_edit_destroy(((LookColor *)p)->edit); }
static void look_color(cui_context *ui, AppState *s, const cui_theme *t, const char *id,
                       const char *label, uint32_t value, AppEdit intent) {
    text(ui,t,label);
    cui_id key=cui_id_from(ui,id);
    LookColor *field=cui_resource_get(ui,key,UINT64_C(0x6c6f6f6b636f6c72),sizeof *field,NULL,look_color_destroy);
    if(!field)return;
    if(!field->initialized) {
        snprintf(field->buffer,sizeof field->buffer,"#%06X",value&0xffffff);
        field->edit=cui_edit_create((cui_edit_config){.buffer=field->buffer,.capacity=sizeof field->buffer});
        field->initialized=true; field->acknowledged=value;
    }
    if(!field->edit)return;
    bool focused=cui_focused(ui)==key;
    bool reset=field->acknowledged!=value || field->generation!=s->live.generation ||
        field->connected!=s->live.connected || (field->was_focused&&!focused);
    size_t event_count=0;const cui_event *events=cui_frame_events(ui,&event_count);
    for(size_t i=0;i<event_count;++i)if(focused&&!cui_event_consumed(ui,i)&&events[i].type==CUI_EVENT_KEY_DOWN&&events[i].key==CUI_KEY_ESCAPE) {
        reset=true;cui_consume_event(ui,i);
    }
    field->generation=s->live.generation;field->connected=s->live.connected;field->was_focused=focused;
    if(reset) {
        char hex[16];snprintf(hex,sizeof hex,"#%06X",value&0xffffff);
        cui_edit_set_content(field->edit,(cui_edit_content){.text=cui_string(hex)});
        field->acknowledged=value;
    }
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    cui_begin_box(ui,0,(cui_box){.width=22,.height=22,.fixed_width=true,.background={(value>>16)&255,(value>>8)&255,value&255,255},.border=t->border,.border_width=1});cui_end_box(ui);
    cui_input_result result=cui_input_ex(ui,key,t,cui_string(label),field->edit,&field->state,
        (cui_input_options){.width=-1,.height=36,.disabled=composition_disabled(s),.invalid=strlen(field->buffer)!=7});
    if(result.submitted) {
        (void)app_request_edit(s,intent,field->buffer);
        char hex[16];snprintf(hex,sizeof hex,"#%06X",value&0xffffff);
        cui_edit_set_content(field->edit,(cui_edit_content){.text=cui_string(hex)});
        cui_invalidate(ui);
    }
    cui_end_box(ui);
}
typedef struct { AppState *state; } ZoomRequest;
static void zoom_request(void *user,const cui_component_slider_request *request) {
    if(request->phase!=CUI_SLIDER_COMMIT)return;
    char value[24];snprintf(value,sizeof value,"%.2f",request->requested);
    app_request_edit(((ZoomRequest *)user)->state,APP_EDIT_ZOOM_FACTOR,value);
}
static void zoom_control(cui_context *ui,AppState *s,const cui_theme *t,const char *name,float width) {
    cui_id id=cui_id_from(ui,name);
    cui_slider_state *state=cui_resource_get(ui,id,UINT64_C(0x7a6f6f6d736c6964),sizeof *state,NULL,NULL);
    double value=s->live.zoom_factor>0?s->live.zoom_factor:1;
    double min=s->live.zoom_min>=1?s->live.zoom_min:1,max=s->live.zoom_max>=min?s->live.zoom_max:2;
    ZoomRequest request={s};
    if(state)cui_slider_events(ui,id,t,cui_string("Zoom level"),value,state,
        (cui_slider_options){.minimum=min,.maximum=max,.step=.05,.disabled=composition_disabled(s)},
        (cui_slider_style_options){.width=width,.track_height=4,.hit_height=24,.hide_label=true},
        (cui_component_slider_handler){zoom_request,&request});
}
static void zoom_popup(cui_context *ui,AppState *s,const cui_theme *t,float width,float height) {
    if(!s->zoom_popup_initialized)return;
    if(!cui_overlay_begin(ui,&s->zoom_popup))return;
    cui_rect anchor=cui_bounds(ui,cui_id_from(ui,"stage-zoom"));
    float popup_width=fminf(238,fmaxf(0,width-16));
    cui_rect panel=cui_popup_place(anchor,(cui_size){popup_width,244},
        (cui_rect){8,8,fmaxf(0,width-16),fmaxf(0,height-16)},CUI_POPUP_ABOVE,6).bounds;
    cui_overlay_set_bounds(&s->zoom_popup,panel);
    cui_begin_portal(ui,s->zoom_popup.id,(cui_box){.padding=10,.gap=10,.background=t->surface,.border=t->border,.border_width=1},panel,230);
    cui_box_shadow shadow={.layer={.offset_y=18,.sigma=22,.color={16,17,19,t->dark?184:51}}};
    cui_set_box_shadows(ui,&shadow,1,(cui_corner_radii){0});
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=6,.align_y=CUI_ALIGN_CENTER});
    app_icon(ui,cui_id_from(ui,"zoom-pop-icon"),t,cui_string("zoom"),(cui_icon_options){.size=CUI_ICON_CUSTOM,.custom_size=14});
    title(ui,t,"ZOOM",10);cui_end_box(ui);
    text(ui,t,"Basic zoom · free");
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=3});
    const float levels[]={1,1.15f,1.3f,1.5f,1.8f};
    for(unsigned i=0;i<5;++i) {
        char id[40],label[24],value[24];snprintf(id,sizeof id,"zoom-pop-preset-%u",i);
        snprintf(label,sizeof label,i?"%.2f×":"Off",levels[i]);snprintf(value,sizeof value,"%.2f",levels[i]);
        if(button(ui,t,id,label,NULL,-1,30,fabsf(s->live.zoom_factor-levels[i])<.001f,
            composition_disabled(s)||(s->live.enabled&&(levels[i]<s->live.zoom_min||levels[i]>s->live.zoom_max)))) {
            app_request_edit(s,APP_EDIT_ZOOM_FACTOR,value);cui_invalidate(ui);
        }
    }
    cui_end_box(ui);
    char level[40];snprintf(level,sizeof level,"Level %.2f×",s->live.zoom_factor>0?s->live.zoom_factor:1);text(ui,t,level);
    zoom_control(ui,s,t,"zoom-pop-level",popup_width-20);
    bool follow=s->live.zoom_follow;
    if(cui_switch_ex(ui,cui_id_from(ui,"zoom-follow"),t,cui_string("Follow pointer"),&follow,NULL,
        (cui_switch_options){.disabled=composition_disabled(s)||!s->live.can_zoom_follow,
            .accessibility_label=cui_string("Follow pointer")})) {
        app_request_edit(s,APP_EDIT_ZOOM_FOLLOW,follow?"true":"false");cui_invalidate(ui);
    }
    text(ui,t,s->live.cinematic_zoom_active?"Cinematic zoom · active":s->live.cinematic_zoom_available?"Cinematic zoom · available":"Cinematic zoom · Pro");
    cui_end_box(ui);cui_overlay_end(ui);
}
static void look_controls(cui_context *ui,AppState *s,const cui_theme *t) {
    text(ui,t,"Background behind the screen");
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=0});
    const char *labels[]={"Blur","Gradient","Solid"},*values[]={"blurred","gradient","solid"};
    for(unsigned i=0;i<3;++i) {
        char id[40];snprintf(id,sizeof id,"background-%s",values[i]);
        if(button(ui,t,id,labels[i],NULL,-1,30,!strcmp(s->live.screen_background,values[i]),composition_disabled(s))) {
            app_request_edit(s,APP_EDIT_SCREEN_BACKGROUND,values[i]);cui_invalidate(ui);
        }
    }
    cui_end_box(ui);
    if(!strcmp(s->live.screen_background,"gradient")) {
        look_color(ui,s,t,"background-from","From",s->live.gradient_from,APP_EDIT_GRADIENT_FROM);
        bool via=s->live.gradient_via_enabled;
        if(cui_switch_ex(ui,cui_id_from(ui,"background-middle-enabled"),t,cui_string("Middle stop"),&via,NULL,
            (cui_switch_options){.disabled=composition_disabled(s),.accessibility_label=cui_string("Middle gradient stop")})) {
            app_request_edit(s,APP_EDIT_GRADIENT_VIA_ENABLED,via?"true":"false");cui_invalidate(ui);
        }
        if(s->live.gradient_via_enabled)look_color(ui,s,t,"background-via","Middle",s->live.gradient_via,APP_EDIT_GRADIENT_VIA);
        look_color(ui,s,t,"background-to","To",s->live.gradient_to,APP_EDIT_GRADIENT_TO);
        char note[64];snprintf(note,sizeof note,"%s stops, %.0f°",s->live.gradient_via_enabled?"Three":"Two",s->live.gradient_angle);
        text(ui,t,note);
    } else if(!strcmp(s->live.screen_background,"solid"))
        look_color(ui,s,t,"background-fill","Fill",s->live.screen_background_color,APP_EDIT_SCREEN_BACKGROUND_COLOR);
    text(ui,t,"Enter a hex colour and press Enter.");
}
static void source_content(cui_context *ui, AppState *s, const cui_theme *t) {
    cui_begin_box(ui, cui_id_from(ui, "source-content"),
        (cui_box){.width = -1, .padding = 10, .gap = 10});
    if (s->workspace == APP_STREAM) {
        title(ui, t, "Destination", 12);
        text(ui, t, s->live.connected ? output_state(s, APP_STREAM) : "No destination connected");
        button(ui, t, "destination-choose", "Set up destination", "network", -1, 36, false, true);
        rule(ui, t);
    }
    if (s->workspace == APP_RECORD) {
        title(ui, t, "Recording", 12);
        text(ui, t, s->live.connected ? output_state(s, APP_RECORD) : "No take connected");
        text(ui, t, "A recording writes to one file. Pauses and cuts keep the same take.");
        rule(ui, t);
    }
    if (section(ui, s, t, 0, "Composition")) {
        text(ui, t, "Capture source");
        app_source_picker(ui, s, t, APP_DEVICE_SCREEN, "source-choose",
            s->live.connected ? s->live.screen_name : "No source connected");
        text(ui, t, "Layout");
        layout_picker(ui, s, t);
        cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 4});
        if (button(ui, t, "fit-contain", "Contain", NULL, -1, 30, !s->fit_cover, composition_disabled(s))) {
            app_request_edit(s, APP_EDIT_FIT, "contain"); cui_invalidate(ui);
        }
        if (button(ui, t, "fit-cover", "Cover", NULL, -1, 30, s->fit_cover, composition_disabled(s))) {
            app_request_edit(s, APP_EDIT_FIT, "cover"); cui_invalidate(ui);
        }
        cui_end_box(ui);
        text(ui, t, s->live.enabled ? "Acknowledged composition" : "Layout choices preview locally until capture is connected.");
    }
    rule(ui, t);
    if (section(ui, s, t, 1, "Camera")) {
        text(ui, t, "Device");
        app_source_picker(ui, s, t, APP_DEVICE_CAMERA, "camera-choose",
            s->live.connected ? (s->live.camera_enabled ? s->live.camera_name : "No camera") : "No camera connected");
        text(ui, t, "Position");
        cui_begin_box(ui,0,(cui_box){.width=-1,.gap=4});
        for (unsigned row = 0; row < 3; ++row) {
            cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 4});
            for (unsigned col = 0; col < 3; ++col) {
                unsigned anchor = row * 3 + col;
                char id[32], name[48];
                const char *rows[] = {"Top", "Center", "Bottom"};
                const char *cols[] = {"left", "center", "right"};
                snprintf(id, sizeof id, "anchor-%u", anchor);
                snprintf(name, sizeof name, "%s %s", rows[row], cols[col]);
                cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
                colors.active = t->selection;
                colors.normal = t->background;
                colors.border = s->camera_anchor == anchor ? cast_design_tokens(t->dark).selected_line : t->border;
                AnchorDot dot = {t, s->camera_anchor == anchor};
                if (cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(name),
                    (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = 64,
                        .height = 64, .disabled = composition_disabled(s) || anchor == 4,
                        .content = anchor == 4 ? NULL : anchor_dot, .content_user = &dot,
                        .icon_only = true,
                        .selected = s->camera_anchor == anchor, .toggle_semantics = true,
                        .toggled = s->camera_anchor == anchor, .custom_colors = true, .colors = colors})) {
                    app_request_edit(s, APP_EDIT_ANCHOR, composition_anchors[anchor]); cui_invalidate(ui);
                }
            }
            cui_end_box(ui);
        }
        cui_end_box(ui);
        text(ui, t, "Size");
        cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 4});
        const char *labels[] = {"Wide", "Medium", "Close"};
        const float sizes[] = {20, 26, 38};
        for (unsigned i = 0; i < 3; ++i) {
            char id[32]; snprintf(id, sizeof id, "camera-size-%u", i);
            if (button(ui, t, id, labels[i], NULL, -1, 30, s->camera_size == sizes[i], composition_disabled(s))) {
                char value[16]; snprintf(value, sizeof value, "%.0f", sizes[i]);
                app_request_edit(s, APP_EDIT_SIZE, value); cui_invalidate(ui);
            }
        }
        cui_end_box(ui);
    }
    rule(ui, t);
    if (section(ui, s, t, 2, "Look"))
        look_controls(ui,s,t);
    rule(ui, t);
    if (section(ui, s, t, 3, "Presets"))
        text(ui, t, "Saved presets will appear here after daemon integration.");
    cui_end_box(ui);
}
static void card_begin(cui_context *ui, const cui_theme *t, const char *id, const char *heading) {
    cui_begin_box(ui, cui_id_from(ui, id), (cui_box){.width = -1, .padding = 10, .gap = 9,
        .background = t->surface, .border = t->border, .border_width = 1});
    const char *icon=!strcmp(id,"output-card")?(!strcmp(heading,"Recording")?"rec":!strcmp(heading,"Virtual camera")?"eye":"stream"):!strcmp(id,"audio-card")?"sliders":!strcmp(id,"details-card")?(!strcmp(heading,"Take format")?"film":!strcmp(heading,"Where it lands")?"cam":"stream"):NULL;
    if(icon)cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    if(icon)app_icon(ui,cui_id_from(ui,heading)^UINT64_C(0x6361726469636f6e),t,cui_string(icon),(cui_icon_options){.size=CUI_ICON_CUSTOM,.custom_size=16});
    title(ui, t, heading, 12.5f);
    if(icon)cui_end_box(ui);
}
typedef struct { const cui_theme *theme; const char *label; float size; } FramingLabel;
static void framing_label(cui_context *ui, void *user) {
    const FramingLabel *label = user;
    cui_begin_box(ui, 0, (cui_box){.gap = 1, .align_x = CUI_ALIGN_CENTER});
    cui_text(ui, cui_string(label->label), 11, cast_design_tokens(label->theme->dark).secondary);
    char value[16]; snprintf(value, sizeof value, "%.0f%%", label->size);
    cui_text(ui, cui_string(value), 9, label->theme->muted);
    cui_end_box(ui);
}
static void director_card(cui_context *ui, AppState *s, const cui_theme *t) {
    card_begin(ui, t, "director-card", "Director");
    text(ui, t, "Framing and zoom. Privacy stays on the bar under the stage.");
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .align_y = CUI_ALIGN_CENTER});
    cui_text(ui, cui_string("Framing"), 11.5f, cast_design_tokens(t->dark).secondary);
    fill(ui);
    char percent[24]; snprintf(percent, sizeof percent, "%.0f%%", s->camera_size);
    cui_text(ui, cui_string(percent), 10.5f, t->muted);
    cui_end_box(ui);
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 3});
    const char *names[] = {"Wide", "Medium", "Close"};
    const float sizes[] = {20, 26, 38};
    for (unsigned i = 0; i < 3; ++i) {
        char id[40]; snprintf(id, sizeof id, "director-framing-%u", i);
        FramingLabel label = {t, names[i], sizes[i]};
        bool selected = s->camera_size == sizes[i];
        cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
        colors.normal = t->background; colors.active = t->selection;
        colors.border = selected ? cast_design_tokens(t->dark).selected_line : t->border;
        if (cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(names[i]),
            (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = -1, .height = 34,
                .icon_only = true, .content = framing_label, .content_user = &label,
                .selected = selected, .toggle_semantics = true, .toggled = selected,
                .disabled = composition_disabled(s), .custom_colors = true, .colors = colors})) {
            char value[16]; snprintf(value, sizeof value, "%.0f", sizes[i]);
                app_request_edit(s, APP_EDIT_SIZE, value); cui_invalidate(ui);
        }
    }
    cui_end_box(ui);
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true});
    text(ui,t,"Zoom");fill(ui);
    char level[24];snprintf(level,sizeof level,"%.2f×",s->live.zoom_factor>0?s->live.zoom_factor:1);text(ui,t,level);cui_end_box(ui);
    float director_width=cui_bounds(ui,cui_id_from(ui,"director-card")).w;
    zoom_control(ui,s,t,"director-zoom",director_width>22?director_width-22:258);
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=3});
    const float levels[]={1,1.15f,1.3f,1.5f,1.8f};
    for(unsigned i=0;i<5;++i) {
        char id[32],label[24],value[24];snprintf(id,sizeof id,"zoom-preset-%u",i);
        snprintf(label,sizeof label,i?"%.2f×":"Off",levels[i]);snprintf(value,sizeof value,"%.2f",levels[i]);
        bool disabled=composition_disabled(s)||(s->live.enabled&&(levels[i]<s->live.zoom_min||levels[i]>s->live.zoom_max));
        if(button(ui,t,id,label,NULL,-1,30,fabsf(s->live.zoom_factor-levels[i])<.001f,disabled)) {app_request_edit(s,APP_EDIT_ZOOM_FACTOR,value);cui_invalidate(ui);}
    }
    cui_end_box(ui);
    text(ui,t,s->live.cinematic_zoom_active?"Cinematic zoom · active":s->live.cinematic_zoom_available?"Cinematic zoom · available":"Cinematic zoom · Pro");
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .align_y = CUI_ALIGN_CENTER});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .gap = 2});
    cui_text(ui, cui_string("Move the screen only"), 12, t->text);
    cui_text(ui, cui_string("The camera tile keeps its size"), 10.5f, t->muted);
    cui_end_box(ui);
    bool unavailable_screen_only = false;
    cui_switch_ex(ui, cui_id_from(ui, "director-screen-only"), t,
        cui_string(""), &unavailable_screen_only, NULL,
        (cui_switch_options){.width=34,.disabled=true,.size=CUI_CONTROL_SMALL,
            .accessibility_label=cui_string("Move the screen only unavailable")});
    cui_end_box(ui);
    cui_end_box(ui);
}
static void record_action(cui_context *ui,AppState *s,const cui_theme *t,const char *id,const char *label,const char *icon,const char *action,bool primary) {
    ButtonIcon artwork={cui_id_from(ui,id)^UINT64_C(0x7265636f72646963),t,icon,14};
    if(cui_button_ex(ui,cui_id_from(ui,id),t,cui_string(label),(cui_button_options){
        .variant=primary?CUI_BUTTON_PRIMARY:CUI_BUTTON_SECONDARY,.width=-1,.height=36,.font_size=12,
        .leading_content=icon?button_icon:NULL,.leading_user=&artwork,
        .disabled=composition_disabled(s)||!s->live.enabled||!s->live.connected||s->live.record_finalizing})) {
        app_request_edit(s,APP_EDIT_RECORD_ACTION,action);cui_invalidate(ui);
    }
}
static void recording_output(cui_context *ui,AppState *s,const cui_theme *t) {
    card_begin(ui,t,"output-card","Recording");
    char status[96];
    if(s->live.record_finalizing)snprintf(status,sizeof status,"Finalising…");
    else if(s->live.record_countdown_active)snprintf(status,sizeof status,"Recording in %.0f…",ceil((double)s->live.record_countdown_remaining_ns/1e9));
    else snprintf(status,sizeof status,"%s",!s->live.connected?"Not connected":s->live.record_cut?"Cut · ready to resume":s->live.record_paused?"Paused":s->live.recording?"Recording":"Off");
    badge(ui,t,status);
    text(ui,t,"A take writes to one file. Cuts omit media time; resuming continues that same file.");
    if(s->live.record_path[0])cui_text_ellipsis(ui,cui_id_from(ui,"record-current-path"),cui_string(s->live.record_path),
        (cui_text_ellipsis_options){.width=-1,.font_size=11,.color=t->muted});
    if(s->live.record_cut)text(ui,t,"Cut omits media time. Resume keeps the same file.");
    else if(s->live.record_paused)text(ui,t,"Paused: the take receives neutral video and silent audio.");
    if(s->live.record_finalizing)record_action(ui,s,t,"output-start","Finalising…",NULL,"stop",false);
    else if(s->live.record_countdown_active)record_action(ui,s,t,"record-cancel","Cancel countdown",NULL,"cancel",false);
    else if(!s->live.recording)record_action(ui,s,t,"output-start","Start recording","rec","start",true);
    else {
        bool paused=s->live.record_paused||s->live.record_cut;
        record_action(ui,s,t,paused?"record-resume":"record-cut",paused?"Resume recording":"Cut at playhead",paused?"play":"cut",paused?"resume":"cut",true);
        cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=6});
        if(!paused)record_action(ui,s,t,"record-pause","Pause","pause","pause",false);
        record_action(ui,s,t,"output-start","Stop","stop","stop",false);
        cui_end_box(ui);
    }
    if(s->live.recording) {
        char duration[64];uint64_t seconds=s->live.record_duration_ns/UINT64_C(1000000000);
        snprintf(duration,sizeof duration,"%02llu:%02llu:%02llu",(unsigned long long)(seconds/3600),(unsigned long long)(seconds/60%60),(unsigned long long)(seconds%60));text(ui,t,duration);
    }
    if(s->live.error[0])text(ui,t,s->live.error);
    cui_end_box(ui);
}
typedef struct {AppState *app;AppEdit edit;} AudioGainRequest;
static void audio_gain_request(void *user,const cui_component_slider_request *request) {
    if(request->phase!=CUI_SLIDER_COMMIT)return;
    AudioGainRequest *gain=user;char value[24];
    snprintf(value,sizeof value,"%.2f",pow(10,request->requested/20)*100);
    app_request_edit(gain->app,gain->edit,value);
}
static void audio_channel(cui_context *ui,AppState *s,const cui_theme *t,bool mic) {
    const char *name=mic?"Mic":"Desktop";
    const char *switch_id=mic?"audio-mic-enabled":"audio-desktop-enabled";
    float gain=mic?s->live.mic_gain:s->live.desktop_gain;
    bool enabled=mic?s->live.mic:s->live.desktop;
    cui_begin_box(ui,cui_id_from(ui,mic?"audio-mic-channel":"audio-desktop-channel"),
        (cui_box){.width=-1,.gap=2,.custom_padding=true,.padding_edges={.top=7,.bottom=7},
            .custom_border_edges=true,.border_edges={.bottom=1},.border=t->border});
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    cui_text(ui,cui_string(name),11.5f,t->text);fill(ui);
    char db[24];if(gain>0)snprintf(db,sizeof db,"%+.1f dB",20*log10f(gain/100));else snprintf(db,sizeof db,"−∞ dB");
    cui_text_ex(ui,cui_string(db),10.5f,cast_design_tokens(t->dark).secondary,0,(cui_text_style){.family=1,.monospace=true});
    if(cui_switch_ex(ui,cui_id_from(ui,switch_id),t,cui_string(""),&enabled,NULL,
        (cui_switch_options){.width=28,.size=CUI_CONTROL_SMALL,.disabled=composition_disabled(s)||!s->live.connected,
            .accessibility_label=cui_string(mic?"Microphone enabled":"Desktop audio enabled")})) {
        app_request_edit(s,mic?APP_EDIT_AUDIO_MIC:APP_EDIT_AUDIO_DESKTOP,enabled?"true":"false");cui_invalidate(ui);
    }
    cui_end_box(ui);
    app_source_picker(ui,s,t,mic?APP_DEVICE_MIC:APP_DEVICE_DESKTOP,mic?"audio-mic-source":"audio-desktop-source",
        s->live.connected?(mic?s->live.mic_name:s->live.desktop_name):"No audio device connected");
    cui_id id=cui_id_from(ui,mic?"audio-mic-gain":"audio-desktop-gain");
    cui_slider_state *state=cui_resource_get(ui,id,UINT64_C(0x617564696f676169),sizeof *state,NULL,NULL);
    AudioGainRequest request={s,mic?APP_EDIT_AUDIO_MIC_GAIN:APP_EDIT_AUDIO_DESKTOP_GAIN};
    float card_width=cui_bounds(ui,cui_id_from(ui,"audio-card")).w;
    float width=card_width>22?card_width-22:258;
    if(state)cui_slider_events(ui,id,t,cui_string(mic?"Microphone gain":"Desktop audio gain"),fmax(-24,fmin(12,gain>0?20*log10(gain/100):-24)),state,
        (cui_slider_options){.minimum=-24,.maximum=12,.step=1,.disabled=composition_disabled(s)||!s->live.connected},
        (cui_slider_style_options){.width=width,.track_height=4,.hit_height=24,.hide_label=true},
        (cui_component_slider_handler){audio_gain_request,&request});
    cui_end_box(ui);
}
static void audio_card(cui_context *ui,AppState *s,const cui_theme *t) {
    card_begin(ui,t,"audio-card",s->workspace==APP_RECORD?"Audio mixer":"Audio");
    audio_channel(ui,s,t,true);audio_channel(ui,s,t,false);
    bool enabled=s->live.virtual_audio;
    if(cui_switch_ex(ui,cui_id_from(ui,"audio-virtual-enabled"),t,cui_string("Virtual microphone"),&enabled,NULL,
        (cui_switch_options){.disabled=composition_disabled(s)||!s->live.connected||!s->live.can_virtual_audio,
            .accessibility_label=cui_string("Virtual microphone")})) {
        app_request_edit(s,APP_EDIT_AUDIO_VIRTUAL,enabled?"true":"false");cui_invalidate(ui);
    }
    text(ui,t,s->workspace==APP_CAMERA?"Expose the same mix to calls through the virtual microphone.":"The mix is shared by recording and streaming. Muting a channel mutes it everywhere.");
    if(s->live.virtual_audio&&s->live.virtual_silent)text(ui,t,"Virtual microphone is silent while the virtual camera is private.");
    text(ui,t,"Live level meters are unavailable.");
    if(s->live.connected)text(ui,t,!s->live.audio_available?"Audio backend unavailable":
        s->live.mic_ready&&s->live.desktop_ready?"Microphone and desktop audio ready":
        s->live.mic_ready?"Microphone ready · desktop audio unavailable":
        s->live.desktop_ready?"Desktop audio ready · microphone unavailable":"Audio sources unavailable");
    cui_end_box(ui);
}
static void format_row(cui_context *ui,const cui_theme *t,const char *label,const char *value) {
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    cui_text(ui,cui_string(label),11.5f,cast_design_tokens(t->dark).secondary);fill(ui);
    cui_text_ex(ui,cui_string(value),11,cast_design_tokens(t->dark).secondary,0,(cui_text_style){.family=1,.monospace=true});cui_end_box(ui);
}
static void inspector_content(cui_context *ui, AppState *s, const cui_theme *t) {
    cui_begin_box(ui, cui_id_from(ui, "inspector-content"),
        (cui_box){.width = -1, .padding = 10, .gap = 10});
    if(s->workspace!=APP_RECORD)director_card(ui, s, t);
    const char *headings[] = {"Virtual camera", "Recording", "Live stream"};
    const char *actions[] = {"Start virtual camera", "Start recording", "Start stream"};
    const char *icons[] = {"eye", "file", "network"};
    if(s->workspace==APP_RECORD)recording_output(ui,s,t);
    else {
    card_begin(ui, t, "output-card", headings[s->workspace]);
    badge(ui, t, output_state(s, s->workspace));
    text(ui, t, s->workspace == APP_CAMERA
        ? "One composition for your calls. Virtual camera output starts paused; revealing is a separate action."
        : s->workspace == APP_RECORD
        ? "Record your composition to a file. Cuts omit time; resuming continues the same recording."
        : "Send your composition to a streaming destination. Start paused, then reveal when you are ready.");
    ButtonIcon output_icon = {cui_id_from(ui, "output-start-icon"), t, icons[s->workspace], 14};
    cui_button_ex(ui, cui_id_from(ui, "output-start"), t, cui_string(actions[s->workspace]),
        (cui_button_options){.variant = CUI_BUTTON_PRIMARY, .width = -1, .height = 40,
            .font_size = 12, .leading_content = button_icon, .leading_user = &output_icon,
            .icon_size = 14, .disabled = true});
    text(ui, t, s->live.enabled ? "Output controls are read only in this milestone." : "Requires a connected capture daemon.");
    if (s->live.error[0]) text(ui, t, s->live.error);
    cui_end_box(ui);
    }
    audio_card(ui,s,t);
    card_begin(ui, t, "details-card", s->workspace == APP_CAMERA ? "Where it lands" :
        s->workspace == APP_RECORD ? "Take format" : "Destination & quality");
    if(s->workspace==APP_RECORD) {
        format_row(ui,t,"Container",s->live.connected?s->live.record_container:"Unavailable");
        format_row(ui,t,"Encoder",s->live.connected?s->live.record_codec:"Unavailable");
        char fps[32];snprintf(fps,sizeof fps,"%d fps",s->live.fps);format_row(ui,t,"Frame rate",s->live.connected?fps:"Unavailable");
        if(button(ui,t,"settings-recording","Recording settings","gear",-1,30,false,false)) {
            s->settings_category=2;app_open_settings(ui,s,cui_id_from(ui,"settings-recording"));
        }
    } else text(ui,t,s->workspace==APP_CAMERA?"Select cast as the camera in your calling app once the virtual device is running.":"A destination and stream-key file must be configured before broadcasting. Keys are never displayed here.");
    cui_end_box(ui);
    card_begin(ui, t, "pro-card", "Go further with Pro");
    badge(ui, t, s->live.connected ? (s->live.pro ? "PRO BUILD" : "COMMUNITY") : "PRO");
    text(ui, t, "Cinematic zoom, teleprompter and subtitles.");
    text(ui, t, "Recording, streaming and existing zoom remain free. Edition capabilities will come from the daemon.");
    cui_end_box(ui);
    cui_end_box(ui);
}
static void side_panel(cui_context *ui, AppState *s, const cui_theme *t, bool sources,
                       float width, float height) {
    cui_begin_box(ui, cui_id_from(ui, sources ? "sources-panel" : "inspector-panel"),
        (cui_box){.width = width, .height = -1, .background = sources ? t->surface : t->background,
            .clip = true, .custom_border_edges = true,
            .border_edges = {.left = sources ? 0 : 1, .right = sources ? 1 : 0}, .border = t->border});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 36, .row = true, .gap = 8,
        .padding_edges = {.left = 10, .right = 10}, .custom_padding = true,
        .border = t->border, .custom_border_edges = true, .border_edges = {.bottom = 1},
        .align_y = CUI_ALIGN_CENTER});
    title(ui, t, sources ? (s->workspace == APP_CAMERA ? "IN THE FRAME" :
        s->workspace == APP_RECORD ? "SOURCES & TAKE" : "DESTINATION & SOURCES") :
        (s->workspace == APP_CAMERA ? "OUTPUT" : s->workspace == APP_RECORD ? "RECORDING" : "BROADCAST"), 10);
    fill(ui);
    const char *toggle_id = sources ? "toggle-sources" : "toggle-inspector";
    if (icon_button(ui, t, toggle_id, sources ? "Hide left panel [" : "Hide right panel ]",
                    sources ? "panel-l" : "panel-r", false)) {
        if (sources) s->show_sources = false; else s->show_inspector = false;
        cui_invalidate(ui);
    }
    cui_end_box(ui);
    cui_scroll_options o = {.width = width - 2, .height = fmaxf(60, height - (sources ? 82 : 36)),
        .vertical = true, .auto_measure_content = true, .bar_thickness = 5, .wheel_step = 32};
    cui_id scroll_id = cui_id_from(ui, sources ? "sources-scroll" : "inspector-scroll");
    cui_scroll_state *scroll = sources ? &s->source_scroll : &s->inspector_scroll;
    if (cui_scrollable_begin(ui, scroll_id, t, scroll, o)) {
        if (sources) source_content(ui, s, t); else inspector_content(ui, s, t);
        cui_scrollable_end_ex(ui, scroll_id, t, scroll, o);
    }
    if (sources) {
        fill(ui);
        cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 46, .padding = 8});
        if (button(ui, t, "sources-settings", "Settings", "settings", -1, 30, false, false))
            app_open_settings(ui, s, cui_id_from(ui, "sources-settings"));
        cui_end_box(ui);
    }
    cui_end_box(ui);
}
static void empty_layer(cui_context *ui, const cui_theme *t, const char *id,
                        cui_rect bounds, bool camera, bool compact) {
    cui_begin_absolute_box(ui, cui_id_from(ui, id),
        (cui_box){.background = camera ? t->secondary : t->surface, .border = t->border,
            .border_width = 1, .radius = camera ? fminf(bounds.w, bounds.h) / 2 : 0,
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER, .gap = 10, .padding = 8,
            .clip = true}, bounds);
    app_icon(ui, cui_id_from(ui, camera ? "stage-camera-icon" : "stage-screen-icon"), t,
        cui_string(camera ? "circle-user" : "frame"),
        (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = compact ? 20 : 28});
    if (!compact) {
        title(ui, t, camera ? "Camera" : "No screen connected", camera ? 12 : 14);
        if (!camera) text(ui, t, "Choose a source when capture is connected.");
    }
    cui_end_box(ui);
}
static void stage(cui_context *ui, AppState *s, const cui_theme *t, float width, float height) {
    float padding = height < 140 ? 4 : 20;
    cui_begin_box(ui, cui_id_from(ui, "stage-area"),
        (cui_box){.width = -1, .height = -1, .padding = padding,
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER, .clip = true});
    float aspect = s->live.connected && s->live.width > 0 && s->live.height > 0
        ? (float)s->live.width / s->live.height : 16.f / 9;
    float sw = fmaxf(24, fminf(width - padding * 2, (height - padding * 2) * aspect));
    float sh = sw / aspect;
    cui_begin_box(ui, cui_id_from(ui, "composition-stage"),
        (cui_box){.width = sw, .height = sh, .background = {16, 17, 19, 255},
            .border = cast_design_tokens(t->dark).stage_line, .border_width = 1, .clip = true});
    if (s->live.enabled) {
        if (s->live.connected && s->live.has_frame && s->live.target == (int)s->workspace) {
            cui_paint_bitmap(ui, cui_id_from(ui, "live-preview"),
                (cui_bitmap_paint){.bitmap = s->live.frame, .opacity = 1},
                (cui_box){.width = sw, .height = sh});
        } else {
            cui_text(ui, cui_string(s->live.connected ? "Waiting for preview" : "Daemon not connected"), 13, t->muted);
        }
        cui_end_box(ui);
        cui_end_box(ui);
        return;
    }
    bool screen = s->layout != APP_WEBCAM, camera = s->layout != APP_SCREEN;
    cui_rect screen_rect = {12, 12, sw - 24, sh - 24};
    float camera_w = sw * s->camera_size / 100;
    cui_rect cam_rect = {sw - camera_w - 16, sh - camera_w - 16, camera_w, camera_w};
    if (s->layout == APP_SPLIT) {
        screen_rect = (cui_rect){12, 12, (sw - 36) * .58f, sh - 24};
        cam_rect = (cui_rect){screen_rect.x + screen_rect.w + 12, 12, (sw - 36) * .42f, sh - 24};
    } else if (s->layout == APP_WEBCAM) cam_rect = (cui_rect){12, 12, sw - 24, sh - 24};
    else if (s->layout == APP_STAGE) {
        float lane = fmaxf(camera_w + 24, sw * .25f);
        screen_rect = (cui_rect){12, 12, sw - lane - 24, sh - 24};
        cam_rect = (cui_rect){sw - lane + (lane - camera_w) / 2, (sh - camera_w) / 2, camera_w, camera_w};
    } else {
        unsigned col = s->camera_anchor % 3, row = s->camera_anchor / 3;
        cam_rect.x = 16 + (sw - camera_w - 32) * col / 2;
        cam_rect.y = 16 + (sh - camera_w - 32) * row / 2;
    }
    if (screen) empty_layer(ui, t, "stage-screen", screen_rect, false, screen_rect.w < 300 || sh < 170);
    if (camera) empty_layer(ui, t, "stage-camera", cam_rect, true, cam_rect.w < 160 || sh < 170);
    cui_end_box(ui);
    cui_end_box(ui);
}
typedef struct { ButtonIcon artwork; bool reverse; } RailIcon;
static void rail_icon(cui_context *ui, void *user) {
    const RailIcon *rail = user;
    app_icon(ui, rail->artwork.id, rail->artwork.theme, cui_string("chev"),
        (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = 12,
            .custom_color = true, .color = rail->artwork.theme->muted,
            .custom_transform = rail->reverse,
            .transform = {.a = -1, .d = -1}});
}
static void reopen_rail(cui_context *ui, AppState *s, const cui_theme *t,
                        bool sources, float width, float height) {
    const char *name = sources ? "toggle-sources" : "toggle-inspector";
    cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
    colors.normal = t->surface; colors.hover = cast_design_tokens(t->dark).hover;
    colors.active = colors.hover; colors.border = t->border;
    RailIcon artwork = {{cui_id_from(ui, name) ^ UINT64_C(0x7261696c), t, "chev", 12}, !sources};
    cui_begin_absolute_box(ui, cui_id_from(ui, sources ? "left-reopen-rail" : "right-reopen-rail"),
        (cui_box){0}, (cui_rect){sources ? 2 : width - 20, fmaxf(0, (height - 72) / 2), 18, 72});
    if (cui_button_ex(ui, cui_id_from(ui, name), t,
        cui_string(sources ? "Show left panel [" : "Show right panel ]"),
        (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = 18, .height = 72,
            .icon_only = true, .content = rail_icon, .content_user = &artwork,
            .custom_colors = true, .colors = colors})) {
        if (sources) s->show_sources = true; else s->show_inspector = true;
        cui_invalidate(ui);
    }
    cui_end_box(ui);
}
static void center(cui_context *ui, AppState *s, const cui_theme *t,
                   float width, float height, float window_width, bool sources_mounted, bool inspector_mounted) {
    bool short_window = height < 320;
    float heading_height = short_window ? 32 : 40;
    float notice_height = short_window ? 28 : 44;
    float meta_height = short_window ? 20 : 24;
    float bar_height = short_window ? 30 : 38;
    cui_begin_box(ui, cui_id_from(ui, "center"),
        (cui_box){.width = -1, .height = -1, .background = t->background, .clip = true});
    cui_begin_box(ui, cui_id_from(ui, "center-header"),
        (cui_box){.width = -1, .height = heading_height, .padding = 4, .row = true, .gap = 8,
            .align_y = CUI_ALIGN_CENTER, .background = t->surface, .clip = true});
    title(ui, t, s->workspace == APP_CAMERA ? "Composition" : s->workspace == APP_RECORD ? "Take" : "Broadcast", 13);
    if (width >= 560) text(ui, t, "One stage. Every output.");
    fill(ui);
    const char *left_toggle = window_width < 1000 ? "toggle-sources" : "center-toggle-sources";
    const char *right_toggle = window_width < 800 ? "toggle-inspector" : "center-toggle-inspector";
    if (icon_button(ui, t, left_toggle, "Sources panel [", "panel-left", sources_mounted))
        toggle_panel(ui, s, 1, window_width, cui_id_from(ui, left_toggle));
    if (icon_button(ui, t, right_toggle, "Output panel ]", "panel-right", inspector_mounted))
        toggle_panel(ui, s, 2, window_width, cui_id_from(ui, right_toggle));
    cui_end_box(ui);
    cui_begin_box(ui, cui_id_from(ui, "connection-notice"),
        (cui_box){.width = -1, .height = notice_height, .padding = 4, .row = true, .gap = 8,
            .align_y = CUI_ALIGN_CENTER, .clip = true});
    app_icon(ui, cui_id_from(ui, "offline-icon"), t, cui_string("info"),
        (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = 14});
    const char *notice = s->live.devices.loading || s->device_request_requested ? "Finding devices…" :
        s->edit_requested || s->live.pending || s->source_choice_requested ? "Applying changes…" :
        s->live.edit_error[0] ? s->live.edit_error :
        s->live.enabled ? (s->live.connected ? (s->live.error[0] ? s->live.error :
            "Private composition preview · outputs controlled separately") :
            (s->live.error[0] ? s->live.error : "Waiting for daemon · reconnecting automatically")) :
        (width > 460 ? "Capture is not connected to this preview." : "No preview connection");
    text(ui, t, notice);
    cui_end_box(ui);
    stage(ui, s, t, width, fmaxf(24, height - heading_height - notice_height - meta_height - bar_height));
    cui_begin_box(ui, cui_id_from(ui, "stage-meta"),
        (cui_box){.width = -1, .height = meta_height, .row = true, .padding = 2, .gap = 8,
            .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER});
    char dimensions[64];
    if (s->live.connected) snprintf(dimensions, sizeof dimensions, "%d × %d · %d fps", s->live.width, s->live.height, s->live.fps);
    else snprintf(dimensions, sizeof dimensions, "16:9");
    text(ui, t, dimensions); text(ui, t, app_layout_name(s->layout));
    text(ui, t, s->fit_cover ? "Cover" : "Contain");
    cui_end_box(ui);
    cui_begin_box(ui, cui_id_from(ui, "stagebar"),
        (cui_box){.width = -1, .height = bar_height, .row = true, .gap = 6, .padding = 2,
            .background = t->surface, .align_y = CUI_ALIGN_CENTER, .clip = true});
    const char *target_icons[] = {"cam", "rec", "stream"};
    cui_begin_box(ui, 0, (cui_box){.row = true, .gap = 6, .align_y = CUI_ALIGN_CENTER,
        .padding = 5, .border_width = 1, .border = t->border, .background = t->secondary});
    app_icon(ui, cui_id_from(ui, "stagebar-target-icon"), t, cui_string(target_icons[s->workspace]),
        (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = 12});
    cui_text(ui, cui_string(app_workspace_name(s->workspace)), 11, cast_design_tokens(t->dark).secondary);
    cui_end_box(ui);
    if (width >= 680) cui_text(ui, cui_string("Privacy"), 10.5f, t->muted);
    fill(ui);
    if (width >= 540) {
        cui_begin_box(ui, 0, (cui_box){.width = 286, .fixed_width = true});
        layout_picker_ex(ui, s, t, true);
        cui_end_box(ui);
    } else cui_text(ui, cui_string(s->live.connected ? "Ready" : "Not connected"), 10.5f, t->muted);
    if(button(ui,t,"stage-zoom","Zoom","zoom",76,30,s->zoom_popup.open,composition_disabled(s))) {
        if(!s->zoom_popup_initialized) {cui_overlay_init(&s->zoom_popup,cui_id_from(ui,"zoom-pop"),false);s->zoom_popup_initialized=true;}
        if(s->zoom_popup.open)cui_overlay_close(ui,&s->zoom_popup);
        else cui_overlay_open(ui,&s->zoom_popup,cui_id_from(ui,"stage-zoom"));
        cui_invalidate(ui);
    }
    cui_end_box(ui);
    if (window_width >= 1000 && !sources_mounted) reopen_rail(ui, s, t, true, width, height);
    if (window_width >= 800 && !inspector_mounted) reopen_rail(ui, s, t, false, width, height);
    cui_end_box(ui);
}
static void status_dot(cui_context *ui, cui_color color, float size) {
    cui_begin_box(ui, 0, (cui_box){.width = size, .height = size,
        .fixed_width = true, .fixed_height = true, .background = color, .radius = size / 2});
    cui_end_box(ui);
}
static void status_separator(cui_context *ui, const cui_theme *t) {
    cui_begin_box(ui, 0, (cui_box){.width = 1, .height = 12,
        .fixed_width = true, .fixed_height = true, .background = t->border});
    cui_end_box(ui);
}
typedef struct { const cui_theme *theme; const char *label, *state; cui_color dot; } StatusLane;
static void status_lane_body(cui_context *ui, void *user) {
    const StatusLane *lane = user;
    cui_begin_box(ui, 0, (cui_box){.row = true, .gap = 5, .align_y = CUI_ALIGN_CENTER});
    status_dot(ui, lane->dot, 6);
    cui_text(ui, cui_string(lane->label), 10.5f, cast_design_tokens(lane->theme->dark).secondary);
    cui_text_ex(ui, cui_string(lane->state), 10.5f, lane->theme->text, 0,
                (cui_text_style){.weight = CUI_TEXT_WEIGHT_SEMIBOLD});
    cui_end_box(ui);
}
static void status(cui_context *ui, AppState *s, const cui_theme *t, float width) {
    cui_begin_box(ui, cui_id_from(ui, "statusbar"),
        (cui_box){.width = -1, .height = 28, .row = true, .gap = 10,
            .padding_edges = {.left = 10, .right = 10}, .custom_padding = true,
            .border = t->border, .custom_border_edges = true, .border_edges = {.top = 1},
            .background = t->surface, .align_y = CUI_ALIGN_CENTER, .clip = true});
    status_dot(ui, s->live.connected ? cast_design_tokens(t->dark).live : t->muted, 6);
    cui_text(ui, cui_string(s->live.connected ? "Daemon connected" :
        s->live.enabled ? "Daemon disconnected · retrying" : "Daemon not connected"), 10.5f, t->muted);
    /* No MCP state is supplied by this read-only bridge; retain the reference
     * chip's position without claiming an active server. */
    if (width >= 600) {
        cui_begin_box(ui, cui_id_from(ui, "status-mcp"), (cui_box){.height = 18, .row = true, .gap = 5,
            .padding_edges = {.left = 6, .right = 6}, .custom_padding = true,
            .border_width = 1, .border = t->border, .align_y = CUI_ALIGN_CENTER});
        status_dot(ui, t->muted, 5);
        cui_text(ui, cui_string("MCP —"), 10.5f, cast_design_tokens(t->dark).secondary);
        cui_end_box(ui);
        status_separator(ui, t);
    }
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 5,
        .align_y = CUI_ALIGN_CENTER, .clip = true});
    if (width >= 1100) cui_text(ui, cui_string(s->live.error[0] ? s->live.error :
        s->edit_requested||s->live.pending ? "Applying changes…" : s->live.enabled ? (s->live.connected?"Ready":"Disconnected") : "Local preview"), 10.5f,
        s->live.error[0] ? t->danger : t->muted);
    cui_end_box(ui);
    if (width >= 680) {
        const char *ids[] = {"status-camera", "status-record", "status-stream"};
        const char *labels[] = {"cam", "rec", "stream"};
        for (unsigned i = 0; i < 3; ++i) {
            bool active = s->live.connected && (i == 0 ? s->live.virtual_enabled :
                i == 1 ? s->live.recording : s->live.stream_active);
            bool paused = i == 0 ? s->live.virtual_paused : i == 1 ? s->live.record_paused : s->live.stream_paused;
            const char *state = !s->live.connected ? "—" : !active ? "off" : paused ? "paused" : "live";
            if(i==1&&s->live.connected)state=s->live.record_finalizing?"finalising":s->live.record_countdown_active?"countdown":s->live.record_cut?"cut":state;
            bool writing=active&&!paused&&!(i==1&&(s->live.record_cut||s->live.record_countdown_active||s->live.record_finalizing));
            StatusLane lane = {t, labels[i], state, writing ? cast_design_tokens(t->dark).live : t->muted};
            cui_theme_button_colors colors = t->buttons[CUI_THEME_BUTTON_SECONDARY];
            colors.normal = colors.hover = colors.active = colors.border = (cui_color){0};
            float lane_width = 6 + 10 + cui_measure_text(ui, cui_string(labels[i]), 10.5f).width +
                cui_measure_text(ui, cui_string(state), 10.5f).width;
            if (cui_button_ex(ui, cui_id_from(ui, ids[i]), t, cui_string(labels[i]),
                (cui_button_options){.variant = CUI_BUTTON_SECONDARY, .width = lane_width,
                    .height = 20, .icon_only = true, .content = status_lane_body, .content_user = &lane,
                    .custom_colors = true, .colors = colors})) {
                s->workspace = (AppWorkspace)i;
                s->workspace_tab = cui_id_from(ui, workspace_ids[i]);
                cui_invalidate(ui);
            }
        }
    }
    if (width >= 1150) {
        status_separator(ui, t);
        app_icon(ui, cui_id_from(ui, "status-project-icon"), t, cui_string("save"),
            (cui_icon_options){.size = CUI_ICON_CUSTOM, .custom_size = 11,
                .custom_color = true, .color = t->muted});
        cui_text(ui, cui_string("Untitled composition"), 10.5f, cast_design_tokens(t->dark).secondary);
        cui_text(ui, cui_string("unsaved"), 10.5f, t->muted);
    }
    cui_end_box(ui);
}
static void drawer(cui_context *ui, AppState *s, const cui_theme *t, float width, float height) {
    if (!s->panel_sheet_initialized || !s->panel_sheet.overlay.open) return;
    cui_surface_options o = {.viewport = {0, 0, width, height}, .edge = s->drawer_kind == 1 ? CUI_SHEET_LEFT : CUI_SHEET_RIGHT,
        .width = fminf(320, width - 48), .sheet_minimum = 260,
        .sheet_maximum = fminf(320, width - 48)};
    /* CUI's sheet size is retained on the surface, not an options field. */
    s->panel_sheet.sheet_size = fminf(320, width - 48);
    o.no_sheet_resize = true;
    if (cui_surface_begin(ui, t, &s->panel_sheet,
        cui_string(s->drawer_kind == 1 ? "Sources" : "Output"), cui_string("Workspace panel"), o)) {
        if (s->drawer_kind == 1) source_content(ui, s, t); else inspector_content(ui, s, t);
        cui_surface_end(ui);
    }
}
void app_frame(cui_context *ui, AppState *s, float width, float height) {
    if(s->zoom_popup_initialized) {
        if(s->settings.overlay.open || (s->live.enabled&&!s->live.connected))cui_overlay_close(ui,&s->zoom_popup);
        else cui_overlay_update(ui,&s->zoom_popup);
    }
    s->source_picker.viewport = (cui_rect){0, 0, width, height};
    if (s->source_picker.popup.overlay.open && (!s->live.connected ||
        s->source_picker.generation != s->live.generation || s->settings.overlay.open))
        app_source_picker_close(ui, s);
    /* A drawer becomes its persistent panel at the desktop breakpoint. */
    if (s->panel_sheet_initialized && s->panel_sheet.overlay.open &&
        width >= (s->drawer_kind == 1 ? 1000 : 800))
        cui_surface_close(ui, &s->panel_sheet, CUI_SURFACE_CLOSED);
    sync_live(s);
    shortcuts(ui, s, width);
    cui_theme t = cast_theme(app_is_dark(s));
    float chrome_height = width < 800 ? 106 : 68;
    float work_height = fmaxf(100, height - chrome_height - 29);
    bool left = width >= 1000 && s->show_sources;
    bool right = width >= 800 && s->show_inspector;
    float center_width = width - (left ? 232 : 0) - (right ? 300 : 0);
    cui_begin_box(ui, cui_id_from(ui, "root"),
        (cui_box){.width = -1, .height = -1, .background = t.background, .clip = true});
    chrome(ui, s, &t, width);
    cui_begin_box(ui, cui_id_from(ui, "workspace"),
        (cui_box){.width = -1, .height = -1, .row = true, .clip = true});
    if (left) side_panel(ui, s, &t, true, 232, work_height);
    center(ui, s, &t, center_width, work_height, width, left, right);
    if (right) side_panel(ui, s, &t, false, 300, work_height);
    cui_end_box(ui);
    status(ui, s, &t, width);
    cui_end_box(ui);
    drawer(ui, s, &t, width, height);
    if (!left && !(s->panel_sheet.overlay.open && s->drawer_kind == 1))
        app_source_picker_close(ui, s);
    zoom_popup(ui,s,&t,width,height);
    app_settings_frame(ui, s, &t, width, height);
}

const char *app_setting(const AppState *a,const char *section,const char *key) {
    for(unsigned i=0;i<a->live.setting_count && i<APP_SETTING_MAX;++i)
        if(!strcmp(a->live.settings[i].section,section)&&!strcmp(a->live.settings[i].key,key))return a->live.settings[i].value;
    return "";
}
static bool extension_request(AppState *a,bool command,unsigned argc,const char *const *argv) {
    if(!a || !a->live.connected || a->live.pending || a->live.snapshot_busy || a->edit_requested || a->extension_requested || !argc || argc>4)return false;
    for(unsigned i=0;i<argc;++i)if(!argv[i] || strlen(argv[i])>=sizeof a->extension_args[i])return false;
    for(unsigned i=0;i<argc;++i)snprintf(a->extension_args[i],sizeof a->extension_args[i],"%s",argv[i]);
    a->extension_argc=argc;a->extension_command=command;a->extension_generation=a->live.generation;a->extension_requested=true;return true;
}
bool app_request_setting(AppState *a,const char *key,const char *value) {
    const char *args[]={key,value};return extension_request(a,false,2,args);
}
bool app_request_command(AppState *a,unsigned argc,const char *const *argv) {
    return extension_request(a,true,argc,argv);
}
