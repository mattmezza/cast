#include "panel.h"
#include "panel_transport.h"
#define CLAY_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#include "clay.h"
#pragma GCC diagnostic pop
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#ifdef WITH_X11
#define Cursor X11Cursor
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Cursor
#endif
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small immediate widgets share Clay's layout, clipping, and pointer geometry.
 * The worker transports bounded requests; no socket operation runs on this thread. */
enum {
    TAB_SOURCE,
    TAB_CAMERA,
    TAB_AUDIO,
    TAB_EFFECTS,
    TAB_SETTINGS
};
typedef enum {
    FIELD_BOOL,
    FIELD_INT,
    FIELD_DOUBLE,
    FIELD_TEXT,
    FIELD_ENUM,
    FIELD_COLOR
} FieldType;
enum {
    REQUIRE_CURSOR = 1,
    REQUIRE_INPUT = 2,
    REQUIRE_REGION = 4,
    RECORD_LOCK = 8,
    READ_ONLY = 16
};
typedef struct {
    const char *key, *label, *choices;
    size_t offset, length;
    FieldType type;
    double minimum, maximum;
    int tab, group, flags;
} FieldSpec;
#define FD(key, label, field, kind, min, max, choices, tab, group, flags)                          \
    {key, label, choices, offsetof(Config, field), sizeof(((Config *)0)->field), kind, min, max,   \
     tab, group, flags}
#define FB(key, label, field, tab, group, flags)                                                   \
    FD(key, label, field, FIELD_BOOL, 0, 0, NULL, tab, group, flags)
#define FI(key, label, field, min, max, tab, group, flags)                                         \
    FD(key, label, field, FIELD_INT, min, max, NULL, tab, group, flags)
#define FN(key, label, field, min, max, tab, group, flags)                                         \
    FD(key, label, field, FIELD_DOUBLE, min, max, NULL, tab, group, flags)
#define FT(key, label, field, tab, group, flags)                                                   \
    FD(key, label, field, FIELD_TEXT, 0, 0, NULL, tab, group, flags)
#define FE(key, label, field, choices, tab, group, flags)                                          \
    FD(key, label, field, FIELD_ENUM, 0, 0, choices, tab, group, flags)
#define FC(key, label, field, tab, group, flags)                                                   \
    FD(key, label, field, FIELD_COLOR, 0, 0, NULL, tab, group, flags)
static const FieldSpec fields[] = {
    FT("capture.monitor", "Monitor name", monitor, TAB_SOURCE, 0, 0),
    FE("composition.fit", "Screen fit", fit, "contain,cover", TAB_SOURCE, 0, 0),
    FN("zoom.factor", "Zoom factor", zoom_factor, 1, 20, TAB_SOURCE, 0, 0),
    FB("zoom.follow", "Follow pointer", zoom_follow, TAB_SOURCE, 0, REQUIRE_CURSOR),
    FI("capture.x", "Region X", region_x, 0, 16384, TAB_SOURCE, 1, REQUIRE_REGION),
    FI("capture.y", "Region Y", region_y, 0, 16384, TAB_SOURCE, 1, REQUIRE_REGION),
    FI("capture.width", "Region width", region_w, 0, 16384, TAB_SOURCE, 1, REQUIRE_REGION),
    FI("capture.height", "Region height", region_h, 0, 16384, TAB_SOURCE, 1, REQUIRE_REGION),
    FN("zoom.min", "Minimum zoom", zoom_min, 1, 20, TAB_SOURCE, 2, 0),
    FN("zoom.max", "Maximum zoom", zoom_max, 1, 20, TAB_SOURCE, 2, 0),
    FN("zoom.step", "Zoom step", zoom_step, .01, 10, TAB_SOURCE, 2, 0),
    FI("zoom.transition_ms", "Zoom transition (ms)", zoom_transition_ms, 0, 3000, TAB_SOURCE, 2, 0),
    FI("zoom.deadzone", "Pointer deadzone (px)", zoom_deadzone, 0, 2000, TAB_SOURCE, 2,
       REQUIRE_CURSOR),
    FN("zoom.smoothing", "Pointer smoothing", zoom_smoothing, .001, 1, TAB_SOURCE, 2,
       REQUIRE_CURSOR),
    FB("camera.enabled", "Camera enabled", camera_enabled, TAB_CAMERA, 0, 0),
    FB("camera.visible", "Visible in composition", camera_visible, TAB_CAMERA, 0, 0),
    FT("camera.device", "Camera device", camera_device, TAB_CAMERA, 3, 0),
    FN("camera.width_percent", "Camera width (%)", camera_width_percent, 1, 100, TAB_CAMERA, 0, 0),
    FE("camera.shape", "Shape", shape, "rectangle,rounded,circle", TAB_CAMERA, 0, 0),
    FE("camera.anchor", "Anchor", anchor, "top-left,top-right,bottom-left,bottom-right,free",
       TAB_CAMERA, 0, 0),
    FE("camera.aspect", "Aspect ratio", aspect, "native,16:9,4:3,1:1", TAB_CAMERA, 0, 0),
    FB("camera.mirror", "Mirror", mirror, TAB_CAMERA, 0, 0),
    FE("composition.split_side", "Split camera side", split_side, "left,right", TAB_CAMERA, 1, 0),
    FN("composition.split_ratio", "Split camera width (%)", split_ratio, 5, 95, TAB_CAMERA, 1, 0),
    FI("camera.x", "Free position X", camera_x, -7680, 7680, TAB_CAMERA, 1, 0),
    FI("camera.y", "Free position Y", camera_y, -4320, 4320, TAB_CAMERA, 1, 0),
    FI("camera.crop_x", "Crop offset X", crop_x, -16384, 16384, TAB_CAMERA, 1, 0),
    FI("camera.crop_y", "Crop offset Y", crop_y, -16384, 16384, TAB_CAMERA, 1, 0),
    FI("camera.margin", "Margin (px)", margin, 0, 4096, TAB_CAMERA, 2, 0),
    FI("camera.radius", "Corner radius (px)", radius, 0, 2048, TAB_CAMERA, 2, 0),
    FI("camera.border_width", "Border width (px)", border_width, 0, 256, TAB_CAMERA, 2, 0),
    FC("camera.border_color", "Border color", border_color, TAB_CAMERA, 2, 0),
    FT("camera.corner_order", "Corner cycle order", corner_order, TAB_CAMERA, 2, 0),
    FB("audio.mic", "Microphone", mic, TAB_AUDIO, 0, 0),
    FT("audio.mic_source", "Microphone source", mic_source, TAB_AUDIO, 0, 0),
    FN("audio.mic_gain", "Microphone gain (0–4)", mic_gain, 0, 4, TAB_AUDIO, 0, 0),
    FB("audio.desktop", "Desktop audio", desktop, TAB_AUDIO, 0, 0),
    FT("audio.desktop_source", "Desktop source", desktop_source, TAB_AUDIO, 0, 0),
    FN("audio.desktop_gain", "Desktop gain (0–4)", desktop_gain, 0, 4, TAB_AUDIO, 0, 0),
    FB("audio.virtual", "Virtual microphone", virtual_audio, TAB_AUDIO, 1, 0),
    FT("audio.virtual_name", "Virtual microphone name", virtual_name, TAB_AUDIO, 1, READ_ONLY),
    FB("cursor.enabled", "Cursor", cursor, TAB_EFFECTS, 0, REQUIRE_CURSOR),
    FB("cursor.highlight", "Cursor highlight", cursor_highlight, TAB_EFFECTS, 0, REQUIRE_CURSOR),
    FB("clicks.enabled", "Click rings", clicks, TAB_EFFECTS, 0, REQUIRE_INPUT),
    FB("keys.enabled", "Keystrokes", keys, TAB_EFFECTS, 0, REQUIRE_INPUT),
    FE("keys.mode", "Keystroke mode", keys_mode, "shortcuts,all", TAB_EFFECTS, 0, REQUIRE_INPUT),
    FB("annotations.live_keys", "Keystrokes in live output", annotations_live_keys, TAB_EFFECTS, 1,
       REQUIRE_INPUT),
    FB("annotations.live_clicks", "Clicks in live output", annotations_live_clicks, TAB_EFFECTS, 1,
       REQUIRE_INPUT),
    FB("annotations.record_keys", "Keystrokes in recording", annotations_record_keys, TAB_EFFECTS,
       1, REQUIRE_INPUT),
    FB("annotations.record_clicks", "Clicks in recording", annotations_record_clicks, TAB_EFFECTS,
       1, REQUIRE_INPUT),
    FI("cursor.size", "Cursor size (px)", cursor_size, 1, 256, TAB_EFFECTS, 2, REQUIRE_CURSOR),
    FC("cursor.color", "Cursor color", cursor_color, TAB_EFFECTS, 2, REQUIRE_CURSOR),
    FI("clicks.duration_ms", "Click duration (ms)", click_duration_ms, 50, 10000, TAB_EFFECTS, 2,
       REQUIRE_INPUT),
    FI("clicks.radius", "Click radius (px)", click_radius, 1, 512, TAB_EFFECTS, 2, REQUIRE_INPUT),
    FB("clicks.middle", "Observe middle clicks", click_middle, TAB_EFFECTS, 2, REQUIRE_INPUT),
    FC("clicks.left_color", "Left click color", click_left_color, TAB_EFFECTS, 2, REQUIRE_INPUT),
    FC("clicks.right_color", "Right click color", click_right_color, TAB_EFFECTS, 2, REQUIRE_INPUT),
    FC("clicks.middle_color", "Middle click color", click_middle_color, TAB_EFFECTS, 2,
       REQUIRE_INPUT),
    FE("keys.position", "Keystroke position", keys_position,
       "top-left,top-right,bottom-left,bottom-right", TAB_EFFECTS, 3, REQUIRE_INPUT),
    FI("keys.font_size", "Keystroke font size", keys_font_size, 8, 96, TAB_EFFECTS, 3,
       REQUIRE_INPUT),
    FI("keys.timeout_ms", "Keystroke timeout (ms)", keys_timeout_ms, 100, 30000, TAB_EFFECTS, 3,
       REQUIRE_INPUT),
    FC("keys.color", "Keystroke color", keys_color, TAB_EFFECTS, 3, REQUIRE_INPUT),
    FC("keys.background", "Keystroke background", keys_background, TAB_EFFECTS, 3, REQUIRE_INPUT),
    FT("keys.filter", "Hidden shortcuts", keys_filter, TAB_EFFECTS, 3, REQUIRE_INPUT),
    FT("keys.navigation", "Navigation keys", keys_navigation, TAB_EFFECTS, 3, REQUIRE_INPUT),
    FT("output.pause_text", "Pause message", pause_text, TAB_SETTINGS, 0, 0),
    FC("output.pause_color", "Pause background", pause_color, TAB_SETTINGS, 0, 0),
    FT("record.directory", "Recording directory", record_dir, TAB_SETTINGS, 0, RECORD_LOCK),
    FI("record.countdown", "Recording countdown (s)", record_countdown, 0, 60, TAB_SETTINGS, 0,
       RECORD_LOCK),
    FT("record.container", "Container", record_container, TAB_SETTINGS, 1, RECORD_LOCK),
    FT("record.video_codec", "Video codec", video_codec, TAB_SETTINGS, 1, RECORD_LOCK),
    FT("record.audio_codec", "Audio codec", audio_codec, TAB_SETTINGS, 1, RECORD_LOCK),
    FI("record.crf", "Video quality (CRF)", record_crf, 0, 51, TAB_SETTINGS, 1, RECORD_LOCK),
    FT("record.preset", "Encoder preset", record_preset, TAB_SETTINGS, 1, RECORD_LOCK),
    FI("record.queue", "Recording queue (frames)", record_queue, 1, 120, TAB_SETTINGS, 1,
       RECORD_LOCK),
    FT("composition.layout_order", "Layout cycle order", layout_order, TAB_SETTINGS, 2, 0),
    FT("composition.preset_order", "Preset cycle order", preset_order, TAB_SETTINGS, 2, 0),
    FT("output.backend", "Capture backend", backend, TAB_SETTINGS, 3, READ_ONLY),
    FT("output.device", "Virtual camera device", output_device, TAB_SETTINGS, 3, READ_ONLY),
    FB("output.enabled", "Live output at start", live_enabled, TAB_SETTINGS, 3, READ_ONLY),
    FI("output.width", "Output width", width, 64, 7680, TAB_SETTINGS, 3, READ_ONLY),
    FI("output.height", "Output height", height, 64, 4320, TAB_SETTINGS, 3, READ_ONLY),
    FI("output.fps", "Frames per second", fps, 1, 120, TAB_SETTINGS, 3, READ_ONLY),
    FT("ipc.socket", "Daemon socket", socket_path, TAB_SETTINGS, 3, READ_ONLY),
    FI("ipc.timeout_ms", "CLI timeout (ms)", ipc_timeout_ms, 100, 30000, TAB_SETTINGS, 3,
       READ_ONLY),
};
#define FIELD_COUNT (sizeof fields / sizeof fields[0])
#define WIDGET_MAX 384
#define TEXT_CACHE_MAX 384
typedef struct {
    char value[PATH_MAX];
    bool dirty;
    uint64_t pending, revision, submitted_revision;
} FieldEdit;
typedef enum {
    W_BUTTON,
    W_FIELD,
    W_SELECT,
    W_OPTION
} WidgetType;
typedef enum {
    A_NONE,
    A_COMMAND,
    A_SETTING,
    A_FIELD,
    A_APPLY,
    A_TAB,
    A_GROUP,
    A_DROPDOWN,
    A_OPTION,
    A_PREVIEW,
    A_DISMISS
} Action;
typedef struct {
    uint32_t id;
    WidgetType type;
    Action action;
    bool enabled;
    int index, auxiliary;
    const char *arg[6];
    int argc;
    char value[PATH_MAX];
    Clay_BoundingBox box;
} Widget;
typedef struct {
    SDL_Texture *texture;
    char *text;
    int size, width, height;
    uint64_t used;
} TextCache;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font[4], *raster_font[4];
    float density, input_scale;
    PanelClient *client;
    PanelSnapshot snapshot;
    FieldEdit edit[FIELD_COUNT];
    Widget widgets[WIDGET_MAX];
    int widget_count, tab;
    bool groups[5][4], record_preview, quit, mouse_down, click;
    uint32_t focus, active_text, dropdown;
    int dropdown_field, dropdown_choice;
    bool select_all;
    size_t caret;
    float mouse_x, mouse_y, scroll;
    float width, height;
    bool preview_moved, preview_dragging;
    float preview_x, preview_y, drag_x, drag_y;
    char error[CAST_ERR], reply[CAST_ERR];
    uint64_t command_seen, draw_frame;
    uint64_t frame_generation, frame_epoch;
    Frame frame;
    SDL_Texture *preview;
    TextCache cache[TEXT_CACHE_MAX];
    char strings[49152];
    size_t string_used;
} Panel;
static const Clay_Color background = {16, 17, 19, 255};
static const Clay_Color surface = {24, 26, 29, 255};
static const Clay_Color control = {39, 42, 46, 255};
static const Clay_Color hovered = {52, 56, 62, 255};
static const Clay_Color foreground = {248, 249, 251, 255};
static const Clay_Color secondary = {187, 193, 202, 255};
static const Clay_Color muted = {151, 158, 168, 255};
static const Clay_Color accent = {128, 201, 255, 255};
static const Clay_Color danger = {255, 150, 147, 255};
static const Clay_Color line = {68, 73, 81, 255};
static const int font_sizes[] = {13, 16, 19, 25};

