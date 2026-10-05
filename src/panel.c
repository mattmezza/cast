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
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Cursor
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Small immediate widgets share Clay's layout, clipping, and pointer geometry.
 * The worker transports bounded requests; no socket operation runs on this thread. */
enum {
    TAB_SOURCE,
    TAB_CAMERA,
    TAB_AUDIO,
    TAB_EFFECTS,
    TAB_SETTINGS,
    TAB_STREAM,
    TAB_COUNT
};
#define GROUP_COUNT 6
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
    READ_ONLY = 16,
    STREAM_LOCK = 32,
    REQUIRE_EXCLUSION = 64
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
    FE("stream.service", "Service preset", stream.service, "custom,twitch,youtube", TAB_STREAM, 0,
       STREAM_LOCK),
    FT("stream.server_url", "Server URL (rtmp/rtmps)", stream.server_url, TAB_STREAM, 0,
       STREAM_LOCK),
    FT("stream.key_file", "Stream-key file — path only, never the secret", stream.key_file,
       TAB_STREAM, 0, STREAM_LOCK),
    FI("stream.video_bitrate_kbps", "Video bitrate (kbps)", stream.video_bitrate_kbps, 100, 50000,
       TAB_STREAM, 0, STREAM_LOCK),
    FI("stream.audio_bitrate_kbps", "Audio bitrate (kbps)", stream.audio_bitrate_kbps, 32, 320,
       TAB_STREAM, 0, STREAM_LOCK),
    FE("stream.encoder_preset", "Encoder preset", stream.encoder_preset,
       "ultrafast,superfast,veryfast,faster,fast,medium,slow,slower,veryslow", TAB_STREAM, 0,
       STREAM_LOCK),

    FE("composition.layout", "Layout", layout, "overlay,stage,split,screen,camera", TAB_SOURCE, 0,
       0),
    FB("annotations.stream_keys", "Keystrokes in streaming", annotations_stream_keys, TAB_EFFECTS,
       1, REQUIRE_INPUT),
    FB("annotations.stream_clicks", "Clicks in streaming", annotations_stream_clicks, TAB_EFFECTS,
       1, REQUIRE_INPUT),
    FT("capture.monitor", "Monitor name", monitor, TAB_SOURCE, 0, 0),
    FE("composition.fit", "Screen fit", fit, "contain,cover", TAB_SOURCE, 0, 0),
    FN("zoom.factor", "Zoom factor", zoom_factor, 1, 20, TAB_SOURCE, 0, 0),
    FB("zoom.follow", "Follow pointer", zoom_follow, TAB_SOURCE, 0, REQUIRE_CURSOR),
    FE("capture.exclusion", "Panel / preview overlap", capture_exclusion, "mask,transparent",
       TAB_SOURCE, 1, REQUIRE_EXCLUSION),
    FC("capture.mask_color", "Overlap mask color", capture_mask_color, TAB_SOURCE, 1,
       REQUIRE_EXCLUSION),
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
    FN("screen.width_percent", "Screen width (%)", screen_width_percent, 1, 100, TAB_SOURCE, 3, 0),
    FN("camera.width_percent", "Camera width (%)", camera_width_percent, 1, 100, TAB_SOURCE, 3, 0),
    FI("screen.margin", "Screen margin (px)", screen_margin, 0, 4096, TAB_SOURCE, 4, 0),
    FI("screen.radius", "Screen corner radius (px)", screen_radius, 0, 2048, TAB_SOURCE, 4, 0),
    FI("screen.border_width", "Screen border width (px)", screen_border_width, 0, 256, TAB_SOURCE,
       4, 0),
    FC("screen.border_color", "Screen border color", screen_border_color, TAB_SOURCE, 4, 0),
    FE("screen.background", "Screen background", screen_background, "blurred,gradient,solid",
       TAB_SOURCE, 4, 0),
    FC("screen.background_color", "Screen background color", screen_background_color, TAB_SOURCE, 4,
       0),
    FI("screen.background_blur_radius", "Screen background blur (px)",
       screen_background_blur_radius, 1, 128, TAB_SOURCE, 4, 0),
    FN("screen.background_brightness", "Screen background brightness", screen_background_brightness,
       0, 1, TAB_SOURCE, 4, 0),
    FE("background.source", "Blurred backdrop source", background_source, "screen,camera",
       TAB_SOURCE, 5, 0),
    FC("background.gradient_from", "Gradient start color", gradient_from, TAB_SOURCE, 5, 0),
    FB("background.gradient_via_enabled", "Middle gradient stop", gradient_via_enabled, TAB_SOURCE,
       5, 0),
    FC("background.gradient_via", "Gradient middle color", gradient_via, TAB_SOURCE, 5, 0),
    FC("background.gradient_to", "Gradient end color", gradient_to, TAB_SOURCE, 5, 0),
    FN("background.gradient_angle", "Gradient angle (degrees)", gradient_angle, 0, 360, TAB_SOURCE,
       5, 0),
    FN("background.gradient_waypoint", "Middle stop position (%)", gradient_waypoint, 1, 99,
       TAB_SOURCE, 5, 0),
    FB("camera.enabled", "Camera enabled", camera_enabled, TAB_CAMERA, 0, 0),
    FB("camera.visible", "Visible in composition", camera_visible, TAB_CAMERA, 0, 0),
    FT("camera.device", "Camera device", camera_device, TAB_CAMERA, 3, 0),
    FN("camera.width_percent", "Camera width (%)", camera_width_percent, 1, 100, TAB_CAMERA, 0, 0),
    FE("camera.shape", "Shape", shape, "rectangle,rounded,circle", TAB_CAMERA, 0, 0),
    FE("camera.anchor", "Anchor", anchor,
       "top-left,top,top-right,left,right,bottom-left,bottom,bottom-right,free", TAB_CAMERA, 0, 0),
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
    FE("camera.background", "Background", camera_background, "blurred,gradient,solid", TAB_CAMERA,
       2, 0),
    FC("camera.background_color", "Background color", camera_background_color, TAB_CAMERA, 2, 0),
    FI("camera.background_blur_radius", "Background blur (px)", camera_background_blur_radius, 1,
       128, TAB_CAMERA, 2, 0),
    FN("camera.background_brightness", "Background brightness (0–1)", camera_background_brightness,
       0, 1, TAB_CAMERA, 2, 0),
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
    FB("annotations.virtual_keys", "Keystrokes in virtual camera", annotations_virtual_keys,
       TAB_EFFECTS, 1, REQUIRE_INPUT),
    FB("annotations.virtual_clicks", "Clicks in virtual camera", annotations_virtual_clicks,
       TAB_EFFECTS, 1, REQUIRE_INPUT),
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
    FB("logo.enabled", "Logo enabled", logo_enabled, TAB_EFFECTS, 4, 0),
    FT("logo.path", "Logo image path", logo_path, TAB_EFFECTS, 4, 0),
    FE("logo.anchor", "Logo anchor", logo_anchor,
       "top-left,top,top-right,left,right,bottom-left,bottom,bottom-right", TAB_EFFECTS, 4, 0),
    FN("logo.width_percent", "Logo width (%)", logo_width_percent, 1, 100, TAB_EFFECTS, 4, 0),
    FI("logo.margin_x", "Logo X inset (px)", logo_margin_x, 0, 7680, TAB_EFFECTS, 4, 0),
    FI("logo.margin_y", "Logo Y inset (px)", logo_margin_y, 0, 4320, TAB_EFFECTS, 4, 0),
    FN("logo.opacity", "Logo opacity (0–1)", logo_opacity, 0, 1, TAB_EFFECTS, 4, 0),
    FB("text.enabled", "Text enabled", text_enabled, TAB_EFFECTS, 5, 0),
    FT("text.content", "Text content", text_content, TAB_EFFECTS, 5, 0),
    FT("text.font", "Text font", text_font, TAB_EFFECTS, 5, 0),
    FI("text.size", "Text size (px)", text_size, 8, 256, TAB_EFFECTS, 5, 0),
    FC("text.color", "Text color", text_color, TAB_EFFECTS, 5, 0),
    FE("text.anchor", "Text anchor", text_anchor,
       "top-left,top,top-right,left,right,bottom-left,bottom,bottom-right", TAB_EFFECTS, 5, 0),
    FI("text.margin_x", "Text X inset (px)", text_margin_x, 0, 7680, TAB_EFFECTS, 5, 0),
    FI("text.margin_y", "Text Y inset (px)", text_margin_y, 0, 4320, TAB_EFFECTS, 5, 0),
    FN("text.opacity", "Text opacity (0–1)", text_opacity, 0, 1, TAB_EFFECTS, 5, 0),
    FT("output.pause_text", "Pause title", pause_text, TAB_SETTINGS, 4, 0),
    FT("output.pause_subtitle", "Pause subtitle", pause_subtitle, TAB_SETTINGS, 4, 0),
    FT("output.pause_footer", "Pause footer", pause_footer, TAB_SETTINGS, 4, 0),
    FC("output.pause_color", "Pause background", pause_color, TAB_SETTINGS, 4, 0),
    FC("output.pause_foreground", "Pause text color", pause_foreground, TAB_SETTINGS, 4, 0),
    FI("output.pause_title_size", "Pause title size (px)", pause_title_size, 8, 256, TAB_SETTINGS,
       4, 0),
    FI("output.pause_subtitle_size", "Pause subtitle size (px)", pause_subtitle_size, 8, 256,
       TAB_SETTINGS, 4, 0),
    FI("output.pause_footer_size", "Pause footer size (px)", pause_footer_size, 8, 256,
       TAB_SETTINGS, 4, 0),
    FI("output.pause_text_gap", "Pause text gap (px)", pause_text_gap, 0, 512, TAB_SETTINGS, 4, 0),
    FT("output.blur_title", "Blur title", blur_title, TAB_SETTINGS, 5, 0),
    FT("output.blur_subtitle", "Blur subtitle", blur_subtitle, TAB_SETTINGS, 5, 0),
    FT("output.blur_footer", "Blur footer", blur_footer, TAB_SETTINGS, 5, 0),
    FC("output.blur_color", "Blur tint", blur_color, TAB_SETTINGS, 5, 0),
    FC("output.blur_foreground", "Blur text color", blur_foreground, TAB_SETTINGS, 5, 0),
    FI("output.blur_radius", "Blur radius (px)", blur_radius, 1, 128, TAB_SETTINGS, 5, 0),
    FN("output.blur_opacity", "Blur tint opacity (0–1)", blur_opacity, 0, 1, TAB_SETTINGS, 5, 0),
    FI("output.blur_title_size", "Blur title size (px)", blur_title_size, 8, 256, TAB_SETTINGS, 5,
       0),
    FI("output.blur_subtitle_size", "Blur subtitle size (px)", blur_subtitle_size, 8, 256,
       TAB_SETTINGS, 5, 0),
    FI("output.blur_footer_size", "Blur footer size (px)", blur_footer_size, 8, 256, TAB_SETTINGS,
       5, 0),
    FI("output.blur_text_gap", "Blur text gap (px)", blur_text_gap, 0, 512, TAB_SETTINGS, 5, 0),
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
    FB("output.enabled", "Virtual camera at start", virtual_enabled, TAB_SETTINGS, 3, READ_ONLY),
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
    W_OPTION,
    W_LINK
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
    A_DISMISS,
    A_MAIN_TAB,
    A_LANE,
    A_SECTION,
    A_REVERT,
    A_APPLY_SECTION,
    A_SETUP,
    A_BACK,
    A_CLOSE
} Action;
typedef enum {
    ICON_NONE,
    ICON_BACK,
    ICON_FORWARD,
    ICON_PLAY,
    ICON_PAUSE,
    ICON_STOP,
    ICON_RECORD,
    ICON_CLOSE,
    ICON_FREEZE,
    ICON_BLUR,
    ICON_CUT,
    ICON_SCREEN,
    ICON_CAMERA,
    ICON_AUDIO,
    ICON_EFFECTS,
    ICON_SETTINGS,
    ICON_RESET,
    ICON_PLUS,
    ICON_MINUS,
    ICON_STREAM,
    ICON_INFO,
    ICON_CHEVRON,
    ICON_DOT
} Icon;
#define ICON_DATA_BASE 4096
typedef struct {
    uint32_t id;
    WidgetType type;
    Action action;
    bool enabled;
    int index, auxiliary;
    bool draft;
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
typedef enum {
    VIEW_OPERATE,
    VIEW_COMPOSE,
    VIEW_SECTION,
    VIEW_SETUP
} PanelView;
#define VIEW_SCROLL_COUNT 10
typedef struct {
    uint32_t magic;
    uint64_t session_cookie;
    PanelView view, return_view;
    int section, lane, return_section;
    float scroll[VIEW_SCROLL_COUNT];
} NavigationMemory;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font[5], *raster_font[5];
    float density, input_scale;
    PanelClient *client;
    PanelSnapshot snapshot;
    FieldEdit edit[FIELD_COUNT];
    Widget widgets[WIDGET_MAX];
    int widget_count, tab;
    int main_tab, open_lane, open_section;
    bool stream_setup;
    PanelView view, return_view;
    int return_section;
    float view_scroll[VIEW_SCROLL_COUNT];
    bool restore_scroll;
    uint64_t navigation_generation, navigation_attachment;
    NavigationMemory *navigation;
    bool groups[TAB_COUNT][GROUP_COUNT], quit, mouse_down, click;
    uint32_t focus, active_text, dropdown;
    int dropdown_field, dropdown_choice;
    bool select_all;
    size_t caret;
    float mouse_x, mouse_y, scroll;
    float width, height;
    char error[CAST_ERR], reply[CAST_ERR];
    uint64_t command_seen, draw_frame;
    bool draft_context;
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
static const Clay_BorderWidth outline_width = {.left = 1, .right = 1, .top = 1, .bottom = 1};
static const int font_sizes[] = {13, 16, 19, 25, 48};

static Clay_String literal(const char *s)
{
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}
/* These two compact roles reuse Inter's existing secondary slot. They are
 * rasterized at their actual density/size, rather than shrinking cached text. */
#define FONT_QUIET 100
#define FONT_META 200
static TTF_Font *text_font(Panel *p, int key, bool raster)
{
    int slot = key < 5 ? key : 0;
    TTF_Font *font = raster ? p->raster_font[slot] : p->font[slot];
    float size = key == FONT_QUIET ? 11 : key == FONT_META ? 11.5f : font_sizes[slot];
    TTF_SetFontSize(font, size * (raster ? p->density : 1));
    TTF_SetFontStyle(font, key == FONT_META || slot == 2 || slot == 3 ? TTF_STYLE_BOLD
                                                                      : TTF_STYLE_NORMAL);
    return font;
}
static int scroll_index(const Panel *p)
{
    return p->view == VIEW_OPERATE   ? 0
           : p->view == VIEW_COMPOSE ? 1
           : p->view == VIEW_SETUP   ? 9
                                     : 2 + p->open_section;
}
static void remember_scroll(Panel *p)
{
    Clay_ScrollContainerData body = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    if (body.found && !p->restore_scroll) {
        p->view_scroll[scroll_index(p)] = body.scrollPosition->y;
    }
}
static void navigate_view(Panel *p, PanelView view, int section)
{
    remember_scroll(p);
    p->view = view;
    p->main_tab = view == VIEW_SETUP ? p->return_view != VIEW_OPERATE
                                     : view == VIEW_COMPOSE || view == VIEW_SECTION;
    p->open_section = view == VIEW_SECTION ? section : -1;
    p->stream_setup = view == VIEW_SETUP;
    p->dropdown = 0;
    p->restore_scroll = true;
}
/* POSIX shared memory keeps only navigation numbers across panel processes.
 * It is session memory, reset for each daemon socket session; no config, draft,
 * media, secret or daemon IPC field is persisted here. */
static void open_navigation(Panel *p, const Config *config)
{
    unsigned long hash = 5381;
    for (const unsigned char *c = (const unsigned char *)config->socket_path; *c; c++) {
        hash = hash * 33 + *c;
    }
    char name[96];
    snprintf(name, sizeof name, "/cast-panel-view-%lu-%lx", (unsigned long)getuid(), hash);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 0777) != 0600 ||
        (st.st_size != 0 && st.st_size != sizeof(NavigationMemory)) ||
        (!st.st_size && ftruncate(fd, sizeof(NavigationMemory)))) {
        close(fd);
        return;
    }
    void *memory = mmap(NULL, sizeof(NavigationMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory != MAP_FAILED) {
        p->navigation = memory;
    }
}
static void save_navigation(Panel *p)
{
    if (!p->navigation || !p->navigation_generation) {
        return;
    }
    remember_scroll(p);
    NavigationMemory saved = {.magic = 0x43565032,
                              .session_cookie = p->navigation_generation,
                              .view = p->view,
                              .section = p->open_section,
                              .lane = p->open_lane,
                              .return_view = p->return_view,
                              .return_section = p->return_section};
    memcpy(saved.scroll, p->view_scroll, sizeof saved.scroll);
    *p->navigation = saved;
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
static void quiet_label(const char *text, Clay_Color ink, bool semibold)
{
    CLAY_TEXT(literal(text),
              CLAY_TEXT_CONFIG({.fontId = 0,
                                .fontSize = semibold ? 12 : 11,
                                .userData = (void *)(uintptr_t)(semibold ? FONT_META : FONT_QUIET),
                                .textColor = ink,
                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
}
static const char *fit_text(Panel *p, const char *text, float available, int key)
{
    int width, height;
    TTF_Font *font = text_font(p, key, false);
    size_t n = strcspn(text, "\n");
    TTF_GetStringSize(font, text, n, &width, &height);
    if ((float)width <= available && n == strlen(text)) {
        return text;
    }
    if (available < 8) {
        return "";
    }
    n = n > 480 ? 480 : n;
    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) {
        n--;
    }
    int dots;
    TTF_GetStringSize(font, "…", strlen("…"), &dots, &height);
    while (n) {
        TTF_GetStringSize(font, text, n, &width, &height);
        if (width + dots <= available) {
            break;
        }
        n--;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    return format(p, "%.*s…", (int)n, text);
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
static void icon_slot(Icon icon, bool enabled, bool selected)
{
    uintptr_t data = ICON_DATA_BASE + (uintptr_t)icon + (!enabled ? 256 : 0) + (selected ? 512 : 0);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(16), .height = CLAY_SIZING_FIXED(16)}},
          .custom = {.customData = (void *)data}})
    {
    }
}
/* Status dots are local UI geometry; every dot also has an explicit text state. */
static void state_marker(Clay_Color ink)
{
    uintptr_t data = ICON_DATA_BASE + ICON_DOT + ((uintptr_t)(unsigned)ink.r << 40) +
                     ((uintptr_t)(unsigned)ink.g << 32) + ((uintptr_t)(unsigned)ink.b << 24);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(7), .height = CLAY_SIZING_FIXED(7)}},
          .custom = {.customData = (void *)data}})
    {
    }
}
static Icon button_icon(const Panel *p, uint32_t id, Action action)
{
    if (id == 99 || id == 90 || id == 92 || id == 93 || id == 94 || id == 96 || id == 87) {
        return ICON_BACK;
    }
    if (id == 22) {
        return ICON_SCREEN;
    }
    if (id == 130) {
        return p->snapshot.stream.active && !p->snapshot.state.stream_paused ? ICON_PAUSE
                                                                             : ICON_PLAY;
    }
    if (id >= 139 && id <= 141) {
        return ICON_STOP;
    }
    if (id == 137) {
        return ICON_FREEZE;
    }
    if (id == 138) {
        return ICON_BLUR;
    }
    if (id == 150) {
        return ICON_SETTINGS;
    }
    if (id == 30) {
        return p->snapshot.state.virtual_paused ? ICON_PLAY : ICON_PAUSE;
    }
    if (id == 31 || id == 37) {
        return ICON_FREEZE;
    }
    if (id == 32) {
        return p->snapshot.countdown && !p->snapshot.state.recording ? ICON_CLOSE
               : p->snapshot.state.recording                         ? ICON_STOP
                                                                     : ICON_RECORD;
    }
    if (id == 33) {
        return p->snapshot.state.record_paused || p->snapshot.state.record_cut ? ICON_PLAY
                                                                               : ICON_PAUSE;
    }
    if (id == 34) {
        return p->snapshot.state.group_paused ? ICON_PLAY : ICON_PAUSE;
    }
    if (id == 35) {
        return ICON_CLOSE;
    }
    if (id == 36 || id == 38) {
        return ICON_BLUR;
    }
    if (id == 39) {
        return ICON_CUT;
    }
    if (id == 41 || id == 74 || id == 91 || id == 95 || id == 97 || id == 88) {
        return ICON_FORWARD;
    }
    if (id == 42 || id == 78) {
        return ICON_RESET;
    }
    if (id == 76) {
        return ICON_MINUS;
    }
    if (id == 77) {
        return ICON_PLUS;
    }
    if (id >= 70 && id <= 75) {
        return ICON_SCREEN;
    }
    if (id == 80) {
        return ICON_CAMERA;
    }
    if (id == 83) {
        return ICON_AUDIO;
    }
    if (id == 84) {
        return ICON_CLOSE;
    }
    if (id == 85) {
        return ICON_RESET;
    }
    return action == A_COMMAND ? ICON_SETTINGS : ICON_NONE;
}
static void button(Panel *p, uint32_t id, const char *text, bool enabled, bool selected,
                   Action action, int index)
{
    if (action != A_TAB && action != A_GROUP && action != A_PREVIEW && action != A_DROPDOWN &&
        action != A_BACK && p->snapshot.command_queued > p->snapshot.command_completed) {
        enabled = false;
    }
    Widget *w = widget(p, id, W_BUTTON, action, enabled);
    if (w) {
        w->index = index;
    }
    Clay_Color bg = selected && enabled  ? (Clay_Color){35, 63, 83, 255}
                    : hot(id) && enabled ? hovered
                    : id == 99           ? background
                                         : control;
    Clay_Color ink = enabled ? selected ? accent : foreground : muted;
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = (id == 30 || id == 32 || id == 130 || id == 34 || id == 22)
                                             ? CLAY_SIZING_GROW()
                                             : CLAY_SIZING_FIT(),
                                .height = CLAY_SIZING_FIXED(id == 22 || id == 34 ? 30 : 40)},
                     .padding = {10, action == A_DROPDOWN ? 26 : 10, id == 22 || id == 34 ? 6 : 8,
                                 id == 22 || id == 34 ? 6 : 8},
                     .childGap = 6,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .border = {.color = p->focus == id ? accent
                              : selected     ? (Clay_Color){60, 104, 134, 255}
                                             : bg,
                     .width = outline_width}})
    {
        Icon icon = button_icon(p, id, action);
        if (icon != ICON_NONE) {
            icon_slot(icon, enabled, selected);
        }
        if (id == 22 || id == 34) {
            quiet_label(text, ink, true);
        } else {
            label(text, 1, ink);
        }
    }
}
static void command_button(Panel *p, uint32_t id, const char *text, bool enabled, const char *a,
                           const char *b, const char *c)
{
    const State *s = &p->snapshot.state;
    bool selected =
        (id == 22 && (p->snapshot.config.preview ||
                      (p->snapshot.countdown && p->snapshot.capabilities.preview))) ||
        (id == 31 && s->virtual_frozen) || (id == 36 && s->virtual_blurred) ||
        (id == 37 && s->record_frozen) || (id == 38 && s->record_blurred) ||
        (id == 39 && s->record_cut) ||
        (id == 30 && (!p->snapshot.config.virtual_enabled || s->virtual_paused)) ||
        (id == 32 && !s->recording && !p->snapshot.countdown && !p->snapshot.finalizing) ||
        (id == 33 && (s->record_paused || s->record_cut)) ||
        (id == 130 && (!p->snapshot.stream.active || s->stream_paused)) ||
        (id == 137 && s->stream_frozen) || (id == 138 && s->stream_blurred);
    button(p, id, text, enabled, selected, A_COMMAND, 0);
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
           (!(f->flags & REQUIRE_REGION) || p->snapshot.capabilities.region_selection) &&
           (!(f->flags & REQUIRE_EXCLUSION) || p->snapshot.capabilities.panel_exclusion);
}
static bool writable(const Panel *p, const FieldSpec *f)
{
    return p->snapshot.connected && supported(p, f) && !(f->flags & READ_ONLY) &&
           (!(f->flags & STREAM_LOCK) || !p->snapshot.state.stream_active) &&
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
static void text_wrapped(const char *, Clay_Color);
static void field_row(Panel *p, size_t index)
{
    const FieldSpec *f = &fields[index];
    bool enabled = writable(p, f);
    uint32_t id = 1000 + (uint32_t)index * 3;
    char current[PATH_MAX];
    field_value(f, &p->snapshot.config, current, sizeof current);
    if (!p->edit[index].dirty && p->active_text != id) {
        snprintf(p->edit[index].value, sizeof p->edit[index].value, "%s", current);
    }
    if (p->edit[index].dirty) {
        snprintf(current, sizeof current, "%s", p->edit[index].value);
    }
    bool stacked = f->type != FIELD_BOOL && f->type != FIELD_ENUM;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIT()},
                     .padding = {0, 0, 5, 5},
                     .childGap = 8,
                     .layoutDirection = stacked ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .border = {.color = line, .width = {.bottom = 1}}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
        {
            label(f->label, stacked ? 0 : 1, stacked ? muted : secondary);
        }
        CLAY({.layout = {.sizing = {.width = stacked ? CLAY_SIZING_GROW() : CLAY_SIZING_FIT()},
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            if (f->type == FIELD_BOOL) {
                bool on = !strcmp(current, "true");
                button(p, id, on ? "On" : "Off", enabled, on, A_SETTING, (int)index);
                Widget *w = &p->widgets[p->widget_count - 1];
                w->draft = p->draft_context;
                snprintf(w->value, sizeof w->value, "%s", on ? "false" : "true");
            } else if (f->type == FIELD_ENUM) {
                button(p, id, format(p, "%s", current), enabled, false, A_DROPDOWN, (int)index);
                p->widgets[p->widget_count - 1].type = W_SELECT;
                p->widgets[p->widget_count - 1].draft = p->draft_context;
            } else {
                Widget *w = widget(p, id, W_FIELD, A_FIELD, enabled);
                if (w) {
                    w->index = (int)index;
                    w->draft = p->draft_context;
                }
                CLAY({.id = element_id(id),
                      .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                            .height = CLAY_SIZING_FIXED(40)}},
                      .backgroundColor = p->active_text == id ? surface : background,

                      .border = {.color = p->focus == id ? accent : line, .width = outline_width},
                      .custom = {.customData = (void *)(uintptr_t)(index + 1)}})
                {
                }
                if (f->flags & READ_ONLY) {
                    label("Restart", 0, secondary);
                } else if (p->edit[index].dirty && !p->draft_context) {
                    button(p, id + 1, "Apply", enabled, false, A_APPLY, (int)index);
                }
            }
        }
        if (!supported(p, f)) {
            text_wrapped("Unavailable with this capture backend.", secondary);
        }
        if ((f->flags & STREAM_LOCK) && !enabled && p->snapshot.state.stream_active) {
            text_wrapped("Stop streaming first; presentation styling stays editable.", secondary);
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
/* The header and tabs never scroll. Both bodies use one retained scroll container. */
static const char *const lane_names[] = {"Virtual camera", "Recording", "Streaming", "Audio"};
static const char *const section_names[] = {
    "Source & layout", "Camera",  "Background & stage", "Overlays", "Annotations & pointer",
    "Audio",           "Settings"};
static const char *virtual_status(const PanelSnapshot *s)
{
    return !s->config.virtual_enabled ? "Off"
           : s->state.virtual_paused  ? "Paused"
           : s->state.virtual_blurred ? "Blurred"
           : s->state.virtual_frozen  ? "Frozen"
                                      : "Running";
}
static const char *record_status(const PanelSnapshot *s)
{
    return s->countdown              ? "Countdown"
           : s->finalizing           ? "Finalizing…"
           : !s->state.recording     ? "Off"
           : s->state.record_cut     ? "Cut"
           : s->state.record_paused  ? "Paused"
           : s->state.record_blurred ? "Blurred"
           : s->state.record_frozen  ? "Frozen"
                                     : "Running";
}
static const char *panel_stream_status(const PanelSnapshot *s)
{
    switch (s->stream.state) {
    case STREAM_CONNECTING:
        return "Connecting";
    case STREAM_RECONNECTING:
        return "Retrying";
    case STREAM_STOPPING:
        return "Stopping";
    case STREAM_FAILED:
        return "Failed";
    case STREAM_STREAMING:
        return s->state.stream_paused    ? "Paused"
               : s->state.stream_blurred ? "Blurred"
               : s->state.stream_frozen  ? "Frozen"
                                         : "Running";
    case STREAM_STOPPED:
        return s->config.stream.server_url[0] && s->config.stream.key_file[0] ? "Ready"
                                                                              : "Not configured";
    }
    return "Off";
}
static const char *lane_status(Panel *p, int lane)
{
    const PanelSnapshot *s = &p->snapshot;
    return lane == 0   ? virtual_status(s)
           : lane == 1 ? record_status(s)
           : lane == 2 ? panel_stream_status(s)
                       : format(p, "Mic %s · desktop %s", s->config.mic ? "on" : "off",
                                s->config.desktop ? "on" : "off");
}
static Clay_Color state_color(const PanelSnapshot *s, int lane)
{
    if (lane == 2 && s->stream.state == STREAM_FAILED) {
        return danger;
    }
    if ((lane == 1 && (s->countdown || s->finalizing)) ||
        (lane == 2 &&
         (s->stream.state == STREAM_CONNECTING || s->stream.state == STREAM_RECONNECTING))) {
        return accent;
    }
    bool active = lane == 0   ? s->config.virtual_enabled
                  : lane == 1 ? s->state.recording
                              : s->stream.active;
    bool paused = lane == 0   ? s->state.virtual_paused
                  : lane == 1 ? s->state.record_paused || s->state.record_cut
                              : s->state.stream_paused;
    return !active  ? secondary
           : paused ? (Clay_Color){224, 197, 102, 255}
                    : (Clay_Color){131, 221, 182, 255};
}
static void text_button(Panel *p, uint32_t id, const char *text, bool enabled, Action action,
                        int index, bool selected)
{
    Widget *w = widget(p, id, W_BUTTON, action, enabled);
    if (w) {
        w->index = index;
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(42)},
                     .padding = {0, 0, 8, 8},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = hot(id) ? surface : background,
          .border = {.color = p->focus == id ? accent
                              : selected     ? accent
                                             : line,
                     .width = p->focus == id ? outline_width
                                             : (Clay_BorderWidth){.bottom = selected ? 2 : 1}}})
    {
        label(text, 1, enabled ? foreground : muted);
    }
}
static void pinned_header(Panel *p)
{
    const PanelSnapshot *s = &p->snapshot;
    float available = fminf(p->width, 520) - 32;
    float share = (available - 12) / 3;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 8, 0},
                     .childGap = 8,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(44)},
                         .childGap = 6}})
        {
            for (int lane = 0; lane < 3; lane++) {
                uint32_t id = 10 + (uint32_t)lane;
                Widget *w = widget(p, id, W_BUTTON, A_LANE, true);
                if (w) {
                    w->index = lane;
                }
                CLAY({.id = element_id(id),
                      .layout = {.sizing = {.width = CLAY_SIZING_FIXED(share),
                                            .height = CLAY_SIZING_FIXED(44)},
                                 .padding = {6, 6, 4, 4},
                                 .childGap = 2,
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
                      .backgroundColor = hot(id)                                    ? hovered
                                         : p->main_tab == 0 && p->open_lane == lane ? surface
                                                                                    : control,
                      .border = {.color = p->focus == id ? accent : control,
                                 .width = outline_width}})
                {
                    label(fit_text(p, lane_names[lane], share - 12, 0), 0,
                          hot(id) ? secondary : muted);
                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(share - 12)},
                                     .childGap = 5,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
                    {
                        state_marker(state_color(s, lane));
                        label(fit_text(p, lane_status(p, lane), share - 24, 0), 0,
                              state_color(s, lane));
                    }
                }
            }
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(30)},
                         .childGap = 6}})
        {
            command_button(p, 34, s->state.group_paused ? "Resume" : "Pause all", s->connected,
                           s->state.group_paused ? "resume" : "pause", NULL, NULL);
            command_button(p, 22,
                           s->countdown        ? "Countdown"
                           : s->config.preview ? "Preview on"
                                               : "Preview off",
                           s->connected && s->capabilities.preview && !s->countdown, "preview",
                           s->config.preview ? "off" : "on", NULL);
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                         .padding = {10, 10, 7, 7},
                         .childGap = 7},
              .backgroundColor = surface})
        {
            icon_slot(ICON_INFO, true, false);
            text_wrapped(s->exclusion[0]
                             ? s->exclusion
                             : "Capture exclusion status is unavailable while disconnected.",
                         muted);
        }
    }
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {0, 0, 10, 0}},
          .border = {.color = line, .width = {.bottom = 1}}})
    {
        text_button(p, 5, "Operate", true, A_MAIN_TAB, 0, p->main_tab == 0);
        text_button(p, 6, "Compose", true, A_MAIN_TAB, 1, p->main_tab == 1);
        /* Closing is local and remains available while disconnected or waiting
         * for a daemon command. Keep the two output-action rows unchanged. */
        widget(p, 7, W_BUTTON, A_CLOSE, true);
        CLAY({.id = element_id(7),
              .layout = {.sizing = {.width = CLAY_SIZING_FIXED(76),
                                    .height = CLAY_SIZING_FIXED(42)},
                         .childGap = 5,
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                            .y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = hot(7) ? hovered : background,
              .border = {.color = p->focus == 7 ? accent : line,
                         .width = p->focus == 7 ? outline_width
                                               : (Clay_BorderWidth){.bottom = 1}}})
        {
            icon_slot(ICON_CLOSE, true, false);
            quiet_label("Close", foreground, true);
        }
    }
}
static void disclosure(Panel *p, uint32_t id, const char *title, const char *detail, Action action,
                       int index, bool open)
{
    Widget *w = widget(p, id, W_BUTTON, action, true);
    if (w) {
        w->index = index;
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 12, 12},
                     .childGap = 8,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = hot(id) ? surface : background,
          .border = {.color = p->focus == id ? accent : line,
                     .width = p->focus == id ? outline_width : (Clay_BorderWidth){.bottom = 1}}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 4}})
        {
            label(title, action == A_LANE ? 3 : 2, foreground);
            if (action != A_LANE && detail && detail[0]) {
                text_wrapped(detail, secondary);
            }
        }
        if (action == A_LANE && detail && detail[0]) {
            label(detail, 0, index < 3 ? state_color(&p->snapshot, index) : secondary);
        }
        icon_slot(open ? ICON_CHEVRON : ICON_FORWARD, true, false);
    }
}
static int find_field(const char *key)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (!strcmp(fields[i].key, key)) {
            return (int)i;
        }
    }
    return -1;
}
static void named_field(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index >= 0) {
        field_row(p, (size_t)index);
    }
}
static void lane_annotations(Panel *p, int lane)
{
    const char *names[] = {"virtual", "record", "stream"};
    p->draft_context = false;
    named_field(p, format(p, "annotations.%s_keys", names[lane]));
    named_field(p, format(p, "annotations.%s_clicks", names[lane]));
}
static void effect_buttons(Panel *p, int lane)
{
    const State *s = &p->snapshot.state;
    const char *commands[] = {"virtual", "record", "stream"};
    bool frozen = lane == 0 ? s->virtual_frozen : lane == 1 ? s->record_frozen : s->stream_frozen;
    bool blurred = lane == 0   ? s->virtual_blurred
                   : lane == 1 ? s->record_blurred
                               : s->stream_blurred;
    uint32_t freeze_id = lane == 0 ? 31 : lane == 1 ? 37 : 137;
    uint32_t blur_id = lane == 0 ? 36 : lane == 1 ? 38 : 138;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
    {
        command_button(p, freeze_id, frozen ? "Unfreeze" : "Freeze", p->snapshot.connected,
                       commands[lane], frozen ? "unfreeze" : "freeze", NULL);
        command_button(p, blur_id, blurred ? "Blur off" : "Blur", p->snapshot.connected,
                       commands[lane], "blur", blurred ? "off" : "on");
        command_button(p, 139 + (uint32_t)lane, "Stop", p->snapshot.connected, commands[lane],
                       "stop", NULL);
    }
}
static void lane_body(Panel *p, int lane)
{
    const PanelSnapshot *s = &p->snapshot;
    bool connected = s->connected;
    if (lane == 0) {
        text_wrapped(
            !s->config.virtual_enabled ? "Starts privacy-paused — Resume reveals the composition."
            : s->state.virtual_paused
                ? "Privacy pause: solid screen, silence. Freeze/blur remembered underneath."
            : s->state.virtual_blurred
                ? "Blurred above the composition · silence. Conference sees this tile."
            : s->state.virtual_frozen ? "Frozen — held frame, silence. Conference sees this tile."
                                      : "Transmitting the composition. Conference sees this tile.",
            muted);
        command_button(p, 30,
                       !s->config.virtual_enabled ? "Start virtual camera"
                       : s->state.virtual_paused  ? "Resume virtual camera"
                                                  : "Pause virtual camera",
                       connected, "virtual",
                       !s->config.virtual_enabled ? "start"
                       : s->state.virtual_paused  ? "resume"
                                                  : "pause",
                       NULL);
        if (s->config.virtual_enabled) {
            effect_buttons(p, lane);
            lane_annotations(p, lane);
        }
    } else if (lane == 1) {
        if (s->countdown) {
            unsigned seconds =
                (unsigned)((s->countdown_remaining_ns + 999999999ULL) / 1000000000ULL);
            text_wrapped(s->state.recording ? "Resuming the same file — Cancel available."
                                            : "Recording starts — Cancel available.",
                         muted);
            command_button(p, 35, format(p, "Cancel — %u s", seconds), connected, "record",
                           "cancel", NULL);
        } else if (s->finalizing) {
            text_wrapped("Closing the recording file.", muted);
            command_button(p, 32, "Finalizing…", false, "record", "start", NULL);
        } else if (!s->state.recording) {
            text_wrapped("Start begins the configured countdown; cut removes media time.", muted);
            command_button(p, 32, "Start recording", connected, "record", "start", NULL);
        } else {
            uint64_t seconds = s->duration_ns / 1000000000ULL;
            text_wrapped(format(p, "%02llu:%02llu · %s", (unsigned long long)(seconds / 60),
                                (unsigned long long)(seconds % 60), s->state.record_path),
                         muted);
            if (s->state.record_cut) {
                text_wrapped("Media clock stopped · file kept. Resume continues the same file.",
                             muted);
                command_button(p, 33,
                               format(p, "Resume — %d s countdown", s->config.record_countdown),
                               connected, "record", "resume", NULL);
                command_button(p, 32, "Stop recording", connected, "record", "stop", NULL);
            } else {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
                {
                    command_button(p, 33, s->state.record_paused ? "Resume" : "Pause", connected,
                                   "record", s->state.record_paused ? "resume" : "pause", NULL);
                    command_button(p, 39, "Cut time", connected, "record", "cut", NULL);
                }
                effect_buttons(p, lane);
                text_wrapped("Pause writes a styled screen and silence. Cut removes media time.",
                             muted);
            }
        }
        lane_annotations(p, lane);
        named_field(p, "record.countdown");
    } else if (lane == 2) {
        if (s->stream.active) {
            const char *destination = !strcmp(s->config.stream.service, "custom")
                                          ? "Custom RTMP destination"
                                          : s->config.stream.service;
            text_wrapped(format(p, "%s · mic %s · desktop %s", destination,
                                s->config.mic ? "on" : "off", s->config.desktop ? "on" : "off"),
                         muted);
            if (s->stream.state == STREAM_RECONNECTING) {
                text_wrapped(format(p, "Retrying %u · current privacy state retained.",
                                    s->stream.retry_attempt),
                             accent);
            }
            if (s->state.stream_paused) {
                text_wrapped("Connected sessions start privacy-paused. Resume streaming reveals "
                             "the composition to viewers.",
                             secondary);
            }
            command_button(p, 130, s->state.stream_paused ? "Resume streaming" : "Pause streaming",
                           connected && s->stream.state == STREAM_STREAMING, "stream",
                           s->state.stream_paused ? "resume" : "pause", NULL);
            effect_buttons(p, lane);
        } else {
            if (s->stream.state == STREAM_FAILED) {
                text_wrapped(s->stream.error, danger);
            } else {
                text_wrapped(s->config.stream.server_url[0]
                                 ? "Start opens privacy-paused."
                                 : "Set up a destination in Streaming setup…",
                             muted);
            }
            command_button(p, 130,
                           s->stream.state == STREAM_FAILED ? "Retry connection"
                           : s->config.stream.server_url[0] && s->config.stream.key_file[0]
                               ? "Start streaming"
                               : "Start streaming — set up first",
                           connected && s->config.stream.server_url[0] &&
                               s->config.stream.key_file[0],
                           "stream", "start", NULL);
        }
        button(p, 150, "Streaming setup…", true, false, A_SETUP, 1);
        lane_annotations(p, lane);
    } else {
        text_wrapped("What the audience hears — check this before revealing anything.", muted);
        p->draft_context = false;
        named_field(p, "audio.mic");
        named_field(p, "audio.desktop");
        named_field(p, "audio.virtual");
        button(p, 151, "Sources & gains", true, false, A_SECTION, 5);
        text_wrapped("Gains in Compose → Audio · virtual source name is config-only.", muted);
    }
}
/* Prototype sections regroup existing controls without discarding advanced fields. */
static int field_section(const FieldSpec *f)
{
    const char *key = f->key;
    if (f->tab == TAB_STREAM) {
        return 6;
    }
    if (f->tab == TAB_CAMERA) {
        return 1;
    }
    if (f->tab == TAB_AUDIO) {
        return 5;
    }
    if (f->tab == TAB_EFFECTS) {
        return !strncmp(key, "logo.", 5) || !strncmp(key, "text.", 5) ? 3 : 4;
    }
    if (f->tab == TAB_SOURCE) {
        return !strncmp(key, "screen.", 7) || !strncmp(key, "background.", 11) ||
                       !strcmp(key, "camera.width_percent")
                   ? 2
                   : 0;
    }
    return 6;
}
static bool field_in_section(const FieldSpec *f, int section)
{
    return section == 7 ? f->tab == TAB_STREAM : field_section(f) == section;
}
static bool section_dirty(const Panel *p, int section)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (field_in_section(&fields[i], section) && p->edit[i].dirty) {
            return true;
        }
    }
    return false;
}
static void section_actions(Panel *p, int section)
{
    bool connected = p->snapshot.connected;
    if (section == 0) {
        CLAY({.layout = {.childGap = 6}})
        {
            command_button(p, 70, "Monitor", connected, "capture", "monitor", NULL);
            command_button(p, 71, "Select region…",
                           connected && p->snapshot.capabilities.region_selection, "capture",
                           "region", "select");
        }
        command_button(p, 72, "Select window…",
                       connected && p->snapshot.capabilities.window_selection, "capture", "window",
                       "select");
        text_wrapped("Interactive selection keeps the previous source on Esc.", muted);
        CLAY({.layout = {.childGap = 6}})
        {
            command_button(p, 90, "Previous layout", connected, "layout", "prev", NULL);
            command_button(p, 91, "Next layout", connected, "layout", "next", NULL);
        }
        CLAY({.layout = {.childGap = 6}})
        {
            button(p, 40, "Preset", connected && p->snapshot.config.preset_count > 0, false,
                   A_DROPDOWN, -1);
            p->widgets[p->widget_count - 1].type = W_SELECT;
            command_button(p, 92, "Previous", connected, "preset", "prev", NULL);
            command_button(p, 41, "Next", connected, "preset", "next", NULL);
        }
        text_wrapped("Composition only — never touches outputs.", muted);
    } else if (section == 1) {
        command_button(p, 80, "List cameras", connected, "camera", "list", NULL);
        text_wrapped(
            "Stage places the screen opposite the anchor. Mirror changes camera content only.",
            muted);
    } else if (section == 2) {
        text_wrapped("Backdrop fills uncovered screen/stage area. Screen and camera sizes preserve "
                     "their aspect ratios.",
                     muted);
    } else if (section == 3) {
        text_wrapped("Overlays sit above sources; pause replaces them, freeze/blur include them. "
                     "Static text is literal; placeholders are not expanded.",
                     muted);
    } else if (section == 4) {
        command_button(p, 84, "Clear now", connected, "keys", "clear", NULL);
    } else if (section == 5) {
        command_button(p, 83, "List audio sources", connected, "audio", "list", NULL);
        text_wrapped("Disappeared sources never fall back to a broad capture.", muted);
        if (p->snapshot.audio_status[0]) {
            text_wrapped(p->snapshot.audio_status, muted);
        }
    } else if (section == 6) {
        button(p, 150, "Streaming setup…", true, false, A_SETUP, 1);
        command_button(p, 85, "Reload config file", connected, "config", "reload", NULL);
    }
}
static const char *section_summary(Panel *p, int section)
{
    const Config *c = &p->snapshot.config;
    switch (section) {
    case 0:
        return format(p, "%s · %s · %s · %.2f×", c->capture_kind, c->layout, c->fit,
                      c->zoom_factor);
    case 1:
        return format(p, "%s · %s · %.0f%%", c->shape, c->anchor, c->camera_width_percent);
    case 2:
        return format(p, "screen %s · gradient %d-stop · stage sizes", c->screen_background,
                      c->gradient_via_enabled ? 3 : 2);
    case 3:
        return format(p, "Logo %s · Text %s", c->logo_enabled ? "on" : "off",
                      c->text_enabled ? "on" : "off");
    case 4:
        return format(p, "cursor %s · clicks %s · keys %s", c->cursor ? "on" : "off",
                      c->clicks ? "on" : "off", c->keys ? c->keys_mode : "off");
    case 5:
        return format(p, "mic %s · desktop %s", c->mic ? "on" : "off", c->desktop ? "on" : "off");
    default:
        return "pause & blur screens · recording · streaming";
    }
}
static void section_list(Panel *p)
{
    float width = fminf(p->width, 520) - 32 - 24;
    for (int section = 0; section < 7; section++) {
        uint32_t id = 100 + (uint32_t)section;
        Widget *w = widget(p, id, W_BUTTON, A_SECTION, true);
        if (w) {
            w->index = section;
        }
        CLAY(
            {.id = element_id(id),
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(64)},
                        .padding = {16, 16, 9, 9},
                        .childGap = 8,
                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
             .backgroundColor = hot(id) ? surface : background,
             .border = {.color = p->focus == id ? accent : line,
                        .width = p->focus == id ? outline_width : (Clay_BorderWidth){.bottom = 1}}})
        {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                             .childGap = 2,
                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                label(fit_text(p, section_names[section], width, 2), 2, foreground);
                label(fit_text(p, section_summary(p, section), width, 0), 0, secondary);
            }
            icon_slot(ICON_FORWARD, true, false);
        }
    }
}
static void compose_body(Panel *p)
{
    if (p->open_section < 0) {
        section_list(p);
        return;
    }
    int section = p->open_section;
    p->draft_context = true;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 10, 18},
                     .childGap = 6,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        section_actions(p, section);
        if (section == 6) {
            label("Recording countdown", 2, foreground);
        }
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (field_section(&fields[i]) == section && fields[i].group == 0 &&
                fields[i].tab != TAB_STREAM) {
                field_row(p, i);
            }
        }
        const char *names[TAB_COUNT][GROUP_COUNT] = {
            {"", "Capture & exclusion", "Zoom behavior", "Stage sizes", "Screen appearance",
             "Background gradient"},
            {"", "Position & crop", "Camera appearance", "Device"},
            {"", "Virtual microphone"},
            {"", "Output annotations", "Cursor & clicks", "Keystroke style", "Logo", "Static text"},
            {"", "Recording format", "Cycle order", "Output & connection", "Pause screen",
             "Blur screen"},
            {"", "Advanced network"}};
        for (int tab = 0; tab < TAB_COUNT; tab++) {
            for (int group = 1; group < GROUP_COUNT; group++) {
                bool any = false;
                for (size_t i = 0; i < FIELD_COUNT; i++) {
                    any |= fields[i].tab == tab && fields[i].group == group &&
                           field_section(&fields[i]) == section;
                }
                if (!any) {
                    continue;
                }
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {0, 0, 14, 2}},
                      .border = {.color = line, .width = {.top = 1}}})
                {
                    label(names[tab][group] ? names[tab][group] : "Advanced", 2, foreground);
                }
                if (tab == TAB_SOURCE && group == 1) {
                    text_wrapped("Mask covers the panel and preview with your chosen color. "
                                 "Transparent shows Cast's screen background through the overlap; "
                                 "it cannot reveal desktop content hidden behind either window.",
                                 secondary);
                }
                if (tab == TAB_SETTINGS && group >= 4) {
                    text_wrapped("Title, subtitle and footer are optional. Templates: {date}, "
                                 "{time}, {datetime:…}.",
                                 muted);
                    text_wrapped(
                        format(p,
                               "Font is config-only: output.%s_font = %s; reload the config file.",
                               group == 4 ? "pause" : "blur",
                               group == 4 ? p->snapshot.config.pause_font
                                          : p->snapshot.config.blur_font),
                        muted);
                }
                for (size_t i = 0; i < FIELD_COUNT; i++) {
                    if (fields[i].tab == tab && fields[i].group == group &&
                        field_section(&fields[i]) == section) {
                        field_row(p, i);
                    }
                }
            }
        }
        if (section == 6) {
            label("Streaming setup", 2, foreground);
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (fields[i].tab == TAB_STREAM) {
                    field_row(p, i);
                }
            }
        }
    }
}
static void page_header(Panel *p)
{
    if (p->view != VIEW_SECTION && p->view != VIEW_SETUP) {
        return;
    }
    CLAY({.id = CLAY_ID("PageHeader"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(52)},
                     .padding = {16, 16, 6, 6},
                     .childGap = 10,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = background,
          .border = {.color = line, .width = {.bottom = 1}}})
    {
        button(p, 99, p->view == VIEW_SETUP ? "Back" : "Compose", true, false, A_BACK, 0);
        const char *title =
            p->view == VIEW_SETUP ? "Streaming setup" : section_names[p->open_section];
        label(fit_text(p, title, fminf(p->width, 520) - 170, 2), 2, foreground);
    }
}
static void operate_body(Panel *p)
{
    p->draft_context = false;
    for (int lane = 0; lane < 4; lane++) {
        disclosure(p, 120 + (uint32_t)lane, lane_names[lane], lane_status(p, lane), A_LANE, lane,
                   p->open_lane == lane);
        if (p->open_lane != lane) {
            continue;
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                         .padding = {16, 16, 0, 12},
                         .childGap = 8,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            lane_body(p, lane);
        }
    }
}
static void pinned_drafts(Panel *p)
{
    int section = p->view == VIEW_SETUP ? 7 : p->view == VIEW_SECTION ? p->open_section : -1;
    unsigned count = 0;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        count += section >= 0 && field_in_section(&fields[i], section) && p->edit[i].dirty;
    }
    if (!count) {
        return;
    }
    CLAY({.id = CLAY_ID("DraftBar"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {10, 10, 6, 6},
                     .childGap = 4,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = surface,
          .border = {.color = line, .width = {.top = 1}}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
        {
            quiet_label(format(p, "%u unapplied edit%s", count, count == 1 ? "" : "s"), secondary,
                        false);
        }
        quiet_label("·", secondary, false);
        button(p, 700 + (uint32_t)section * 2, "Apply", p->snapshot.connected, true,
               A_APPLY_SECTION, section);
        button(p, 701 + (uint32_t)section * 2, "Revert", true, false, A_REVERT, section);
        quiet_label("·", secondary, false);
        quiet_label("session only", secondary, false);
    }
}
static void status_token(Panel *p, uint32_t id, const char *text, int section, float budget)
{
    const char *shown = fit_text(p, text, budget, FONT_QUIET);
    Widget *w = widget(p, id, W_LINK, section < 0 ? A_COMMAND : A_SECTION, true);
    if (w) {
        w->index = section;
        if (section < 0) {
            w->arg[0] = "zoom";
            w->arg[1] = "reset";
            w->argc = 2;
            w->enabled = p->snapshot.connected;
        }
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.height = CLAY_SIZING_FIXED(16)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
    {
        quiet_label(shown, hot(id) ? foreground : secondary, false);
    }
}
static void footer(Panel *p)
{
    float available = fminf(p->width, 520) - 32;
    const PanelSnapshot *s = &p->snapshot;
    const char *error = p->error[0] ? p->error : s->error;
    if (!error[0] && s->stream.state == STREAM_FAILED && s->stream.error[0]) {
        error = format(p, "Streaming failed — %s", s->stream.error);
    }
    const char *message = error[0] ? error
                          : s->command_queued > s->command_completed
                              ? "Waiting for daemon acknowledgement…"
                          : p->reply[0] ? p->reply
                                        : "Ready — daemon is running independently.";
    const char *connection = s->connected               ? "Connected"
                             : p->navigation_generation ? "Reconnecting…"
                                                        : "Disconnected";
    CLAY({.id = CLAY_ID("StatusBar"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(46)},
                     .padding = {16, 16, 6, 6},
                     .childGap = 2,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
          .backgroundColor = surface,
          .border = {.color = line, .width = {.top = 1}}})
    {
        CLAY({.id = CLAY_ID("StatusComposition"),
              .layout = {.sizing = {.height = CLAY_SIZING_FIXED(16)},
                         .childGap = 6,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            bool zoomed = s->config.zoom_factor > 1;
            float token = (available - (zoomed ? 54 : 36)) / (zoomed ? 4 : 3);
            status_token(p, 160, s->config.monitor[0] ? s->config.monitor : s->config.capture_kind,
                         0, token);
            quiet_label("·", muted, false);
            status_token(p, 161, s->config.layout, 0, token);
            quiet_label("·", muted, false);
            status_token(p, 162, s->current_preset[0] ? s->current_preset : "preset —", 2, token);
            if (s->config.zoom_factor > 1) {
                quiet_label("·", muted, false);
                status_token(p, 163, format(p, "zoom %.2f× ×", s->config.zoom_factor), -1, token);
            }
        }
        CLAY({.id = CLAY_ID("StatusDaemon"),
              .layout = {.sizing = {.height = CLAY_SIZING_FIXED(16)},
                         .childGap = 6,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            state_marker(s->connected ? (Clay_Color){131, 221, 182, 255} : danger);
            quiet_label(connection, secondary, false);
            quiet_label(error[0] ? "✗"
                        : s->command_completed && !s->command_failed && !error[0] && p->reply[0]
                            ? "✓"
                            : "·",
                        error[0] ? danger : secondary, false);
            quiet_label(fit_text(p, message, available * .62f, FONT_QUIET),
                        error[0] ? danger : secondary, false);
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

          .border = {.color = line, .width = outline_width},
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
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
          .backgroundColor = background})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(fminf(p->width, 520)),
                                    .height = CLAY_SIZING_GROW()},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            pinned_header(p);
            page_header(p);
            CLAY({.id = CLAY_ID("SettingsScroll"),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                             .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
            {
                if (p->stream_setup) {

                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                                     .padding = {16, 16, 0, 14},
                                     .childGap = 6,
                                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                    {
                        p->draft_context = true;
                        text_wrapped(
                            "The key is the service-provided password for your channel. Keep it in "
                            "a regular file only you can read (600). Cast reads this file when a "
                            "stream starts and never displays or logs its contents.",
                            muted);
                        for (size_t i = 0; i < FIELD_COUNT; i++) {
                            if (fields[i].tab == TAB_STREAM) {
                                field_row(p, i);
                            }
                        }
                        text_wrapped("Advanced network controls (timeouts, reconnect policy, queue "
                                     "budget) are set in [stream] of the configuration file. While "
                                     "a session is active, connection-affecting edits are rejected "
                                     "— stop streaming first; presentation styling stays editable.",
                                     muted);

                        button(p, 153, "Cancel", true, false, A_SETUP, 0);
                    }
                } else if (p->main_tab == 0) {
                    operate_body(p);
                } else {
                    compose_body(p);
                }
            }
            pinned_drafts(p);
            footer(p);
        }
    }
    dropdown_layout(p);
    Clay_RenderCommandArray commands = Clay_EndLayout();
    if (p->restore_scroll) {
        Clay_ScrollContainerData body = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
        if (body.found) {
            body.scrollPosition->y = p->view_scroll[scroll_index(p)];
            p->restore_scroll = false;
        }
    }
    for (int i = 0; i < p->widget_count; i++) {
        Widget *w = &p->widgets[i];
        w->box = Clay_GetElementData(element_id(w->id)).boundingBox;
        if (p->view == VIEW_COMPOSE && w->id >= 100 && w->id <= 106) {
            Clay_BoundingBox body = Clay_GetElementData(CLAY_ID("SettingsScroll")).boundingBox;
            if (w->box.y < body.y || w->box.y + w->box.height > body.y + body.height) {
                w->enabled = false;
            }
        }
    }
    return commands;
}

static Clay_Dimensions measure(Clay_StringSlice text, Clay_TextElementConfig *config, void *data)
{
    Panel *p = data;
    int width = 0, height = 0;
    int font = config->fontId < 5 ? config->fontId : 1;
    if (text.length > 0) {
        TTF_GetStringSize(
            text_font(p, config->userData ? (int)(uintptr_t)config->userData : font, false),
            text.chars, (size_t)text.length, &width, &height);
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
/* One small line family, independent of font coverage and renderer DPI. */
static void stroke(Panel *p, float x1, float y1, float x2, float y2, float thickness)
{
    float length = hypotf(x2 - x1, y2 - y1);
    if (!length) {
        return;
    }
    float dx = -(y2 - y1) / length * thickness / 2;
    float dy = (x2 - x1) / length * thickness / 2;
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(p->renderer, &r, &g, &b, &a);
    SDL_FColor ink = {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    SDL_Vertex vertices[] = {{.position = {x1 + dx, y1 + dy}, .color = ink},
                             {.position = {x2 + dx, y2 + dy}, .color = ink},
                             {.position = {x2 - dx, y2 - dy}, .color = ink},
                             {.position = {x1 - dx, y1 - dy}, .color = ink}};
    const int indices[] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(p->renderer, NULL, vertices, 4, indices, 6);
}
static void circle(Panel *p, float x, float y, float radius, float thickness)
{
    for (int i = 0; i < 64; i++) {
        float a = (float)i * 2 * (float)M_PI / 64;
        float b = (float)(i + 1) * 2 * (float)M_PI / 64;
        stroke(p, x + cosf(a) * radius, y + sinf(a) * radius, x + cosf(b) * radius,
               y + sinf(b) * radius, thickness);
    }
}
static void draw_icon(Panel *p, SDL_FRect box, uintptr_t data)
{
    data -= ICON_DATA_BASE;
    color(p, data & 256 ? muted : data & 512 ? accent : secondary);
    Icon icon = (Icon)(data & 255);
#define L(x1, y1, x2, y2) stroke(p, box.x + (x1), box.y + (y1), box.x + (x2), box.y + (y2), 1.4f)
    switch (icon) {
    case ICON_BACK:
        L(12, 8, 3, 8);
        L(3, 8, 7, 4);
        L(3, 8, 7, 12);
        break;
    case ICON_FORWARD:
        L(4, 4, 9, 8);
        L(9, 8, 4, 12);
        break;
    case ICON_PLAY:
        L(4, 3, 12, 8);
        L(12, 8, 4, 13);
        L(4, 13, 4, 3);
        break;
    case ICON_PAUSE:
        L(5, 3, 5, 13);
        L(11, 3, 11, 13);
        break;
    case ICON_STOP:
        L(3, 3, 13, 3);
        L(13, 3, 13, 13);
        L(13, 13, 3, 13);
        L(3, 13, 3, 3);
        break;
    case ICON_RECORD:
        circle(p, box.x + 8, box.y + 8, 5, 1.4f);
        break;
    case ICON_CLOSE:
        L(4, 4, 12, 12);
        L(12, 4, 4, 12);
        break;
    case ICON_FREEZE:
        L(8, 2, 8, 14);
        L(3, 5, 13, 11);
        L(3, 11, 13, 5);
        break;
    case ICON_BLUR:
        L(4, 3, 12, 3);
        L(2, 6, 14, 6);
        L(4, 9, 12, 9);
        L(6, 12, 10, 12);
        break;
    case ICON_CUT:
        circle(p, box.x + 4, box.y + 12, 2, 1.4f);
        circle(p, box.x + 12, box.y + 12, 2, 1.4f);
        L(5, 10, 13, 2);
        L(11, 10, 3, 2);
        break;
    case ICON_SCREEN:
        L(2, 3, 14, 3);
        L(14, 3, 14, 11);
        L(14, 11, 2, 11);
        L(2, 11, 2, 3);
        L(8, 11, 8, 14);
        L(5, 14, 11, 14);
        break;
    case ICON_CAMERA:
        L(2, 4, 11, 4);
        L(11, 4, 11, 12);
        L(11, 12, 2, 12);
        L(2, 12, 2, 4);
        L(11, 6, 15, 4);
        L(15, 4, 15, 12);
        L(15, 12, 11, 10);
        break;
    case ICON_AUDIO:
        L(6, 2, 10, 2);
        L(10, 2, 10, 9);
        L(10, 9, 6, 9);
        L(6, 9, 6, 2);
        L(3, 7, 3, 11);
        L(3, 11, 13, 11);
        L(13, 11, 13, 7);
        L(8, 11, 8, 14);
        L(5, 14, 11, 14);
        break;
    case ICON_EFFECTS:
        L(3, 12, 12, 3);
        L(3, 12, 5, 14);
        L(5, 14, 14, 5);
        L(14, 5, 12, 3);
        L(3, 2, 3, 6);
        L(1, 4, 5, 4);
        break;
    case ICON_SETTINGS:
        L(2, 4, 14, 4);
        L(2, 8, 14, 8);
        L(2, 12, 14, 12);
        L(5, 2, 5, 6);
        L(11, 6, 11, 10);
        L(7, 10, 7, 14);
        break;
    case ICON_RESET:
        circle(p, box.x + 8, box.y + 8, 5, 1.4f);
        L(2, 2, 2, 6);
        L(2, 6, 6, 6);
        break;
    case ICON_PLUS:
        L(3, 8, 13, 8);
        L(8, 3, 8, 13);
        break;
    case ICON_MINUS:
        L(3, 8, 13, 8);
        break;
    case ICON_STREAM:
        L(2, 8, 2, 13);
        L(7, 4, 7, 13);
        L(12, 1, 12, 13);
        break;
    case ICON_INFO:
        circle(p, box.x + 8, box.y + 8, 6, 1.4f);
        L(8, 7, 8, 11);
        L(8, 4, 8, 4.5f);
        break;
    case ICON_CHEVRON:
        L(4, 6, 8, 10);
        L(8, 10, 12, 6);
        break;
    case ICON_DOT:
        color(p, (Clay_Color){(float)((data >> 40) & 255), (float)((data >> 32) & 255),
                              (float)((data >> 24) & 255), 255});
        /* A filled fan stays a dot at 1x; overlapping thick strokes on a
         * two-pixel ring otherwise look like an asterisk at small sizes. */
        Uint8 dot_r, dot_g, dot_b, dot_a;
        SDL_GetRenderDrawColor(p->renderer, &dot_r, &dot_g, &dot_b, &dot_a);
        SDL_FColor dot_ink = {dot_r / 255.f, dot_g / 255.f, dot_b / 255.f, dot_a / 255.f};
        SDL_Vertex vertices[17] = {{.position = {box.x + 3.5f, box.y + 3.5f}, .color = dot_ink}};
        int triangles[48];
        for (int i = 0; i < 16; i++) {
            float angle = (float)i * 2 * (float)M_PI / 16;
            vertices[i + 1] = (SDL_Vertex){
                .position = {box.x + 3.5f + 2.5f * cosf(angle), box.y + 3.5f + 2.5f * sinf(angle)},
                .color = dot_ink};
            triangles[i * 3] = 0;
            triangles[i * 3 + 1] = i + 1;
            triangles[i * 3 + 2] = (i + 1) % 16 + 1;
        }
        SDL_RenderGeometry(p->renderer, NULL, vertices, 17, triangles, 48);
        break;
    case ICON_NONE:
        break;
    }
#undef L
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
    SDL_Surface *surface_text = TTF_RenderText_Blended(text_font(p, font, true), text, length,
                                                       (SDL_Color){255, 255, 255, 255});
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
static unsigned countdown_seconds(uint64_t remaining_ns)
{
    return (unsigned)(remaining_ns / 1000000000ULL + (remaining_ns % 1000000000ULL != 0));
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
    const char *placeholder =
        strstr(fields[index].key, "footer")     ? "No footer"
        : strstr(fields[index].key, "subtitle") ? "No subtitle"
        : strstr(fields[index].key, "title") || !strcmp(fields[index].key, "output.pause_text")
            ? "No title"
            : "Default";
    const char *text = value[0] ? value : placeholder;
    draw_text(p, text, strlen(text), 1, value[0] ? foreground : muted, x, y);
    SDL_SetRenderClipRect(p->renderer, &outer_clip);
}
static bool list_command_visible(const Panel *p, Clay_BoundingBox box)
{
    if (p->view != VIEW_COMPOSE) {
        return true;
    }
    Clay_BoundingBox viewport = Clay_GetElementData(CLAY_ID("SettingsScroll")).boundingBox;
    for (int i = 0; i < p->widget_count; i++) {
        const Widget *w = &p->widgets[i];
        if (w->id < 100 || w->id > 106) {
            continue;
        }
        Clay_BoundingBox row = w->box;
        bool partial = row.y < viewport.y || row.y + row.height > viewport.y + viewport.height;
        if (partial && box.x >= row.x - .1f && box.y >= row.y - .1f &&
            box.x + box.width <= row.x + row.width + .1f &&
            box.y + box.height <= row.y + row.height + .1f) {
            return false;
        }
    }
    return true;
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
        if (c->commandType != CLAY_RENDER_COMMAND_TYPE_SCISSOR_START &&
            c->commandType != CLAY_RENDER_COMMAND_TYPE_SCISSOR_END &&
            !list_command_visible(p, c->boundingBox)) {
            continue;
        }
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
                      c->userData                     ? (int)(uintptr_t)c->userData
                      : c->renderData.text.fontId < 5 ? c->renderData.text.fontId
                                                      : 1,
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
            if ((uintptr_t)c->renderData.custom.customData >= ICON_DATA_BASE) {
                draw_icon(p, box, (uintptr_t)c->renderData.custom.customData);
                break;
            }
            rounded(p, box, c->renderData.custom.cornerRadius.topLeft,
                    c->renderData.custom.backgroundColor);
            size_t field = (size_t)(uintptr_t)c->renderData.custom.customData - 1;
            if (field < FIELD_COUNT) {
                draw_field(p, field, c->boundingBox, clips[depth]);
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
        if (w->type == W_LINK && (hot(w->id) || p->focus == w->id)) {
            color(p, foreground);
            for (float x = w->box.x; x < w->box.x + w->box.width; x += 3) {
                SDL_RenderPoint(p->renderer, x, w->box.y + w->box.height - 1);
            }
        }
        if (w->type != W_SELECT) {
            continue;
        }
        Clay_ElementData scroll = Clay_GetElementData(CLAY_ID("SettingsScroll"));
        if (w->id >= 1000 || w->id == 40) {
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
static bool widget_in_scroll(const Panel *p, const Widget *w)
{
    (void)p;
    return w->id != 5 && w->id != 6 && w->id != 7 && w->id != 22 && w->id != 34 &&
           !(w->id >= 10 && w->id <= 12) && w->id != 99 && !(w->id >= 160 && w->id <= 163) &&
           !(w->id >= 700 && w->id <= 715) && w->type != W_OPTION;
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
    if (widget_in_scroll(p, w)) {
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
        args[0] = "virtual";
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
/* Local staging never changes output state or the authoritative configuration. */
static void stage_field(Panel *p, int index, const char *value)
{
    if (index < 0 || (size_t)index >= FIELD_COUNT || !writable(p, &fields[index])) {
        return;
    }
    FieldEdit *edit = &p->edit[index];
    snprintf(edit->value, sizeof edit->value, "%s", value);
    edit->dirty = true;
    edit->revision++;
    p->error[0] = 0;
    if (!strcmp(fields[index].key, "stream.service")) {
        Config candidate = p->snapshot.config;
        snprintf(candidate.stream.service, sizeof candidate.stream.service, "%s", value);
        config_stream_preset(&candidate);
        const char *keys[] = {"stream.video_bitrate_kbps", "stream.audio_bitrate_kbps",
                              "stream.encoder_preset"};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            int quality_index = find_field(keys[i]);
            if (quality_index < 0) {
                continue;
            }
            char selected[PATH_MAX];
            field_value(&fields[quality_index], &candidate, selected, sizeof selected);
            stage_field(p, quality_index, selected);
        }
    }
}
static void apply_section(Panel *p, int section)
{
    const char *arguments[CAST_MAX_ARGS] = {"settings"};
    int count = 1;
    size_t indexes[CAST_MAX_ARGS / 2], submitted = 0;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (!field_in_section(&fields[i], section) || !p->edit[i].dirty) {
            continue;
        }
        if (!writable(p, &fields[i]) || p->edit[i].pending) {
            snprintf(p->error, sizeof p->error,
                     "Stop the affected output or wait for acknowledgement before applying %s.",
                     fields[i].label);
            return;
        }
        if (validate_field(&fields[i], p->edit[i].value, p->error, sizeof p->error)) {
            return;
        }
        if (count + 2 > CAST_MAX_ARGS) {
            snprintf(p->error, sizeof p->error,
                     "Too many edited fields for one atomic Apply; apply smaller groups.");
            return;
        }
        indexes[submitted++] = i;
        arguments[count++] = fields[i].key;
        arguments[count++] = p->edit[i].value;
    }
    if (!submitted) {
        return;
    }
    p->error[0] = 0;
    /* A settings batch validates and prepares every resource before daemon mutation. */
    if (panel_client_command(p->client, count, arguments, p->error, sizeof p->error)) {
        return;
    }
    p->reply[0] = 0;
    for (size_t i = 0; i < submitted; i++) {
        FieldEdit *edit = &p->edit[indexes[i]];
        edit->pending = p->snapshot.command_queued + 1;
        edit->submitted_revision = edit->revision;
    }
}
static void activate(Panel *p, Widget *w)
{
    if (!w || !w->enabled) {
        return;
    }
    switch (w->action) {
    case A_CLOSE:
        p->quit = true;
        break;
    case A_COMMAND:
        p->error[0] = 0;
        if (!panel_client_command(p->client, w->argc, w->arg, p->error, sizeof p->error)) {
            p->reply[0] = 0;
        }
        break;
    case A_SETTING:
        if (w->draft) {
            stage_field(p, w->index, w->value);
        } else {
            apply_setting(p, w->index, w->value, false);
        }
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
        p->groups[w->auxiliary][w->index] = !p->groups[w->auxiliary][w->index];
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
            Widget *selector = find_widget(p, p->dropdown);
            if (selector && selector->draft) {
                stage_field(p, p->dropdown_field, w->value);
            } else {
                apply_setting(p, p->dropdown_field, w->value, false);
            }
        }
        p->focus = p->dropdown;
        p->dropdown = 0;
        break;
    case A_MAIN_TAB:
        stop_editing(p);
        navigate_view(p, w->index ? VIEW_COMPOSE : VIEW_OPERATE, -1);
        p->focus = w->index ? 6 : 5;
        break;
    case A_LANE:
        stop_editing(p);
        p->open_lane = p->view == VIEW_OPERATE && p->open_lane == w->index ? -1 : w->index;
        navigate_view(p, VIEW_OPERATE, -1);
        break;
    case A_SECTION:
        stop_editing(p);
        navigate_view(p, VIEW_SECTION, w->index);
        p->focus = 99;
        break;
    case A_SETUP:
        stop_editing(p);
        if (w->index) {
            p->return_view = p->view;
            p->return_section = p->open_section;
            navigate_view(p, VIEW_SETUP, -1);
        } else {
            navigate_view(p, p->return_view, p->return_section);
        }
        break;
    case A_BACK:
        stop_editing(p);
        navigate_view(p, p->view == VIEW_SETUP ? p->return_view : VIEW_COMPOSE,
                      p->view == VIEW_SETUP ? p->return_section : -1);
        p->focus = p->main_tab ? 6 : 5;
        break;
    case A_APPLY_SECTION:
        apply_section(p, w->index);
        stop_editing(p);
        break;
    case A_REVERT:
        stop_editing(p);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (field_in_section(&fields[i], w->index)) {
                field_value(&fields[i], &p->snapshot.config, p->edit[i].value,
                            sizeof p->edit[i].value);
                p->edit[i].dirty = false;
                p->edit[i].revision++;
            }
        }
        break;
    case A_PREVIEW: {
        const char *args[] = {"preview", "target",
                              w->index == 2   ? "stream"
                              : w->index == 1 ? "record"
                                              : "virtual"};
        p->error[0] = 0;
        panel_client_command(p->client, 3, args, p->error, sizeof p->error);
    } break;
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
    if ((event->mod & SDL_KMOD_ALT) && key >= SDLK_1 && key <= SDLK_7) {
        stop_editing(p);
        navigate_view(p, VIEW_SECTION, (int)(key - SDLK_1));
        p->focus = 99;
        return;
    }
    if (!p->active_text && !ctrl && !(event->mod & SDL_KMOD_ALT) &&
        (key == SDLK_1 || key == SDLK_2)) {
        navigate_view(p, key == SDLK_1 ? VIEW_OPERATE : VIEW_COMPOSE, -1);
        p->focus = key == SDLK_1 ? 5 : 6;
        return;
    }
    if (key == SDLK_ESCAPE) {
        if (p->dropdown) {
            p->focus = p->dropdown;
            p->dropdown = 0;
            return;
        }
        stop_editing(p);
        if (p->view == VIEW_SETUP || p->view == VIEW_SECTION) {
            navigate_view(p, p->view == VIEW_SETUP ? p->return_view : VIEW_COMPOSE,
                          p->view == VIEW_SETUP ? p->return_section : -1);
            p->focus = p->main_tab ? 6 : 5;
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
                if (p->view == VIEW_SECTION || p->view == VIEW_SETUP) {
                    apply_section(p, p->view == VIEW_SETUP ? 7 : p->open_section);
                } else {
                    apply_setting(p, index, edit->value, true);
                }
            }
            stop_editing(p);
        }
        return;
    }
    int section = p->view == VIEW_SETUP ? 7 : p->view == VIEW_SECTION ? p->open_section : -1;
    if (!p->dropdown && section >= 0 && (key == SDLK_RETURN || key == SDLK_KP_ENTER) &&
        section_dirty(p, section)) {
        apply_section(p, section);
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
    for (int i = p->widget_count - 1; i >= 0; i--) {
        Widget *w = &p->widgets[i];
        if (p->dropdown && w->type != W_OPTION && w->id != p->dropdown) {
            continue;
        }
        if (!w->enabled || x < w->box.x || y < w->box.y || x >= w->box.x + w->box.width ||
            y >= w->box.y + w->box.height) {
            continue;
        }
        if (widget_in_scroll(p, w)) {
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
static float wheel_delta(const SDL_MouseWheelEvent *wheel)
{
    float direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
    return wheel->y * direction * 6;
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
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        p->scroll += wheel_delta(&e->wheel);
        break;
    case SDL_EVENT_KEY_DOWN:
        key_event(p, &e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        insert_text(p, e->text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        p->mouse_down = false;
        p->dropdown = 0;
        stop_editing(p);
        break;
    default:
        break;
    }
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
        if (fresh.connected && fresh.daemon_generation != p->navigation_attachment) {
            /* Transport attachment counters change on every panel launch. The
             * daemon socket's inode and creation time identify its actual session. */
            struct stat socket_stat;
            uint64_t cookie = 0;
            if (!stat(fresh.config.socket_path, &socket_stat)) {
                cookie = (uint64_t)socket_stat.st_ino ^ ((uint64_t)socket_stat.st_dev << 32) ^
                         ((uint64_t)socket_stat.st_ctim.tv_sec << 16) ^
                         (uint64_t)socket_stat.st_ctim.tv_nsec;
            }
            if (cookie && cookie != p->navigation_generation) {
                NavigationMemory saved = p->navigation ? *p->navigation : (NavigationMemory){0};
                bool valid =
                    saved.magic == 0x43565032 && saved.session_cookie == cookie &&
                    saved.view >= VIEW_OPERATE && saved.view <= VIEW_SETUP && saved.section >= -1 &&
                    saved.section < 7 && (saved.view != VIEW_SECTION || saved.section >= 0) &&
                    saved.lane >= -1 && saved.lane < 4 && saved.return_view >= VIEW_OPERATE &&
                    saved.return_view <= VIEW_SECTION &&
                    (saved.return_view != VIEW_SECTION ||
                     (saved.return_section >= 0 && saved.return_section < 7));
                memset(p->view_scroll, 0, sizeof p->view_scroll);
                p->restore_scroll = true;
                if (valid) {
                    memcpy(p->view_scroll, saved.scroll, sizeof saved.scroll);
                    for (size_t i = 0; i < VIEW_SCROLL_COUNT; i++) {
                        if (!isfinite(p->view_scroll[i]) || p->view_scroll[i] > 0 ||
                            p->view_scroll[i] < -1000000) {
                            p->view_scroll[i] = 0;
                        }
                    }
                    p->open_lane = saved.lane;
                    p->return_view = saved.return_view;
                    p->return_section = saved.return_section;
                    navigate_view(p, saved.view, saved.section);
                } else {
                    p->open_lane = -1;
                    p->return_view = VIEW_OPERATE;
                    p->return_section = -1;
                    navigate_view(p, VIEW_OPERATE, -1);
                }
                p->navigation_generation = cookie;
            }
            p->navigation_attachment = fresh.daemon_generation;
        }
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
        Atom property = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
        XChangeProperty(display, xid, property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&utility, 1);
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
    save_navigation(p);
    if (p->navigation) {
        munmap(p->navigation, sizeof *p->navigation);
    }
    panel_client_close(p->client);
    for (int i = 0; i < TEXT_CACHE_MAX; i++) {
        SDL_DestroyTexture(p->cache[i].texture);
        free(p->cache[i].text);
    }
    for (int i = 0; i < 5; i++) {
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

/* Opt-in native test diagnostics report actual layout and acknowledged state. */
static void write_ui_state(Panel *p, const char *path)
{
    if (!path) {
        return;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        return;
    }
    Clay_BoundingBox scroll = Clay_GetElementData(CLAY_ID("SettingsScroll")).boundingBox;
    Clay_ScrollContainerData scroll_data = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    fprintf(file,
            "{\"tab\":%d,\"frame\":%llu,\"connected\":%s,\"preview_enabled\":%s,\"preview_"
            "available\":%s,\"preview_target\":\"%s\","
            "\"command_pending\":%s,\"command_queued\":%llu,\"command_completed\":%llu,"
            "\"density\":%.3f,\"input_scale\":%.3f,\"raster_font_size\":%.3f,"
            "\"scroll\":[%.1f,%.1f,%.1f,%.1f],\"scroll_offset\":%.3f,\"widgets\":[",
            p->main_tab, (unsigned long long)p->draw_frame,
            p->snapshot.connected ? "true" : "false", p->snapshot.config.preview ? "true" : "false",
            p->snapshot.capabilities.preview ? "true" : "false", p->snapshot.config.preview_target,
            p->snapshot.command_queued > p->snapshot.command_completed ? "true" : "false",
            (unsigned long long)p->snapshot.command_queued,
            (unsigned long long)p->snapshot.command_completed, p->density, p->input_scale,
            TTF_GetFontSize(p->raster_font[1]), scroll.x, scroll.y, scroll.width, scroll.height,
            scroll_data.found ? scroll_data.scrollPosition->y : 0);
    for (int i = 0; i < p->widget_count; i++) {
        const Widget *w = &p->widgets[i];
        const char *key = w->id >= 1000 && w->index >= 0 && (size_t)w->index < FIELD_COUNT
                              ? fields[w->index].key
                              : "";
        fprintf(file,
                "%s{\"id\":%u,\"key\":\"%s\",\"enabled\":%s,"
                "\"box\":[%.1f,%.1f,%.1f,%.1f],\"value\":",
                i ? "," : "", w->id, key, w->enabled ? "true" : "false", w->box.x, w->box.y,
                w->box.width, w->box.height);
        char value[PATH_MAX] = "";
        if (key[0]) {
            field_value(&fields[w->index], &p->snapshot.config, value, sizeof value);
        }
        json_string(file, value);
        bool is_field = key[0] && w->index >= 0 && (size_t)w->index < FIELD_COUNT;
        fprintf(file,
                ",\"dirty\":%s,\"draft\":", is_field && p->edit[w->index].dirty ? "true" : "false");
        json_string(file, is_field ? p->edit[w->index].value : "");
        fputc('}', file);
    }
    Clay_BoundingBox status = Clay_GetElementData(CLAY_ID("StatusBar")).boundingBox;
    Clay_BoundingBox composition = Clay_GetElementData(CLAY_ID("StatusComposition")).boundingBox;
    Clay_BoundingBox daemon = Clay_GetElementData(CLAY_ID("StatusDaemon")).boundingBox;
    fprintf(file, "],\"status_rows\":[[%.1f,%.1f,%.1f,%.1f],[%.1f,%.1f,%.1f,%.1f]]", composition.x,
            composition.y, composition.width, composition.height, daemon.x, daemon.y, daemon.width,
            daemon.height);
    fprintf(file, ",\"view\":%d,\"status_bar\":[%.1f,%.1f,%.1f,%.1f]", p->view, status.x, status.y,
            status.width, status.height);
    fprintf(file,
            ",\"countdown\":%s,\"countdown_seconds\":%u,"
            "\"clipboard_text_available\":%s,\"focus\":%u,\"active_text\":%u,\"edit_text\":",
            p->snapshot.connected && p->snapshot.countdown ? "true" : "false",
            countdown_seconds(p->snapshot.countdown_remaining_ns),
            SDL_HasClipboardText() ? "true" : "false", p->focus, p->active_text);
    int edit_index;
    FieldEdit *edit = active_edit(p, &edit_index);
    json_string(file, edit ? edit->value : "");
    fputs(",\"pause_message\":", file);
    json_string(file, p->snapshot.config.pause_text);
    fputs(",\"camera_anchor\":", file);
    json_string(file, p->snapshot.config.anchor);
    fputs(",\"camera_background\":", file);
    json_string(file, p->snapshot.config.camera_background);
    fputs(",\"virtual_status\":", file);
    json_string(file, virtual_status(&p->snapshot));
    fprintf(file, ",\"open_lane\":%d,\"open_section\":%d,\"stream_setup\":%s", p->open_lane,
            p->open_section, p->stream_setup ? "true" : "false");
    fputs(",\"stream_status\":", file);
    json_string(file, panel_stream_status(&p->snapshot));
    fputs(",\"record_status\":", file);
    json_string(file, record_status(&p->snapshot));
    fputs(",\"error\":", file);
    json_string(file, p->error[0] ? p->error : p->snapshot.error);
    fputs(",\"exclusion\":", file);
    json_string(file, p->snapshot.exclusion);
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
    open_navigation(p, config);
    p->tab = -1;
    p->open_lane = -1;
    p->open_section = -1;
    p->density = 1;
    p->input_scale = 1;
    SDL_SetHint(SDL_HINT_APP_ID, "org.cast.Panel");
    SDL_SetHint(SDL_HINT_X11_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE_UTILITY");
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
        snprintf(error, n, "cannot initialize control panel: %s", SDL_GetError());
        cleanup(p, NULL);
        return -1;
    }
    /* X11 uses physical window dimensions. Create at its final scaled size so
     * SDL's saved position and mwm's first centering both use the same bounds. */
    float initial_scale = 1;
    if (!strcmp(SDL_GetCurrentVideoDriver(), "x11")) {
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (scale > 0) {
            initial_scale = scale;
        }
    }
    p->window = SDL_CreateWindow(
        "cast control panel", (int)roundf(480 * initial_scale), (int)roundf(760 * initial_scale),
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
    for (int i = 0; i < 5; i++) {
        SDL_IOStream *stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        p->font[i] = stream ? TTF_OpenFontIO(stream, true, (float)font_sizes[i]) : NULL;
        stream = SDL_IOFromConstMem(cast_panel_font_data, font_length);
        p->raster_font[i] = stream ? TTF_OpenFontIO(stream, true, (float)font_sizes[i]) : NULL;
        if (!p->font[i] || !p->raster_font[i]) {
            snprintf(error, n, "cannot open bundled control panel font: %s", SDL_GetError());
            cleanup(p, NULL);
            return -1;
        }
        if (i == 2 || i == 3) {
            TTF_SetFontStyle(p->font[i], TTF_STYLE_BOLD);
            TTF_SetFontStyle(p->raster_font[i], TTF_STYLE_BOLD);
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
    /* mwm evaluates rules and centers utility windows on their first MapRequest.
     * Set both properties on the final renderer window while it is still hidden. */
    uint64_t xid = window_xid(p->window);
    if (!SDL_ShowWindow(p->window)) {
        snprintf(error, n, "cannot show control panel window: %s", SDL_GetError());
        cleanup(p, clay_memory);
        return -1;
    }
    p->client = panel_client_open(config, xid, error, n);
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
            for (int i = 0; i < 5; i++) {
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
        save_navigation(p);
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