static Clay_String literal(const char *s)
{
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}
static const char *format(Panel *p, const char *fmt, ...)
{
    if (p->string_used + 512 >= sizeof p->strings) {
        return "";
    }
    char *s = p->strings + p->string_used;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof p->strings - p->string_used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof p->strings - p->string_used) {
        return "";
    }
    p->string_used += (size_t)n + 1;
    return s;
}
static Clay_ElementId element_id(uint32_t id)
{
    return Clay_GetElementIdWithIndex(CLAY_STRING("widget"), id);
}
static void label(const char *text, int font, Clay_Color color)
{
    CLAY_TEXT(literal(text), CLAY_TEXT_CONFIG({.fontId = (uint16_t)font,
                                               .fontSize = (uint16_t)font_sizes[font],
                                               .textColor = color,
                                               .wrapMode = CLAY_TEXT_WRAP_NONE}));
}
static Widget *widget(Panel *p, uint32_t id, WidgetType type, Action action, bool enabled)
{
    if (p->widget_count == WIDGET_MAX) {
        return NULL;
    }
    Widget *w = &p->widgets[p->widget_count++];
    memset(w, 0, sizeof *w);
    w->id = id;
    w->type = type;
    w->action = action;
    w->enabled = enabled;
    return w;
}
static bool hot(uint32_t id)
{
    return Clay_PointerOver(element_id(id));
}
static void button(Panel *p, uint32_t id, const char *text, bool enabled, bool selected,
                   Action action, int index)
{
    if (action != A_TAB && action != A_GROUP && action != A_PREVIEW && action != A_DROPDOWN &&
        p->snapshot.command_queued > p->snapshot.command_completed) {
        enabled = false;
    }
    Widget *w = widget(p, id, W_BUTTON, action, enabled);
    if (w) {
        w->index = index;
    }
    Clay_Color bg = selected             ? (Clay_Color){35, 63, 83, 255}
                    : hot(id) && enabled ? hovered
                                         : control;
    Clay_Color ink = enabled ? selected ? accent : foreground : muted;
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.height = CLAY_SIZING_FIXED(40)},
                     .padding = {12, action == A_DROPDOWN ? 26 : 12, 8, 8},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(6),
          .border = {.color = p->focus == id ? accent
                              : selected     ? (Clay_Color){60, 104, 134, 255}
                                             : bg,
                     .width = CLAY_BORDER_ALL(1)}})
    {
        label(text, 1, ink);
    }
}
static void command_button(Panel *p, uint32_t id, const char *text, bool enabled, const char *a,
                           const char *b, const char *c)
{
    button(p, id, text, enabled, false, A_COMMAND, 0);
    Widget *w = &p->widgets[p->widget_count - 1];
    w->arg[0] = a;
    w->argc = 1;
    if (b) {
        w->arg[w->argc++] = b;
    }
    if (c) {
        w->arg[w->argc++] = c;
    }
}
static void field_value(const FieldSpec *f, const Config *c, char *out, size_t n)
{
    const void *v = (const char *)c + f->offset;
    switch (f->type) {
    case FIELD_BOOL:
        snprintf(out, n, "%s", *(const bool *)v ? "true" : "false");
        break;
    case FIELD_INT:
        snprintf(out, n, "%d", *(const int *)v);
        break;
    case FIELD_DOUBLE:
        snprintf(out, n, "%.6g", *(const double *)v);
        break;
    case FIELD_COLOR:
        snprintf(out, n, "#%06x", *(const uint32_t *)v);
        break;
    default:
        snprintf(out, n, "%s", (const char *)v);
        break;
    }
}
static bool supported(const Panel *p, const FieldSpec *f)
{
    return (!(f->flags & REQUIRE_CURSOR) || p->snapshot.capabilities.cursor_metadata) &&
           (!(f->flags & REQUIRE_INPUT) || p->snapshot.capabilities.input) &&
           (!(f->flags & REQUIRE_REGION) || p->snapshot.capabilities.region_selection);
}
static bool writable(const Panel *p, const FieldSpec *f)
{
    return p->snapshot.connected && !(f->flags & READ_ONLY) &&
           (!(f->flags & RECORD_LOCK) ||
            (!p->snapshot.state.recording && !p->snapshot.finalizing && !p->snapshot.countdown));
}
static int choice_count(const char *s)
{
    int count = s && *s ? 1 : 0;
    while (s && *s) {
        if (*s++ == ',') {
            count++;
        }
    }
    return count;
}
static void choice_value(const char *choices, int index, char *out, size_t n)
{
    const char *s = choices;
    for (int i = 0; s && i < index; i++) {
        s = strchr(s, ',');
        if (s) {
            s++;
        }
    }
    if (!s) {
        out[0] = 0;
        return;
    }
    const char *end = strchr(s, ',');
    snprintf(out, n, "%.*s", (int)(end ? end - s : (ptrdiff_t)strlen(s)), s);
}
static int validate_field(const FieldSpec *f, const char *value, char *out, size_t n)
{
    if (f->type == FIELD_TEXT || f->type == FIELD_ENUM) {
        if (strlen(value) >= f->length) {
            snprintf(out, n, "%s accepts at most %zu bytes", f->label, f->length - 1);
            return -1;
        }
    } else if (f->type == FIELD_COLOR) {
        char *end;
        errno = 0;
        strtoul(value + (value[0] == '#'), &end, 16);
        if (strlen(value) != 7 || value[0] != '#' || errno || *end) {
            snprintf(out, n, "%s expects #RRGGBB", f->label);
            return -1;
        }
    } else if (f->type == FIELD_INT || f->type == FIELD_DOUBLE) {
        char *end;
        errno = 0;
        double v = strtod(value, &end);
        if (errno || end == value || *end || !isfinite(v) || v < f->minimum || v > f->maximum ||
            (f->type == FIELD_INT && floor(v) != v)) {
            snprintf(out, n, "%s expects %s %g–%g", f->label,
                     f->type == FIELD_INT ? "an integer" : "a number", f->minimum, f->maximum);
            return -1;
        }
    }
    return 0;
}
static void field_row(Panel *p, size_t index)
{
    const FieldSpec *f = &fields[index];
    if (!supported(p, f)) {
        return;
    }
    bool enabled = writable(p, f);
    uint32_t id = 1000 + (uint32_t)index * 3;
    char current[PATH_MAX];
    field_value(f, &p->snapshot.config, current, sizeof current);
    if (!p->edit[index].dirty && p->active_text != id) {
        snprintf(p->edit[index].value, sizeof p->edit[index].value, "%s", current);
    }
    bool stacked = f->type != FIELD_BOOL && f->type != FIELD_ENUM;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIT()},
                     .padding = {0, 0, 8, 10},
                     .childGap = 8,
                     .layoutDirection = stacked ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .border = {.color = line, .width = {.bottom = 1}}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
        {
            label(f->label, 1, foreground);
        }
        CLAY({.layout = {.sizing = {.width = stacked ? CLAY_SIZING_GROW() : CLAY_SIZING_FIT()},
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            if (f->type == FIELD_BOOL) {
                bool on = !strcmp(current, "true");
                button(p, id, on ? "On" : "Off", enabled, on, A_SETTING, (int)index);
                Widget *w = &p->widgets[p->widget_count - 1];
                snprintf(w->value, sizeof w->value, "%s", on ? "false" : "true");
            } else if (f->type == FIELD_ENUM) {
                button(p, id, format(p, "%s", current), enabled, false, A_DROPDOWN, (int)index);
                p->widgets[p->widget_count - 1].type = W_SELECT;
            } else {
                Widget *w = widget(p, id, W_FIELD, A_FIELD, enabled);
                if (w) {
                    w->index = (int)index;
                }
                CLAY({.id = element_id(id),
                      .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                            .height = CLAY_SIZING_FIXED(40)}},
                      .backgroundColor = p->active_text == id ? surface : background,
                      .cornerRadius = CLAY_CORNER_RADIUS(4),
                      .border = {.color = p->focus == id ? accent : line,
                                 .width = CLAY_BORDER_ALL(1)},
                      .custom = {.customData = (void *)(uintptr_t)(index + 1)}})
                {
                }
                if (f->flags & READ_ONLY) {
                    label("Restart", 0, secondary);
                } else if (p->edit[index].dirty) {
                    button(p, id + 1, "Apply", enabled, false, A_APPLY, (int)index);
                }
            }
        }
        if (stacked && (f->flags & RECORD_LOCK) && !enabled && p->snapshot.connected) {
            label("Stop recording to edit", 0, secondary);
        }
    }
}

static void text_wrapped(const char *text, Clay_Color color)
{
    CLAY_TEXT(
        literal(text),
        CLAY_TEXT_CONFIG(
            {.fontId = 0, .fontSize = 13, .textColor = color, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
}
static const char *const section_names[] = {"Source", "Camera", "Audio", "Effects", "Settings"};

static void status_line(Panel *p)
{
    const PanelSnapshot *s = &p->snapshot;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(42)},
                     .childGap = 12,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
    {
        if (p->tab >= 0) {
            button(p, 99, "Back", true, false, A_TAB, -1);
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
        {
            label(p->tab < 0 ? "cast" : section_names[p->tab], 3, foreground);
        }
        if (s->state.recording || s->countdown || s->finalizing) {
            uint64_t secs = s->duration_ns / 1000000000ULL;
            label(s->countdown ? format(p, "Starts in %.1f s", s->countdown_remaining_ns / 1e9)
                  : s->finalizing
                      ? "Saving…"
                      : format(p, "%02llu:%02llu:%02llu", (unsigned long long)(secs / 3600),
                               (unsigned long long)(secs / 60 % 60),
                               (unsigned long long)(secs % 60)),
                  1, danger);
        }
    }
    const char *live = !s->connected             ? "Disconnected"
                       : !s->config.live_enabled ? "Live output off"
                       : s->state.live_paused    ? "Live paused"
                       : s->state.live_frozen    ? "Live frozen"
                                                 : "Live";
    const char *record = s->state.recording
                             ? (s->state.record_paused ? "Recording paused" : "Recording")
                             : "Not recording";
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 12}})
    {
        label(live, 0,
              s->connected && s->config.live_enabled && !s->state.live_paused &&
                      !s->state.live_frozen
                  ? (Clay_Color){131, 221, 182, 255}
                  : secondary);
        if (s->connected) {
            label(record, 0, s->state.recording ? danger : secondary);
        }
    }
}

static void output_controls(Panel *p)
{
    const PanelSnapshot *s = &p->snapshot;
    bool live = s->connected && s->config.live_enabled;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 8}})
    {
        command_button(p, 30, s->state.live_paused ? "Resume live" : "Pause live", live, "live",
                       s->state.live_paused ? "resume" : "pause", NULL);
        if (live && !s->state.live_paused) {
            command_button(p, 31, s->state.live_frozen ? "Unfreeze" : "Freeze", true, "live",
                           s->state.live_frozen ? "unfreeze" : "freeze", NULL);
        }
        if (!s->state.recording && !s->countdown) {
            command_button(p, 32, s->finalizing ? "Saving…" : "Start record",
                           s->connected && !s->finalizing, "record", "start", NULL);
        }
    }
    if (s->state.recording || s->countdown || s->state.group_paused) {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 8}})
        {
            if (s->state.recording || s->countdown) {
                command_button(p, 32, s->countdown ? "Cancel" : "Stop record", s->connected,
                               "record", "stop", NULL);
            }
            if (s->state.recording) {
                command_button(p, 33, s->state.record_paused ? "Resume" : "Pause", s->connected,
                               "record", s->state.record_paused ? "resume" : "pause", NULL);
            }
            command_button(p, 34, s->state.group_paused ? "Resume all" : "Pause all", s->connected,
                           s->state.group_paused ? "resume" : "pause", NULL, NULL);
        }
    }
}

static void preview_content(Panel *p, bool compact)
{
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .childGap = 6,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
    {
        button(p, 20, "Live", true, !p->record_preview, A_PREVIEW, 0);
        button(p, 21, "Recording", true, p->record_preview, A_PREVIEW, 1);
        if (!compact) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                             .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
            {
                label("Preview", 0, secondary);
            }
        }
    }
    CLAY({.id = CLAY_ID("OutputPreview"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = {5, 6, 7, 255},
          .custom = {.customData = p}})
    {
        if (!p->snapshot.connected) {
            label(compact ? "Disconnected" : "Start the daemon with cast", 0, secondary);
        } else if (!p->preview) {
            label("Waiting for output…", 0, secondary);
        }
    }
}

static void preview_area(Panel *p)
{
    bool compact = p->tab >= 0;
    float preview_height = compact ? 146 : fminf(254, fmaxf(170, p->height - 490));
    CLAY({.id = CLAY_ID("PreviewSpace"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                .height = CLAY_SIZING_FIXED(preview_height)}}})
    {
        if (!compact) {
            CLAY({.id = CLAY_ID("PreviewPanel"),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                             .layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 8}})
            {
                preview_content(p, false);
            }
        } else {
            CLAY({.layout = {
                      .sizing = {.width = CLAY_SIZING_FIXED(100), .height = CLAY_SIZING_GROW()},
                      .layoutDirection = CLAY_TOP_TO_BOTTOM,
                      .childGap = 6,
                      .padding = {0, 0, 14, 0}}})
            {
                label("Preview", 1, foreground);
                text_wrapped("Drag to move", secondary);
            }
        }
    }
}

static void floating_preview(Panel *p)
{
    if (p->tab < 0) {
        return;
    }
    Clay_ElementData space = Clay_GetElementData(CLAY_ID("PreviewSpace"));
    float x = p->preview_moved ? p->preview_x : space.boundingBox.x + space.boundingBox.width - 208;
    float y = p->preview_moved ? p->preview_y : space.boundingBox.y;
    x = fmaxf(8, fminf(x, p->width - 216));
    Clay_BoundingBox scroll = Clay_GetElementData(CLAY_ID("SettingsScroll")).boundingBox;
    float top = space.boundingBox.y;
    float bottom = fmaxf(top, scroll.y + scroll.height - 146);
    y = fmaxf(top, fminf(y, bottom));
    /* A floating preview may cover settings, never navigation, output actions or notices. */
    if (y > top && y < scroll.y) {
        y = y < (top + scroll.y) / 2 || bottom < scroll.y ? top : scroll.y;
    }
    p->preview_x = x;
    p->preview_y = y;
    CLAY({.id = CLAY_ID("PreviewPanel"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(208), .height = CLAY_SIZING_FIXED(146)},
                     .padding = {5, 5, 5, 5},
                     .childGap = 5,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = surface,
          .border = {.color = line, .width = CLAY_BORDER_ALL(1)},
          .floating = {.offset = {x, y}, .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 5}})
    {
        preview_content(p, true);
    }
}

static void home_navigation(Panel *p)
{
    CLAY({.id = CLAY_ID("SettingsScroll"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {0, 0, 8, 0}},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        for (int i = 0; i < 5; i++) {
            uint32_t id = 100 + (uint32_t)i;
            Widget *w = widget(p, id, W_BUTTON, A_TAB, true);
            if (w) {
                w->index = i;
            }
            CLAY({.id = element_id(id),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                        .height = CLAY_SIZING_FIXED(46)},
                             .padding = {10, 12, 0, 0},
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = hot(id) ? control : background,
                  .border = {.color = p->focus == id ? accent : line,
                             .width = p->focus == id ? (Clay_BorderWidth)CLAY_BORDER_ALL(1)
                                                     : (Clay_BorderWidth){.bottom = 1}}})
            {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
                {
                    label(section_names[i], 1, foreground);
                }
                label("Open", 0, secondary);
            }
        }
    }
}
static void selection_row(Panel *p)
{
    label("Composition", 2, foreground);
    static const char *const layouts[] = {"overlay", "split", "screen", "camera"};
    static const char *const names[] = {"Overlay", "Split", "Screen", "Camera"};
    for (int row = 0; row < 2; row++) {
        CLAY({.layout = {.childGap = 8}})
        {
            for (int col = 0; col < 2; col++) {
                int i = row * 2 + col;
                button(p, 50 + (uint32_t)i, names[i], p->snapshot.connected,
                       !strcmp(p->snapshot.config.layout, layouts[i]), A_COMMAND, 0);
                Widget *w = &p->widgets[p->widget_count - 1];
                w->arg[0] = "layout";
                w->arg[1] = layouts[i];
                w->argc = 2;
            }
        }
    }
    CLAY({.layout = {.childGap = 8}})
    {
        button(p, 40, "Preset", p->snapshot.connected && p->snapshot.config.preset_count > 0, false,
               A_DROPDOWN, -1);
        p->widgets[p->widget_count - 1].type = W_SELECT;
        command_button(p, 41, "Next", p->snapshot.connected, "preset", "next", NULL);
        command_button(p, 42, "Reset", p->snapshot.connected, "reset", NULL, NULL);
    }
}
static void source_actions(Panel *p)
{
    bool native = p->snapshot.connected && strcmp(p->snapshot.config.backend, "synthetic");
    CLAY({.layout = {.childGap = 8}})
    {
        command_button(p, 70, "Monitor", native, "capture", "monitor", NULL);
        if (p->snapshot.capabilities.region_selection) {
            command_button(p, 71, "Select region", native, "capture", "region", "select");
        }
    }
    CLAY({.layout = {.childGap = 8}})
    {
        if (p->snapshot.capabilities.window_selection) {
            command_button(p, 72, "Select window", native, "capture", "window", "select");
        }
    }
    CLAY({.layout = {.childGap = 8}})
    {
        command_button(p, 73, "List monitors", native, "screen", "list", NULL);
        command_button(p, 74, "Next monitor", native, "screen", "next", NULL);
    }
    CLAY({.layout = {.childGap = 8}})
    {
        if (p->snapshot.capabilities.window_selection) {
            command_button(p, 75, "Active window", native, "capture", "window", "active");
        }
    }
    label(format(p, "Source: %s%s%s", p->snapshot.config.capture_kind,
                 p->snapshot.config.monitor[0] ? " · " : "", p->snapshot.config.monitor),
          0, secondary);
    if (!p->snapshot.capabilities.region_selection || !p->snapshot.capabilities.window_selection) {
        text_wrapped("Source selection is managed by the capture backend. Unsupported selection "
                     "actions are hidden.",
                     secondary);
    }
    CLAY({.layout = {.childGap = 8}})
    {
        command_button(p, 76, "Zoom out", p->snapshot.connected, "zoom", "out", NULL);
        command_button(p, 77, "Zoom in", p->snapshot.connected, "zoom", "in", NULL);
    }
    CLAY({.layout = {.childGap = 8}})
    {
        command_button(p, 78, "Reset zoom", p->snapshot.connected, "zoom", "reset", NULL);
        command_button(p, 79, "Toggle zoom", p->snapshot.connected, "zoom", "toggle", NULL);
    }
}
static void tab_actions(Panel *p)
{
    if (p->tab == TAB_SOURCE) {
        selection_row(p);
        source_actions(p);
    } else if (p->tab == TAB_CAMERA) {
        CLAY({.layout = {.childGap = 8}})
        {
            command_button(p, 80, "List cameras", p->snapshot.connected, "camera", "list", NULL);
        }
    } else if (p->tab == TAB_AUDIO) {
        command_button(p, 83, "List audio sources", p->snapshot.connected, "audio", "list", NULL);
        text_wrapped("Use a PipeWire source name or node ID. An empty source follows the system "
                     "default. Missing sources produce silence.",
                     secondary);
        if (p->snapshot.audio_status[0]) {
            text_wrapped(p->snapshot.audio_status, secondary);
        }
    } else if (p->tab == TAB_EFFECTS) {
        if (p->snapshot.capabilities.input) {
            command_button(p, 84, "Clear keystrokes", p->snapshot.connected, "keys", "clear", NULL);
        }
        if (!p->snapshot.capabilities.cursor_metadata) {
            text_wrapped(p->snapshot.capabilities.embedded_cursor
                             ? "This backend embeds the cursor in capture. Cursor visibility and "
                               "highlighting require a new capture session."
                             : "This backend provides no pointer metadata. Cursor controls and "
                               "pointer following are unavailable.",
                         secondary);
        }
        if (!p->snapshot.capabilities.input) {
            text_wrapped("This backend cannot observe global keys or clicks. Input annotations are "
                         "unavailable.",
                         secondary);
        }
    } else {
        command_button(p, 85, "Reload config file", p->snapshot.connected, "config", "reload",
                       NULL);
        text_wrapped("Applied changes affect this session. Edit your config file to keep settings "
                     "across daemon restarts.",
                     secondary);
    }
}
static const char *const group_names[5][4] = {
    {"", "Region geometry", "Zoom behavior", ""},
    {"", "Position and crop", "Appearance", "Device"},
    {"", "Virtual microphone", "", ""},
    {"", "Output annotations", "Cursor and clicks", "Keystroke style"},
    {"", "Recording format", "Cycle order", "Output and connection"}};
static void settings_area(Panel *p)
{
    CLAY({.id = CLAY_ID("SettingsScroll"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                     .padding = {0, 10, 0, 12},
                     .childGap = 8,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        tab_actions(p);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (fields[i].tab == p->tab && !fields[i].group) {
                field_row(p, i);
            }
        }
        for (int group = 1; group < 4; group++) {
            if (!group_names[p->tab][group][0]) {
                continue;
            }
            bool any = false;
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (fields[i].tab == p->tab && fields[i].group == group &&
                    supported(p, &fields[i])) {
                    any = true;
                }
            }
            if (!any) {
                continue;
            }
            button(p, 200 + (uint32_t)p->tab * 4 + (uint32_t)group,
                   format(p, "%s  %s", p->groups[p->tab][group] ? "Hide" : "Show",
                          group_names[p->tab][group]),
                   true, false, A_GROUP, group);
            if (p->groups[p->tab][group]) {
                for (size_t i = 0; i < FIELD_COUNT; i++) {
                    if (fields[i].tab == p->tab && fields[i].group == group) {
                        field_row(p, i);
                    }
                }
                if (p->tab == TAB_SOURCE && group == 1) {
                    button(p, 86, "Use region geometry", p->snapshot.connected, false, A_COMMAND,
                           0);
                    Widget *w = &p->widgets[p->widget_count - 1];
                    w->argc = 6;
                    w->arg[0] = "capture";
                    w->arg[1] = "region";
                    w->arg[2] = format(p, "%d", p->snapshot.config.region_x);
                    w->arg[3] = format(p, "%d", p->snapshot.config.region_y);
                    w->arg[4] = format(p, "%d", p->snapshot.config.region_w);
                    w->arg[5] = format(p, "%d", p->snapshot.config.region_h);
                }
            }
        }
        if (p->reply[0] && strchr(p->reply, '\n')) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {12, 12, 12, 12}},
                  .backgroundColor = background,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
            {
                text_wrapped(p->reply, secondary);
            }
        }
    }
}
static void footer(Panel *p)
{
    const char *error = p->error[0] ? p->error : p->snapshot.error;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIT()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 4}})
    {
        if (!p->snapshot.connected) {
            text_wrapped(error[0]
                             ? error
                             : "Daemon unavailable. Run cast; this panel reconnects automatically.",
                         danger);
        } else if (error[0]) {
            text_wrapped(error, danger);
        } else if (p->snapshot.command_queued > p->snapshot.command_completed) {
            label("Applying…", 0, accent);
        } else if (p->reply[0] && strcmp(p->reply, "ok")) {
            text_wrapped(strchr(p->reply, '\n') ? "Command result appears in the settings area."
                                                : p->reply,
                         secondary);
        } else if (p->tab == TAB_SETTINGS) {
            text_wrapped(format(p, "%s · %d × %d · %d fps", p->snapshot.config.backend,
                                p->snapshot.config.width, p->snapshot.config.height,
                                p->snapshot.config.fps),
                         secondary);
        }
        if (p->snapshot.connected && p->snapshot.exclusion[0]) {
            text_wrapped(p->snapshot.exclusion, secondary);
        }
    }
}
static void dropdown_layout(Panel *p)
{
    if (!p->dropdown) {
        return;
    }
    Clay_ElementData anchor = Clay_GetElementData(element_id(p->dropdown));
    if (!anchor.found) {
        p->dropdown = 0;
        return;
    }
    int count = p->dropdown_field < 0 ? p->snapshot.config.preset_count
                                      : choice_count(fields[p->dropdown_field].choices);
    float h = 40 * count + 8;
    float x = fminf(anchor.boundingBox.x, p->width - 264),
          y = anchor.boundingBox.y + anchor.boundingBox.height + 4;
    if (y + h > p->height - 16) {
        y = fmaxf(16, anchor.boundingBox.y - h - 4);
    }
    CLAY({.id = CLAY_ID("Dropdown"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(248),
                                .height = CLAY_SIZING_FIXED(fminf(h, p->height - 32))},
                     .padding = {4, 4, 4, 4},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = surface,
          .cornerRadius = CLAY_CORNER_RADIUS(6),
          .border = {.color = line, .width = CLAY_BORDER_ALL(1)},
          .floating = {.offset = {x, y}, .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 10},
          .clip = {.vertical = true}})
    {
        for (int i = 0; i < count; i++) {
            char value[128];
            if (p->dropdown_field < 0) {
                snprintf(value, sizeof value, "%s", p->snapshot.config.presets[i].name);
            } else {
                choice_value(fields[p->dropdown_field].choices, i, value, sizeof value);
            }
            button(p, 400 + (uint32_t)i, format(p, "%s", value), true, i == p->dropdown_choice,
                   A_OPTION, i);
            Widget *w = &p->widgets[p->widget_count - 1];
            w->type = W_OPTION;
            snprintf(w->value, sizeof w->value, "%s", value);
        }
    }
}
static Clay_RenderCommandArray layout(Panel *p)
{
    p->widget_count = 0;
    p->string_used = 0;
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("Panel"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 12, 12},
                     .childGap = 10,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = background})
    {
        CLAY(
            {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0, 520), .height = CLAY_SIZING_GROW()},
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 10}})
        {
            status_line(p);
            preview_area(p);
            output_controls(p);
            if (p->tab < 0) {
                home_navigation(p);
            } else {
                settings_area(p);
            }
            footer(p);
        }
    }
    floating_preview(p);
    dropdown_layout(p);
    Clay_RenderCommandArray commands = Clay_EndLayout();
    for (int i = 0; i < p->widget_count; i++) {
        Clay_ElementData data = Clay_GetElementData(element_id(p->widgets[i].id));
        p->widgets[i].box = data.boundingBox;
    }
    return commands;
}

static Clay_Dimensions measure(Clay_StringSlice text, Clay_TextElementConfig *config, void *data)
{
    Panel *p = data;
    int width = 0, height = 0;
    int font = config->fontId < 4 ? config->fontId : 1;
    if (text.length > 0) {
        TTF_GetStringSize(p->font[font], text.chars, (size_t)text.length, &width, &height);
    }
    return (Clay_Dimensions){(float)width,
                             (float)(height ? height : TTF_GetFontHeight(p->font[font]))};
}
static void clay_error(Clay_ErrorData error)
{
    Panel *p = error.userData;
    snprintf(p->error, sizeof p->error, "Panel layout: %.*s", error.errorText.length,
             error.errorText.chars);
}
static void color(Panel *p, Clay_Color c)
{
    SDL_SetRenderDrawColor(p->renderer, (Uint8)c.r, (Uint8)c.g, (Uint8)c.b, (Uint8)c.a);
}
static SDL_FRect rect(Clay_BoundingBox b)
{
    return (SDL_FRect){b.x, b.y, b.width, b.height};
}
static void rounded(Panel *p, SDL_FRect box, float radius, Clay_Color c)
{
    if (box.w <= 0 || box.h <= 0) {
        return;
    }
    color(p, c);
    radius = fminf(radius, fminf(box.w, box.h) / 2);
    if (radius < 1) {
        SDL_RenderFillRect(p->renderer, &box);
        return;
    }
    SDL_Vertex vertices[38];
    int indices[108];
    SDL_FColor ink = {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
    vertices[0] = (SDL_Vertex){.position = {box.x + box.w / 2, box.y + box.h / 2}, .color = ink};
    const float cx[] = {box.x + box.w - radius, box.x + box.w - radius, box.x + radius,
                        box.x + radius};
    const float cy[] = {box.y + radius, box.y + box.h - radius, box.y + box.h - radius,
                        box.y + radius};
    int count = 1;
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= 8; i++) {
            float angle = ((float)corner * 90 - 90 + (float)i * 90 / 8) * (float)M_PI / 180;
            vertices[count++] = (SDL_Vertex){
                .position = {cx[corner] + cosf(angle) * radius, cy[corner] + sinf(angle) * radius},
                .color = ink};
        }
    }
    for (int i = 0; i < count - 1; i++) {
        indices[i * 3] = 0;
        indices[i * 3 + 1] = i + 1;
        indices[i * 3 + 2] = i + 2 == count ? 1 : i + 2;
    }
    SDL_RenderGeometry(p->renderer, NULL, vertices, count, indices, (count - 1) * 3);
}
static TextCache *cached_text(Panel *p, const char *text, size_t length, int font)
{
    TextCache *slot = NULL;
    for (int i = 0; i < TEXT_CACHE_MAX; i++) {
        TextCache *c = &p->cache[i];
        if (c->text && c->size == font && strlen(c->text) == length &&
            !memcmp(c->text, text, length)) {
            c->used = p->draw_frame;
            return c;
        }
        if (!slot || !c->text || (slot->text && c->used < slot->used)) {
            slot = c;
        }
    }
    if (!slot) {
        return NULL;
    }
    SDL_DestroyTexture(slot->texture);
    free(slot->text);
    memset(slot, 0, sizeof *slot);
    slot->text = malloc(length + 1);
    if (!slot->text) {
        return NULL;
    }
    memcpy(slot->text, text, length);
    slot->text[length] = 0;
    SDL_Surface *surface_text =
        TTF_RenderText_Blended(p->raster_font[font], text, length, (SDL_Color){255, 255, 255, 255});
    if (!surface_text) {
        free(slot->text);
        slot->text = NULL;
        return NULL;
    }
    slot->width = surface_text->w;
    slot->height = surface_text->h;
    slot->size = font;
    slot->texture = SDL_CreateTextureFromSurface(p->renderer, surface_text);
    SDL_DestroySurface(surface_text);
    if (slot->texture) {
        SDL_SetTextureScaleMode(slot->texture, SDL_SCALEMODE_NEAREST);
    }
    slot->used = p->draw_frame;
    return slot;
}
static void outline(Panel *p, SDL_FRect box, float radius, Clay_Color ink)
{
    color(p, ink);
    radius = fminf(radius, fminf(box.w, box.h) / 2);
    if (radius < 1) {
        SDL_RenderRect(p->renderer, &box);
        return;
    }
    SDL_FPoint points[37];
    const float cx[] = {box.x + box.w - radius, box.x + box.w - radius, box.x + radius,
                        box.x + radius};
    const float cy[] = {box.y + radius, box.y + box.h - radius, box.y + box.h - radius,
                        box.y + radius};
    int count = 0;
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= 8; i++) {
            float angle = ((float)corner * 90 - 90 + (float)i * 90 / 8) * (float)M_PI / 180;
            points[count++] =
                (SDL_FPoint){cx[corner] + cosf(angle) * radius, cy[corner] + sinf(angle) * radius};
        }
    }
    points[count++] = points[0];
    SDL_RenderLines(p->renderer, points, count);
}
static void draw_text(Panel *p, const char *text, size_t length, int font, Clay_Color ink, float x,
                      float y)
{
    if (!length) {
        return;
    }
    TextCache *c = cached_text(p, text, length, font);
    if (!c || !c->texture) {
        return;
    }
    SDL_SetTextureColorMod(c->texture, (Uint8)ink.r, (Uint8)ink.g, (Uint8)ink.b);
    SDL_SetTextureAlphaMod(c->texture, (Uint8)ink.a);
    SDL_FRect box = {roundf(x * p->density) / p->density, roundf(y * p->density) / p->density,
                     c->width / p->density, c->height / p->density};
    SDL_RenderTexture(p->renderer, c->texture, NULL, &box);
}
static SDL_Rect intersect(SDL_Rect a, SDL_Rect b)
{
    SDL_Rect result;
    if (!SDL_GetRectIntersection(&a, &b, &result)) {
        return (SDL_Rect){0, 0, 0, 0};
    }
    return result;
}
static SDL_Rect clip_rect(Clay_BoundingBox b)
{
    return (SDL_Rect){(int)floorf(b.x), (int)floorf(b.y), (int)ceilf(b.width),
                      (int)ceilf(b.height)};
}
static void draw_field(Panel *p, size_t index, Clay_BoundingBox b, SDL_Rect outer_clip)
{
    SDL_Rect own_clip = {(int)b.x + 8, (int)b.y + 2, (int)b.width - 16, (int)b.height - 4};
    SDL_Rect clip = intersect(outer_clip, own_clip);
    SDL_SetRenderClipRect(p->renderer, &clip);
    const char *value = p->edit[index].value;
    uint32_t id = 1000 + (uint32_t)index * 3;
    int before = 0, height = 0, total = 0;
    TTF_GetStringSize(p->font[1], value, strlen(value), &total, &height);
    float x = b.x + 10, y = b.y + (b.height - (float)TTF_GetFontHeight(p->font[1])) / 2;
    if (p->active_text == id) {
        size_t caret = p->caret < strlen(value) ? p->caret : strlen(value);
        if (caret) {
            TTF_GetStringSize(p->font[1], value, caret, &before, &height);
        }
        if (before > b.width - 30) {
            x -= before - (b.width - 30);
        }
        if (p->select_all && value[0]) {
            SDL_FRect selection = {x, y, (float)total, (float)TTF_GetFontHeight(p->font[1])};
            color(p, (Clay_Color){51, 86, 111, 255});
            SDL_RenderFillRect(p->renderer, &selection);
        }
        if ((SDL_GetTicks() / 500) % 2 == 0) {
            color(p, accent);
            SDL_RenderLine(p->renderer, x + before, y + 2, x + before,
                           y + TTF_GetFontHeight(p->font[1]) - 2);
        }
    }
    draw_text(p, value[0] ? value : "Default", strlen(value[0] ? value : "Default"), 1,
              value[0] ? foreground : muted, x, y);
    SDL_SetRenderClipRect(p->renderer, &outer_clip);
}
static void render(Panel *p, Clay_RenderCommandArray commands)
{
    p->draw_frame++;
    SDL_SetRenderClipRect(p->renderer, NULL);
    color(p, background);
    SDL_RenderClear(p->renderer);
    SDL_Rect clips[32] = {{0, 0, (int)p->width, (int)p->height}};
    int depth = 0;
    for (int32_t i = 0; i < commands.length; i++) {
        const Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&commands, i);
        SDL_FRect box = rect(c->boundingBox);
        switch (c->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            rounded(p, box, c->renderData.rectangle.cornerRadius.topLeft,
                    c->renderData.rectangle.backgroundColor);
            break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const Clay_BorderRenderData *border = &c->renderData.border;
            color(p, border->color);
            if (border->width.left && border->width.right && border->width.top &&
                border->width.bottom) {
                outline(p, box, border->cornerRadius.topLeft, border->color);
            } else {
                if (border->width.bottom) {
                    SDL_RenderLine(p->renderer, box.x, box.y + box.h - 1, box.x + box.w,
                                   box.y + box.h - 1);
                }
                if (border->width.top) {
                    SDL_RenderLine(p->renderer, box.x, box.y, box.x + box.w, box.y);
                }
                if (border->width.left) {
                    SDL_RenderLine(p->renderer, box.x, box.y, box.x, box.y + box.h);
                }
                if (border->width.right) {
                    SDL_RenderLine(p->renderer, box.x + box.w - 1, box.y, box.x + box.w - 1,
                                   box.y + box.h);
                }
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            draw_text(p, c->renderData.text.stringContents.chars,
                      (size_t)c->renderData.text.stringContents.length,
                      c->renderData.text.fontId < 4 ? c->renderData.text.fontId : 1,
                      c->renderData.text.textColor, box.x, box.y);
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            if (depth < 31) {
                SDL_Rect area = clip_rect(c->boundingBox);
                if (!c->renderData.clip.horizontal) {
                    area.x = clips[depth].x;
                    area.w = clips[depth].w;
                }
                if (!c->renderData.clip.vertical) {
                    area.y = clips[depth].y;
                    area.h = clips[depth].h;
                }
                clips[depth + 1] = intersect(clips[depth], area);
                depth++;
                SDL_SetRenderClipRect(p->renderer, &clips[depth]);
            }
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            if (depth) {
                depth--;
            }
            SDL_SetRenderClipRect(p->renderer, &clips[depth]);
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            rounded(p, box, c->renderData.custom.cornerRadius.topLeft,
                    c->renderData.custom.backgroundColor);
            if (c->renderData.custom.customData == p) {
                if (p->preview && p->snapshot.connected && p->frame.width > 0 &&
                    p->frame.height > 0) {
                    float scale = fminf(box.w / p->frame.width, box.h / p->frame.height);
                    SDL_FRect image = {box.x + (box.w - p->frame.width * scale) / 2,
                                       box.y + (box.h - p->frame.height * scale) / 2,
                                       p->frame.width * scale, p->frame.height * scale};
                    SDL_RenderTexture(p->renderer, p->preview, NULL, &image);
                }
            } else {
                size_t field = (size_t)(uintptr_t)c->renderData.custom.customData - 1;
                if (field < FIELD_COUNT) {
                    draw_field(p, field, c->boundingBox, clips[depth]);
                }
            }
            break;
        default:
            break;
        }
    }
    SDL_SetRenderClipRect(p->renderer, NULL);
    /* Native dropdown chevrons are geometry, so font coverage cannot change them. */
    for (int i = 0; i < p->widget_count; i++) {
        const Widget *w = &p->widgets[i];
        if (w->type != W_SELECT) {
            continue;
        }
        Clay_ElementData scroll = Clay_GetElementData(CLAY_ID("SettingsScroll"));
        if (w->id >= 1000) {
            SDL_SetRenderClipRect(p->renderer,
                                  &(SDL_Rect){(int)scroll.boundingBox.x, (int)scroll.boundingBox.y,
                                              (int)scroll.boundingBox.width,
                                              (int)scroll.boundingBox.height});
        }
        color(p, w->enabled ? secondary : muted);
        float x = w->box.x + w->box.width - 5, y = w->box.y + w->box.height / 2;
        SDL_RenderLine(p->renderer, x - 5, y - 2, x - 2, y + 1);
        SDL_RenderLine(p->renderer, x - 2, y + 1, x + 1, y - 2);
        SDL_SetRenderClipRect(p->renderer, NULL);
    }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    Clay_ElementData area = Clay_GetElementData(CLAY_ID("SettingsScroll"));
    if (scroll.found && scroll.contentDimensions.height > scroll.scrollContainerDimensions.height) {
        float ratio = scroll.scrollContainerDimensions.height / scroll.contentDimensions.height;
        float thumb = fmaxf(24, area.boundingBox.height * ratio);
        float amount = -scroll.scrollPosition->y /
                       (scroll.contentDimensions.height - scroll.scrollContainerDimensions.height);
        rounded(p,
                (SDL_FRect){area.boundingBox.x + area.boundingBox.width - 6,
                            area.boundingBox.y + amount * (area.boundingBox.height - thumb), 3,
                            thumb},
                1.5f, muted);
    }
}

static Widget *find_widget(Panel *p, uint32_t id)
{
    for (int i = 0; i < p->widget_count; i++) {
        if (p->widgets[i].id == id) {
            return &p->widgets[i];
        }
    }
    return NULL;
}
static void stop_editing(Panel *p)
{
    p->active_text = 0;
    p->select_all = false;
    SDL_StopTextInput(p->window);
}
static void set_focus(Panel *p, Widget *w)
{
    if (!w || !w->enabled) {
        return;
    }
    if (p->focus != w->id) {
        stop_editing(p);
    }
    p->focus = w->id;
    if (w->type == W_FIELD) {
        p->active_text = w->id;
        p->caret = strlen(p->edit[w->index].value);
        p->select_all = true;
        SDL_StartTextInput(p->window);
        SDL_Rect area = clip_rect(w->box);
        SDL_SetTextInputArea(p->window, &area, (int)p->caret);
    }
    if (w->id >= 1000 || (w->id >= 200 && w->id < 300) || (w->id >= 40 && w->id < 90) ||
        (w->id >= 100 && w->id < 105)) {
        Clay_ElementData area = Clay_GetElementData(CLAY_ID("SettingsScroll"));
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
        if (scroll.found) {
            if (w->box.y < area.boundingBox.y) {
                scroll.scrollPosition->y += area.boundingBox.y - w->box.y + 8;
            } else if (w->box.y + w->box.height > area.boundingBox.y + area.boundingBox.height) {
                scroll.scrollPosition->y -=
                    w->box.y + w->box.height - area.boundingBox.y - area.boundingBox.height + 8;
            }
        }
    }
}
static void move_focus(Panel *p, int direction)
{
    int start = direction > 0 ? -1 : p->widget_count;
    for (int i = 0; i < p->widget_count; i++) {
        if (p->widgets[i].id == p->focus) {
            start = i;
            break;
        }
    }
    for (int step = 1; step <= p->widget_count; step++) {
        int index = (start + direction * step + p->widget_count * 2) % p->widget_count;
        Widget *w = &p->widgets[index];
        if (w->enabled && (!p->dropdown || w->type == W_OPTION)) {
            set_focus(p, w);
            if (w->type == W_OPTION) {
                p->dropdown_choice = w->index;
            }
            return;
        }
    }
}
static void apply_setting(Panel *p, int index, const char *value, bool draft)
{
    if (index < 0 || (size_t)index >= FIELD_COUNT || !writable(p, &fields[index])) {
        return;
    }
    if (draft && p->edit[index].pending) {
        return;
    }
    if (validate_field(&fields[index], value, p->error, sizeof p->error)) {
        return;
    }
    p->error[0] = 0;
    int result;
    const char *key = fields[index].key;
    const char *args[4] = {0};
    char parameter[128], other[32];
    int argc = 0;
    if (!strcmp(key, "output.pause_text")) {
        args[0] = "live";
        args[1] = "message";
        args[2] = value;
        argc = 3;
    } else if (!strcmp(key, "camera.width_percent")) {
        snprintf(parameter, sizeof parameter, "%.17g%%", strtod(value, NULL));
        args[0] = "camera";
        args[1] = "size";
        args[2] = parameter;
        argc = 3;
    } else if (!strcmp(key, "camera.x") || !strcmp(key, "camera.y") ||
               (!strcmp(key, "camera.anchor") && !strcmp(value, "free"))) {
        bool x = !strcmp(key, "camera.x"), y = !strcmp(key, "camera.y");
        snprintf(parameter, sizeof parameter, "%d", p->snapshot.config.camera_x);
        snprintf(other, sizeof other, "%d", p->snapshot.config.camera_y);
        args[0] = "camera";
        args[1] = "position";
        args[2] = x ? value : parameter;
        args[3] = y ? value : other;
        argc = 4;
    } else if (!strcmp(key, "camera.anchor") || !strcmp(key, "camera.shape") ||
               !strcmp(key, "camera.aspect")) {
        args[0] = "camera";
        args[1] = key + strlen("camera.");
        args[2] = value;
        argc = 3;
    } else if (!strcmp(key, "camera.mirror")) {
        args[0] = "camera";
        args[1] = "mirror";
        args[2] = !strcmp(value, "true") ? "on" : "off";
        argc = 3;
    } else if (!strcmp(key, "camera.visible")) {
        args[0] = "camera";
        args[1] = !strcmp(value, "true") ? "show" : "hide";
        argc = 2;
    } else if (!strcmp(key, "camera.device")) {
        args[0] = "camera";
        args[1] = "device";
        args[2] = value;
        argc = 3;
    } else if (!strcmp(key, "zoom.factor")) {
        args[0] = "zoom";
        args[1] = "set";
        args[2] = value;
        argc = 3;
    } else if (!strcmp(key, "zoom.follow")) {
        args[0] = "zoom";
        args[1] = "follow";
        args[2] = !strcmp(value, "true") ? "on" : "off";
        argc = 3;
    }
    if (argc) {
        result = panel_client_command(p->client, argc, args, p->error, sizeof p->error);
    } else {
        result = panel_client_setting(p->client, key, value, p->error, sizeof p->error);
    }
    if (result) {
        return;
    }
    p->reply[0] = 0;
    if (draft) {
        p->edit[index].pending = p->snapshot.command_queued + 1;
        p->edit[index].submitted_revision = p->edit[index].revision;
    }
}
static void activate(Panel *p, Widget *w)
{
    if (!w || !w->enabled) {
        return;
    }
    switch (w->action) {
    case A_COMMAND:
        p->error[0] = 0;
        if (!panel_client_command(p->client, w->argc, w->arg, p->error, sizeof p->error)) {
            p->reply[0] = 0;
        }
        break;
    case A_SETTING:
        apply_setting(p, w->index, w->value, false);
        break;
    case A_APPLY:
        apply_setting(p, w->index, p->edit[w->index].value, true);
        stop_editing(p);
        break;
    case A_FIELD:
        set_focus(p, w);
        break;
    case A_TAB:
        stop_editing(p);
        p->tab = w->index;
        p->focus = p->tab < 0 ? 100 : 99;
        p->dropdown = 0;
        {
            Clay_ScrollContainerData scroll =
                Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
            if (scroll.found) {
                *scroll.scrollPosition = (Clay_Vector2){0, 0};
            }
        }
        break;
    case A_GROUP:
        p->groups[p->tab][w->index] = !p->groups[p->tab][w->index];
        break;
    case A_DROPDOWN:
        stop_editing(p);
        if (p->dropdown == w->id) {
            p->dropdown = 0;
            break;
        }
        p->dropdown = w->id;
        p->dropdown_field = w->index;
        p->dropdown_choice = 0;
        if (w->index >= 0) {
            char value[PATH_MAX], option[128];
            field_value(&fields[w->index], &p->snapshot.config, value, sizeof value);
            for (int i = 0; i < choice_count(fields[w->index].choices); i++) {
                choice_value(fields[w->index].choices, i, option, sizeof option);
                if (!strcmp(option, value)) {
                    p->dropdown_choice = i;
                }
            }
        }
        break;
    case A_OPTION:
        if (p->dropdown_field < 0) {
            const char *args[] = {"preset", w->value};
            panel_client_command(p->client, 2, args, p->error, sizeof p->error);
        } else {
            apply_setting(p, p->dropdown_field, w->value, false);
        }
        p->focus = p->dropdown;
        p->dropdown = 0;
        break;
    case A_PREVIEW:
        p->record_preview = w->index != 0;
        SDL_DestroyTexture(p->preview);
        p->preview = NULL;
        frame_free(&p->frame);
        break;
    default:
        break;
    }
}

static size_t previous_character(const char *text, size_t pos)
{
    if (!pos) {
        return 0;
    }
    pos--;
    while (pos && ((unsigned char)text[pos] & 0xc0) == 0x80) {
        pos--;
    }
    return pos;
}
static size_t next_character(const char *text, size_t pos)
{
    size_t length = strlen(text);
    if (pos >= length) {
        return length;
    }
    pos++;
    while (pos < length && ((unsigned char)text[pos] & 0xc0) == 0x80) {
        pos++;
    }
    return pos;
}
static FieldEdit *active_edit(Panel *p, int *index)
{
    Widget *w = find_widget(p, p->active_text);
    if (!w || w->type != W_FIELD || !w->enabled) {
        return NULL;
    }
    *index = w->index;
    return &p->edit[w->index];
}
static void insert_text(Panel *p, const char *text)
{
    /* An empty or unavailable clipboard must not replace the selected draft. */
    if (!text || !text[0]) {
        return;
    }
    int index;
    FieldEdit *edit = active_edit(p, &index);
    if (!edit) {
        return;
    }
    for (const unsigned char *s = (const unsigned char *)text; *s; s++) {
        if (*s < 32 || *s == 127) {
            snprintf(p->error, sizeof p->error, "Use a single line for %s", fields[index].label);
            return;
        }
    }
    size_t limit = fields[index].type == FIELD_TEXT ? fields[index].length - 1 : 63;
    size_t used = p->select_all ? 0 : strlen(edit->value), added = strlen(text);
    if (used + added > limit || used + added >= sizeof edit->value) {
        snprintf(p->error, sizeof p->error, "%s accepts at most %zu bytes", fields[index].label,
                 limit);
        return;
    }
    if (p->select_all) {
        edit->value[0] = 0;
        p->caret = 0;
        p->select_all = false;
    }
    if (p->caret > used) {
        p->caret = used;
    }
    memmove(edit->value + p->caret + added, edit->value + p->caret, used - p->caret + 1);
    memcpy(edit->value + p->caret, text, added);
    p->caret += added;
    edit->dirty = true;
    edit->revision++;
    p->error[0] = 0;
}
static void key_event(Panel *p, const SDL_KeyboardEvent *event)
{
    SDL_Keycode key = event->key;
    bool ctrl = (event->mod & SDL_KMOD_CTRL) != 0;
    if (ctrl && key == SDLK_Q) {
        p->quit = true;
        return;
    }
    if (key == SDLK_TAB) {
        move_focus(p, event->mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (key == SDLK_ESCAPE) {
        if (p->dropdown) {
            p->focus = p->dropdown;
            p->dropdown = 0;
            return;
        }
        int index;
        FieldEdit *edit = active_edit(p, &index);
        if (edit) {
            field_value(&fields[index], &p->snapshot.config, edit->value, sizeof edit->value);
            edit->dirty = false;
        }
        stop_editing(p);
        if (!edit && p->tab >= 0) {
            p->tab = -1;
            p->focus = 100;
        }
        return;
    }
    if (p->dropdown &&
        (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT)) {
        int count = p->dropdown_field < 0 ? p->snapshot.config.preset_count
                                          : choice_count(fields[p->dropdown_field].choices);
        if (count) {
            int step = key == SDLK_UP || key == SDLK_LEFT ? -1 : 1;
            p->dropdown_choice = (p->dropdown_choice + step + count) % count;
            p->focus = 400 + (uint32_t)p->dropdown_choice;
        }
        return;
    }
    int index;
    FieldEdit *edit = active_edit(p, &index);
    if (edit) {
        size_t length = strlen(edit->value);
        if (ctrl && key == SDLK_A) {
            p->select_all = true;
            return;
        }
        if (ctrl && key == SDLK_C) {
            if (p->select_all) {
                SDL_SetClipboardText(edit->value);
            }
            return;
        }
        if (ctrl && key == SDLK_V) {
            char *text = SDL_GetClipboardText();
            if (text) {
                insert_text(p, text);
                SDL_free(text);
            }
            return;
        }
        if (key == SDLK_HOME) {
            p->caret = 0;
            p->select_all = false;
        } else if (key == SDLK_END) {
            p->caret = length;
            p->select_all = false;
        } else if (key == SDLK_LEFT) {
            p->caret = p->select_all ? 0 : previous_character(edit->value, p->caret);
            p->select_all = false;
        } else if (key == SDLK_RIGHT) {
            p->caret = p->select_all ? length : next_character(edit->value, p->caret);
            p->select_all = false;
        } else if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
            if (p->select_all) {
                edit->value[0] = 0;
                p->caret = 0;
                p->select_all = false;
            } else if (key == SDLK_BACKSPACE && p->caret) {
                size_t start = previous_character(edit->value, p->caret);
                memmove(edit->value + start, edit->value + p->caret, length - p->caret + 1);
                p->caret = start;
            } else if (key == SDLK_DELETE && p->caret < length) {
                size_t end = next_character(edit->value, p->caret);
                memmove(edit->value + p->caret, edit->value + end, length - end + 1);
            }
            edit->dirty = true;
            edit->revision++;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (edit->dirty) {
                apply_setting(p, index, edit->value, true);
            }
            stop_editing(p);
        }
        return;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        if (p->dropdown) {
            activate(p, find_widget(p, 400 + (uint32_t)p->dropdown_choice));
        } else {
            activate(p, find_widget(p, p->focus));
        }
    } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN) {
        Widget *w = find_widget(p, p->focus);
        if (w && w->type == W_SELECT) {
            activate(p, w);
        } else {
            move_focus(p, key == SDLK_LEFT || key == SDLK_UP ? -1 : 1);
        }
    }
}
static void click_event(Panel *p, float x, float y)
{
    if (p->tab >= 0 && !p->dropdown) {
        Clay_BoundingBox b = Clay_GetElementData(CLAY_ID("PreviewPanel")).boundingBox;
        if (x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height) {
            for (int i = 20; i <= 21; i++) {
                Widget *w = find_widget(p, (uint32_t)i);
                if (w && x >= w->box.x && x < w->box.x + w->box.width && y >= w->box.y &&
                    y < w->box.y + w->box.height) {
                    set_focus(p, w);
                    activate(p, w);
                    return;
                }
            }
            p->preview_dragging = true;
            p->preview_moved = true;
            p->drag_x = x - b.x;
            p->drag_y = y - b.y;
            return;
        }
    }
    for (int i = p->widget_count - 1; i >= 0; i--) {
        Widget *w = &p->widgets[i];
        if (p->dropdown && w->type != W_OPTION && w->id != p->dropdown) {
            continue;
        }
        if (!w->enabled || x < w->box.x || y < w->box.y || x >= w->box.x + w->box.width ||
            y >= w->box.y + w->box.height) {
            continue;
        }
        if (w->id >= 1000 || (w->id >= 200 && w->id < 300) || (w->id >= 40 && w->id < 90) ||
            (w->id >= 100 && w->id < 105)) {
            Clay_ElementData area = Clay_GetElementData(CLAY_ID("SettingsScroll"));
            if (y < area.boundingBox.y || y >= area.boundingBox.y + area.boundingBox.height) {
                continue;
            }
        }
        set_focus(p, w);
        activate(p, w);
        return;
    }
    p->dropdown = 0;
    stop_editing(p);
}
static void event(Panel *p, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        p->quit = true;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        p->mouse_x = e->motion.x / p->input_scale;
        p->mouse_y = e->motion.y / p->input_scale;
        if (p->preview_dragging) {
            p->preview_x = p->mouse_x - p->drag_x;
            p->preview_y = p->mouse_y - p->drag_y;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            p->mouse_down = true;
            click_event(p, e->button.x / p->input_scale, e->button.y / p->input_scale);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            p->mouse_down = false;
            p->preview_dragging = false;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        p->scroll += e->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN:
        key_event(p, &e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        insert_text(p, e->text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        p->mouse_down = false;
        p->preview_dragging = false;
        p->dropdown = 0;
        stop_editing(p);
        break;
    default:
        break;
    }
}
static void clear_preview(Panel *p)
{
    SDL_DestroyTexture(p->preview);
    p->preview = NULL;
    frame_free(&p->frame);
}
static void acknowledge_edit(FieldEdit *edit, bool failed)
{
    /* An acknowledgement belongs to the submitted draft, not later typing. */
    if (!failed && edit->revision == edit->submitted_revision) {
        edit->dirty = false;
    }
    edit->pending = 0;
}

static void poll_client(Panel *p)
{
    PanelSnapshot fresh;
    if (panel_client_snapshot(p->client, &fresh)) {
        if (!fresh.connected) {
            fresh.config = p->snapshot.config;
            fresh.capabilities = p->snapshot.capabilities;
        }
        bool changed =
            fresh.daemon_generation != p->frame_generation || fresh.privacy_epoch != p->frame_epoch;
        if (!fresh.connected || changed) {
            clear_preview(p);
        }
        p->frame_generation = fresh.daemon_generation;
        p->frame_epoch = fresh.privacy_epoch;
        p->snapshot = fresh;
        if (fresh.command_completed != p->command_seen) {
            p->command_seen = fresh.command_completed;
            snprintf(p->reply, sizeof p->reply, "%s", fresh.last_reply);
            snprintf(p->error, sizeof p->error, "%s", fresh.command_failed ? fresh.last_reply : "");
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (p->edit[i].pending && fresh.command_completed >= p->edit[i].pending) {
                    acknowledge_edit(&p->edit[i], fresh.command_failed);
                }
            }
        }
        if (!fresh.connected) {
            p->dropdown = 0;
            stop_editing(p);
        }
    }
    char error[CAST_ERR];
    int rc = panel_client_frame(p->client, p->record_preview, &p->frame, error, sizeof error);
    if (rc < 0) {
        clear_preview(p);
        return;
    }
    if (rc == 0) {
        return;
    }
    float w = 0, h = 0;
    if (!p->preview || !SDL_GetTextureSize(p->preview, &w, &h) || (int)w != p->frame.width ||
        (int)h != p->frame.height) {
        SDL_DestroyTexture(p->preview);
        p->preview =
            SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                              p->frame.width, p->frame.height);
        if (p->preview) {
            SDL_SetTextureScaleMode(p->preview, SDL_SCALEMODE_LINEAR);
        }
    }
    if (!p->preview || !SDL_UpdateTexture(p->preview, NULL, p->frame.data, p->frame.stride)) {
        snprintf(p->error, sizeof p->error, "Output preview: %s", SDL_GetError());
        clear_preview(p);
    }
}
extern const unsigned char cast_panel_font_data[], cast_panel_font_end[];
static uint64_t window_xid(SDL_Window *window)
{
#ifdef WITH_X11
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    Display *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    Window xid = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (display && xid) {
        XClassHint hint = {.res_name = "cast-panel", .res_class = "CastPanel"};
        XSetClassHint(display, xid, &hint);
        XSync(display, False);
        return (uint64_t)xid;
    }
#else
    (void)window;
#endif
    return 0;
}
static void cleanup(Panel *p, void *clay_memory)
{
    if (!p) {
        free(clay_memory);
        return;
    }
    panel_client_close(p->client);
    clear_preview(p);
    for (int i = 0; i < TEXT_CACHE_MAX; i++) {
        SDL_DestroyTexture(p->cache[i].texture);
        free(p->cache[i].text);
    }
    for (int i = 0; i < 4; i++) {
        TTF_CloseFont(p->font[i]);
        TTF_CloseFont(p->raster_font[i]);
    }
    SDL_DestroyRenderer(p->renderer);
    SDL_DestroyWindow(p->window);
    free(clay_memory);
    free(p);
    TTF_Quit();
    SDL_Quit();
}
static void json_string(FILE *file, const char *value)
{
    fputc('"', file);
    for (const unsigned char *s = (const unsigned char *)value; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', file);
            fputc(*s, file);
        } else if (*s < 32) {
            fprintf(file, "\\u%04x", *s);
        } else {
            fputc(*s, file);
        }
    }
    fputc('"', file);
}

/* Opt-in native test diagnostics report actual layout and decoded preview state. */
static void write_ui_state(Panel *p, const char *path)
{
    if (!path) {
        return;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        return;
    }
    Clay_BoundingBox preview = Clay_GetElementData(CLAY_ID("OutputPreview")).boundingBox;
    Clay_BoundingBox panel = Clay_GetElementData(CLAY_ID("PreviewPanel")).boundingBox;
    Clay_BoundingBox scroll = Clay_GetElementData(CLAY_ID("SettingsScroll")).boundingBox;
    fprintf(file,
            "{\"tab\":%d,\"frame\":%llu,\"has_preview\":%s,\"record_preview\":%s,\"command_"
            "pending\":%s,"
            "\"command_queued\":%llu,\"command_completed\":%llu,"
            "\"density\":%.3f,\"input_scale\":%.3f,\"raster_font_size\":%.3f,"
            "\"preview\":[%.1f,%.1f,%.1f,%.1f],"
            "\"preview_panel\":[%.1f,%.1f,%.1f,%.1f],"
            "\"scroll\":[%.1f,%.1f,%.1f,%.1f],\"widgets\":[",
            p->tab, (unsigned long long)p->draw_frame, p->preview ? "true" : "false",
            p->record_preview ? "true" : "false",
            p->snapshot.command_queued > p->snapshot.command_completed ? "true" : "false",
            (unsigned long long)p->snapshot.command_queued,
            (unsigned long long)p->snapshot.command_completed, p->density, p->input_scale,
            TTF_GetFontSize(p->raster_font[1]), preview.x, preview.y, preview.width, preview.height,
            panel.x, panel.y, panel.width, panel.height, scroll.x, scroll.y, scroll.width,
            scroll.height);
    for (int i = 0; i < p->widget_count; i++) {
        const Widget *w = &p->widgets[i];
        const char *key = w->id >= 1000 && w->index >= 0 && (size_t)w->index < FIELD_COUNT
                              ? fields[w->index].key
                              : "";
        fprintf(file,
                "%s{\"id\":%u,\"key\":\"%s\",\"enabled\":%s,"
                "\"box\":[%.1f,%.1f,%.1f,%.1f]}",
                i ? "," : "", w->id, key, w->enabled ? "true" : "false", w->box.x, w->box.y,
                w->box.width, w->box.height);
    }
    fprintf(file,
            "],\"clipboard_text_available\":%s,\"focus\":%u,\"active_text\":%u,\"edit_text\":",
            SDL_HasClipboardText() ? "true" : "false", p->focus, p->active_text);
    int edit_index;
    FieldEdit *edit = active_edit(p, &edit_index);
    json_string(file, edit ? edit->value : "");
    fputs(",\"pause_message\":", file);
    json_string(file, p->snapshot.config.pause_text);
    fputs(",\"error\":", file);
    json_string(file, p->error[0] ? p->error : p->snapshot.error);
    fputs("}\n", file);
    fclose(file);
}

int panel_run(const Config *config, char *error, size_t n)
{
    void *clay_memory = NULL;
    Panel *p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(error, n, "cannot allocate control panel");
        return -1;
    }
    p->snapshot.config = *config;
    p->tab = -1;
    p->density = 1;
    p->input_scale = 1;
    SDL_SetHint(SDL_HINT_APP_ID, "org.cast.Panel");
    SDL_SetHint(SDL_HINT_X11_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE_NORMAL");
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
        snprintf(error, n, "cannot initialize control panel: %s", SDL_GetError());
        cleanup(p, NULL);
        return -1;
    }
    p->window =
        SDL_CreateWindow("cast control panel", 480, 760,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!p->window) {
        snprintf(error, n, "cannot create control panel window: %s", SDL_GetError());
        cleanup(p, NULL);
        return -1;
    }
    p->input_scale = SDL_GetWindowDisplayScale(p->window) / SDL_GetWindowPixelDensity(p->window);
    if (p->input_scale <= 0) {
        p->input_scale = 1;
    }
    SDL_SetWindowSize(p->window, (int)roundf(480 * p->input_scale),
                      (int)roundf(760 * p->input_scale));
    SDL_SetWindowMinimumSize(p->window, (int)roundf(360 * p->input_scale),
                             (int)roundf(640 * p->input_scale));
    p->renderer = SDL_CreateRenderer(p->window, NULL);
    if (!p->renderer) {
        snprintf(error, n, "cannot create control panel renderer: %s", SDL_GetError());
        cleanup(p, NULL);
        return -1;
    }
    SDL_SetRenderVSync(p->renderer, 1);
    SDL_SetRenderDrawBlendMode(p->renderer, SDL_BLENDMODE_BLEND);
    size_t font_length = (size_t)(cast_panel_font_end - cast_panel_font_data);
    for (int i = 0; i < 4; i++) {
        SDL_IOStream *stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        p->font[i] = stream ? TTF_OpenFontIO(stream, true, (float)font_sizes[i]) : NULL;
        stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        p->raster_font[i] = stream ? TTF_OpenFontIO(stream, true, (float)font_sizes[i]) : NULL;
        if (!p->font[i] || !p->raster_font[i]) {
            snprintf(error, n, "cannot open bundled control panel font: %s", SDL_GetError());
            cleanup(p, NULL);
            return -1;
        }
    }
    uint32_t memory_size = Clay_MinMemorySize();
    clay_memory = malloc(memory_size);
    if (!clay_memory) {
        snprintf(error, n, "cannot allocate control panel layout");
        cleanup(p, NULL);
        return -1;
    }
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(memory_size, clay_memory),
                    (Clay_Dimensions){480, 760},
                    (Clay_ErrorHandler){.errorHandlerFunction = clay_error, .userData = p});
    Clay_SetMeasureTextFunction(measure, p);
    p->width = 480;
    p->height = 760;
    /* Creating the renderer can replace the native X11 window for its GL visual.
     * Map only the initialized window, then register its final XID with the daemon. */
    Clay_RenderCommandArray commands = layout(p);
    render(p, commands);
    SDL_RenderPresent(p->renderer);
    if (!SDL_ShowWindow(p->window)) {
        snprintf(error, n, "cannot show control panel window: %s", SDL_GetError());
        cleanup(p, clay_memory);
        return -1;
    }
    p->client = panel_client_open(config, window_xid(p->window), error, n);
    if (!p->client) {
        cleanup(p, clay_memory);
        return -1;
    }
    const char *smoke = getenv("CAST_PANEL_SMOKE_MS"),
               *screenshot = getenv("CAST_PANEL_SCREENSHOT"),
               *ui_state = getenv("CAST_PANEL_UI_STATE");
    uint64_t started = SDL_GetTicks(), deadline = 0;
    if (smoke) {
        char *end;
        unsigned long delay = strtoul(smoke, &end, 10);
        if (!*end && delay >= 100 && delay <= 60000) {
            deadline = started + delay;
        }
    }
    bool captured = false;
    while (!p->quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            event(p, &e);
        }
        int width, height;
        SDL_GetWindowSize(p->window, &width, &height);
        float input_scale =
            SDL_GetWindowDisplayScale(p->window) / SDL_GetWindowPixelDensity(p->window);
        if (input_scale > 0 && fabsf(input_scale - p->input_scale) > .001f) {
            p->input_scale = input_scale;
            SDL_SetWindowMinimumSize(p->window, (int)roundf(360 * input_scale),
                                     (int)roundf(640 * input_scale));
        }
        p->width = width / p->input_scale;
        p->height = height / p->input_scale;
        int pixel_w, pixel_h;
        SDL_GetRenderOutputSize(p->renderer, &pixel_w, &pixel_h);
        float density = pixel_w / p->width;
        if (fabsf(density - p->density) > .001f) {
            p->density = density;
            for (int i = 0; i < 4; i++) {
                TTF_SetFontSize(p->raster_font[i], font_sizes[i] * density);
            }
            for (int i = 0; i < TEXT_CACHE_MAX; i++) {
                SDL_DestroyTexture(p->cache[i].texture);
                free(p->cache[i].text);
                memset(&p->cache[i], 0, sizeof p->cache[i]);
            }
        }
        SDL_SetRenderScale(p->renderer, density, pixel_h / p->height);
        Clay_SetLayoutDimensions((Clay_Dimensions){p->width, p->height});
        Clay_SetPointerState((Clay_Vector2){p->mouse_x, p->mouse_y}, p->mouse_down);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0, p->scroll}, 1.f / 60);
        p->scroll = 0;
        poll_client(p);
        commands = layout(p);
        render(p, commands);
        if (ui_state && p->draw_frame % 4 == 0) {
            write_ui_state(p, ui_state);
        }
        uint64_t now = SDL_GetTicks();
        if (!captured && screenshot &&
            now - started >= (deadline ? (deadline - started) * 3 / 4 : 2000)) {
            SDL_Surface *shot = SDL_RenderReadPixels(p->renderer, NULL);
            if (!shot || !SDL_SavePNG(shot, screenshot)) {
                snprintf(error, n, "cannot capture control panel: %s", SDL_GetError());
                SDL_DestroySurface(shot);
                cleanup(p, clay_memory);
                return -1;
            }
            SDL_DestroySurface(shot);
            captured = true;
        }
        SDL_RenderPresent(p->renderer);
        if (deadline && now >= deadline) {
            p->quit = true;
        }
        SDL_Delay(12);
    }
    cleanup(p, clay_memory);
    return 0;
}
