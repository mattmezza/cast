#include "panel.h"
#include "panel_color_pick.h"
#include "panel_lifecycle.h"
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
#include <sys/file.h>
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
    bool dirty, auto_apply;
    uint64_t pending, revision, submitted_revision;
} FieldEdit;
typedef enum {
    W_BUTTON,
    W_FIELD,
    W_SELECT,
    W_OPTION,
    W_LINK,
    W_SLIDER,
    W_ANCHOR,
    W_COLOR
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
    A_CLOSE,
    A_ALL,
    A_COLOR,
    A_SWATCH,
    A_PICK_COLOR,
    A_MENU,
    A_DAEMON,
    A_QUIT_APP,
    A_QUIT_CANCEL,
    A_QUIT_CONFIRM
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
    ICON_CHEVRON_UP,
    ICON_EYEDROPPER,
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
    bool all_settings[6];
} NavigationMemory;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font[5], *raster_font[5];
    float density, input_scale;
    PanelClient *client;
    PanelLifecycle *lifecycle;
    PanelLifecycleSnapshot lifecycle_state;
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
    bool all_settings[6];
    int open_menu;
    int menu_start, menu_end, dropdown_start, dropdown_end, color_start, color_end;
    int lane_jump;
    uint32_t color_popup, slider_drag;
    int color_field, pick_field;
    PanelColorPick *color_pick;
    bool picking_color, pick_requested, pointer_needs_sync;
    uint64_t device_request;
    int device_count;
    char device_values[64][256], device_labels[64][256];
    bool select_all;
    size_t caret;
    float mouse_x, mouse_y, scroll;
    float width, height;
    char error[CAST_ERR], reply[CAST_ERR];
    uint64_t command_seen, draw_frame;
    uint32_t pending_button;
    uint64_t pending_button_request;
    bool draft_context;
    bool quit_confirmation, quit_after_stop, daemon_stopping, daemon_stopped;
    uint64_t shutdown_request, shutdown_started;
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
    p->color_popup = 0;
    p->open_menu = 0;
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
    snprintf(name, sizeof name, "/cast-panel-flow-view-%lu-%lx", (unsigned long)getuid(), hash);
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
    NavigationMemory saved = {.magic = 0x43565033,
                              .session_cookie = p->navigation_generation,
                              .view = p->view,
                              .section = p->open_section,
                              .lane = p->open_lane,
                              .return_view = p->return_view,
                              .return_section = p->return_section};
    memcpy(saved.scroll, p->view_scroll, sizeof saved.scroll);
    memcpy(saved.all_settings, p->all_settings, sizeof saved.all_settings);
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
    if (action == A_PICK_COLOR) {
        return ICON_EYEDROPPER;
    }
    if (action == A_ALL) {
        return p->all_settings[p->open_section] ? ICON_CHEVRON_UP : ICON_CHEVRON;
    }
    if (id == 7) {
        return ICON_CLOSE;
    }
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
    /* Four lifecycle actions fit one pinned row at the minimum window width. */
    bool compact = id == 7 || id == 22 || id == 8 || id == 9;
    bool global_privacy = id == 34;
    bool card_action = (id >= 30 && id <= 39 && id != 34) || id == 130 ||
                       (id >= 137 && id <= 141) || id == 150 || id == 154 || id == 151;
    bool primary_action = id == 30 || id == 32 || id == 130 || id == 150;
    bool draft_action = id >= 700 && id <= 717;
    bool small_action = (card_action && !primary_action) || draft_action || action == A_ALL ||
                        action == A_PICK_COLOR || global_privacy;
    bool combo = action == A_DROPDOWN;
    float header_width = fminf(p->width, 520) - 32 - 18;
    float compact_width = header_width * (id == 8 ? .32f : id == 9 ? .12f : .28f);
    if (action != A_TAB && action != A_GROUP && action != A_PREVIEW && action != A_DROPDOWN &&
        action != A_BACK && action != A_CLOSE && action != A_QUIT_APP && action != A_QUIT_CANCEL &&
        action != A_ALL && action != A_MENU &&
        p->snapshot.command_queued > p->snapshot.command_completed) {
        enabled = false;
    }
    if (p->pending_button == id && p->pending_button_request) {
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
          .layout = {.sizing = {.width = compact ? CLAY_SIZING_FIXED(compact_width)
                                         : global_privacy || id == 30 || id == 32 || id == 130
                                             ? CLAY_SIZING_GROW()
                                         : card_action || combo ? CLAY_SIZING_GROW()
                                                                : CLAY_SIZING_FIT(),
                                .height = CLAY_SIZING_FIXED(compact ? 36
                                                            : draft_action || action == A_ALL ||
                                                                    action == A_PICK_COLOR ||
                                                                    global_privacy
                                                                ? 30
                                                            : card_action || combo ? 36
                                                                                   : 40)},
                     .padding = {compact || small_action ? 7 : 10,
                                 compact || small_action ? 7
                                 : combo                 ? 26
                                                         : 10,
                                 compact || small_action ? 6 : 8, compact || small_action ? 6 : 8},
                     .childGap = 6,
                     .childAlignment = {.x = primary_action ? CLAY_ALIGN_X_CENTER
                                                            : CLAY_ALIGN_X_LEFT,
                                        .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .border = {.color = p->focus == id ? accent
                              : selected     ? (Clay_Color){60, 104, 134, 255}
                                             : bg,
                     .width = outline_width}})
    {
        if (p->pending_button == id && p->pending_button_request) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14),
                                        .height = CLAY_SIZING_FIXED(14)}},
                  .custom = {.customData = (void *)(uintptr_t)(1048576 + 5)}})
            {
            }
        }
        Icon icon = button_icon(p, id, action);
        if (icon != ICON_NONE && !card_action && !compact) {
            icon_slot(icon, enabled, selected);
        }
        if (compact) {
            quiet_label(fit_text(p, text, compact_width - 14, FONT_META), ink, true);
        } else {
            const char *display =
                id == 40
                    ? (p->snapshot.current_preset[0] ? p->snapshot.current_preset : "Choose preset")
                    : text;
            label(display, small_action ? 0 : 1, ink);
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
static void flow_field_control(Panel *p, size_t index);
static void field_row(Panel *p, size_t index)
{
    flow_field_control(p, index);
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
static const char *const section_names[] = {"Source & layout",       "Camera",
                                            "Background & stage",    "Overlays",
                                            "Annotations & pointer", "Pause & blur screens"};
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
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 10, 0},
                     .childGap = 8,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(36)},
                         .childGap = 6}})
        {
            command_button(p, 22,
                           s->countdown        ? "Countdown"
                           : s->config.preview ? "Preview on"
                                               : "Preview off",
                           s->connected && s->capabilities.preview && !s->countdown, "preview",
                           s->config.preview ? "off" : "on", NULL);
            button(p, 7, "Close panel", true, false, A_CLOSE, 0);
            button(p, 8,
                   p->daemon_stopping                                  ? "Stopping…"
                   : p->lifecycle_state.state == PANEL_DAEMON_STARTING ? "Starting…"
                   : s->connected || p->lifecycle_state.child_pid      ? "Stop daemon"
                                                                       : "Start daemon",
                   !p->daemon_stopping && p->lifecycle_state.state != PANEL_DAEMON_STARTING, false,
                   A_DAEMON, 0);
            button(p, 9, "Quit", true, false, A_QUIT_APP, 0);
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
        if (!s->connected) {
            CLAY({.id = CLAY_ID("DisconnectedNotice"),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {10, 10, 7, 7}},
                  .backgroundColor = surface,
                  .border = {.color = danger, .width = outline_width}})
            {
                text_wrapped(
                    p->daemon_stopped
                        ? "Daemon stopped — Start daemon restores capture. Output controls "
                          "are unavailable until it starts."
                        : "Daemon disconnected — showing last known state. Commands are "
                          "unavailable; outputs keep running per their last state and this "
                          "window reconnects automatically.",
                    p->daemon_stopped ? secondary : danger);
            }
        }
    }
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {0, 0, 10, 0}},
          .border = {.color = line, .width = {.bottom = 1}}})
    {
        text_button(p, 5, "Operate", true, A_MAIN_TAB, 0, p->main_tab == 0);
        text_button(p, 6, "Compose", true, A_MAIN_TAB, 1, p->main_tab == 1);
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
/* Shared native controls use the same FieldEdit values and acknowledgement path
 * as the original text inputs. They never own authoritative daemon state. */
#define FLOW_DATA_BASE 1048576
#define FLOW_SLIDER_DATA 1
#define FLOW_LAYOUT_DATA 2
#define FLOW_ANCHOR_DATA 3
#define FLOW_COLOR_DATA 4
#define FLOW_PENDING_DATA 5
#define FLOW_VALUE_DATA 6
#define FLOW_OVERFLOW_DATA 7
static void stage_field(Panel *, int, const char *);
static void apply_setting(Panel *, int, const char *, bool);
static void commit_field(Panel *, int);
static void choose_field(Panel *, int, const char *);
static void lane_menu_body(Panel *, int);
static const char *flow_value(Panel *p, size_t index)
{
    FieldEdit *edit = &p->edit[index];
    if (!edit->dirty && p->active_text != 1000 + index * 3 && p->active_text != 1002 + index * 3 &&
        !(p->active_text == 6500 && p->color_field == (int)index)) {
        field_value(&fields[index], &p->snapshot.config, edit->value, sizeof edit->value);
    }
    return edit->value;
}
static bool flow_enabled(const Panel *p, size_t index)
{
    return writable(p, &fields[index]) && !p->edit[index].pending &&
           p->snapshot.command_queued <= p->snapshot.command_completed;
}
static void flow_note(Panel *p, size_t index)
{
    const FieldSpec *f = &fields[index];
    if (!supported(p, f)) {
        text_wrapped("Unavailable with this capture backend.", secondary);
    } else if ((f->flags & READ_ONLY)) {
        text_wrapped("Read-only here; configure this in the configuration file.", secondary);
    } else if ((f->flags & RECORD_LOCK) && !writable(p, f) && p->snapshot.connected) {
        text_wrapped("Stop recording to change this setting.", secondary);
    } else if ((f->flags & STREAM_LOCK) && !writable(p, f) && p->snapshot.connected) {
        text_wrapped("Disconnect streaming to change this setting.", secondary);
    }
}
static void flow_choice(Panel *p, uint32_t id, const char *text, size_t index, const char *value,
                        bool selected, float width, float height, WidgetType type)
{
    bool enabled = flow_enabled(p, index);
    Widget *w = widget(p, id, type, A_SETTING, enabled);
    if (w) {
        w->index = (int)index;
        w->draft = p->draft_context;
        snprintf(w->value, sizeof w->value, "%s", value);
    }
    bool missing = false;
    if (!strcmp(fields[index].key, "logo.enabled") || !strcmp(fields[index].key, "text.enabled")) {
        int path =
            find_field(!strcmp(fields[index].key, "logo.enabled") ? "logo.path" : "text.font");
        const char *resource = p->edit[path].dirty ? p->edit[path].value
                               : !strcmp(fields[index].key, "logo.enabled")
                                   ? p->snapshot.config.logo_path
                                   : p->snapshot.config.text_font;
        missing = resource[0] == '/' && access(resource, R_OK);
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width),
                                .height = CLAY_SIZING_FIXED(height)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = selected             ? (Clay_Color){35, 63, 83, 255}
                             : hot(id) && enabled ? hovered
                                                  : surface,
          .border = {.color = missing                                   ? danger
                              : p->focus == id || (selected && enabled) ? accent
                                                                        : line,
                     .width = outline_width}})
    {
        label(text, 0, enabled ? selected ? accent : foreground : muted);
    }
}
static bool field_is_numeric(const FieldSpec *field)
{
    return field->type == FIELD_INT || field->type == FIELD_DOUBLE;
}
static bool field_is_pixels(const FieldSpec *field)
{
    static const char *const keys[] = {
        "capture.x",
        "capture.y",
        "capture.width",
        "capture.height",
        "zoom.deadzone",
        "screen.margin",
        "screen.radius",
        "screen.border_width",
        "screen.background_blur_radius",
        "camera.x",
        "camera.y",
        "camera.crop_x",
        "camera.crop_y",
        "camera.margin",
        "camera.radius",
        "camera.border_width",
        "camera.background_blur_radius",
        "cursor.size",
        "clicks.radius",
        "keys.font_size",
        "logo.margin_x",
        "logo.margin_y",
        "text.size",
        "text.margin_x",
        "text.margin_y",
        "output.pause_title_size",
        "output.pause_subtitle_size",
        "output.pause_footer_size",
        "output.pause_text_gap",
        "output.blur_radius",
        "output.blur_title_size",
        "output.blur_subtitle_size",
        "output.blur_footer_size",
        "output.blur_text_gap",
        "output.width",
        "output.height",
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (!strcmp(field->key, keys[i])) {
            return true;
        }
    }
    return false;
}
/* Dragging covers common presentation sizes. Exact inputs retain the field's
 * full validation range, including values beyond the slider's useful span. */
static void flow_slider_range(const Panel *p, size_t index, double *minimum, double *maximum)
{
    const FieldSpec *f = &fields[index];
    const char *key = f->key;
    *minimum = f->minimum;
    *maximum = f->maximum;
    if (!field_is_pixels(f)) {
        if (!strcmp(key, "stream.video_bitrate_kbps")) {
            *maximum = 16000;
        } else if (!strcmp(key, "zoom.step")) {
            *maximum = 2;
        } else if (!strcmp(key, "clicks.duration_ms")) {
            *maximum = 3000;
        } else if (!strcmp(key, "keys.timeout_ms") || !strcmp(key, "ipc.timeout_ms")) {
            *maximum = 5000;
        }
        return;
    }
    double width = p->snapshot.config.width > 0 ? p->snapshot.config.width : 1920;
    double height = p->snapshot.config.height > 0 ? p->snapshot.config.height : 1080;
    double span = *maximum;
    if (!strcmp(key, "camera.x") || !strcmp(key, "camera.crop_x")) {
        span = width;
        *minimum = fmax(f->minimum, -span);
    } else if (!strcmp(key, "camera.y") || !strcmp(key, "camera.crop_y")) {
        span = height;
        *minimum = fmax(f->minimum, -span);
    } else if (!strcmp(key, "capture.x") || !strcmp(key, "capture.width")) {
        span = fmax(width, 1920);
    } else if (!strcmp(key, "capture.y") || !strcmp(key, "capture.height")) {
        span = fmax(height, 1080);
    } else if (!strcmp(key, "screen.margin") || !strcmp(key, "camera.margin") ||
               !strcmp(key, "screen.radius") || !strcmp(key, "camera.radius")) {
        span = fmin(512, fmin(width, height) / 2);
    } else if (!strcmp(key, "screen.border_width") || !strcmp(key, "camera.border_width")) {
        span = 32;
    } else if (!strcmp(key, "zoom.deadzone") || !strcmp(key, "clicks.radius")) {
        span = 256;
    } else if (!strcmp(key, "logo.margin_x") || !strcmp(key, "text.margin_x")) {
        span = width;
    } else if (!strcmp(key, "logo.margin_y") || !strcmp(key, "text.margin_y")) {
        span = height;
    } else if (strcmp(key, "output.width") && strcmp(key, "output.height")) {
        span = 128;
    }
    *maximum = fmax(*minimum + 1, fmin(f->maximum, span));
}
static bool flow_slider_key(const char *key)
{
    int index = find_field(key);
    return index >= 0 && field_is_numeric(&fields[index]);
}
static double flow_slider_step(const FieldSpec *field)
{
    const char *key = field->key;
    if (!strcmp(key, "stream.video_bitrate_kbps")) {
        return 100;
    }
    if (strstr(key, "_ms")) {
        return 10;
    }
    if (field->type == FIELD_INT || strstr(key, "width_percent") ||
        !strcmp(key, "composition.split_ratio") || !strcmp(key, "background.gradient_waypoint") ||
        !strcmp(key, "background.gradient_angle")) {
        return 1;
    }
    if (!strcmp(key, "zoom.smoothing")) {
        return .001;
    }
    if (!strcmp(key, "zoom.factor") || !strcmp(key, "zoom.min") || !strcmp(key, "zoom.max")) {
        return .05;
    }
    return .01;
}
static const char *flow_slider_unit(const FieldSpec *field)
{
    const char *key = field->key;
    if (field_is_pixels(field)) {
        return "px";
    }
    if (strstr(key, "width_percent") || !strcmp(key, "composition.split_ratio") ||
        !strcmp(key, "background.gradient_waypoint")) {
        return "%";
    }
    if (!strcmp(key, "zoom.factor") || !strcmp(key, "zoom.min") || !strcmp(key, "zoom.max") ||
        !strcmp(key, "zoom.step") || strstr(key, "gain")) {
        return "×";
    }
    if (strstr(key, "_ms")) {
        return "ms";
    }
    if (strstr(key, "_kbps")) {
        return "kbps";
    }
    if (!strcmp(key, "background.gradient_angle")) {
        return "°";
    }
    if (!strcmp(key, "record.countdown")) {
        return "s";
    }
    if (!strcmp(key, "record.queue")) {
        return "frames";
    }
    if (!strcmp(key, "record.crf")) {
        return "CRF";
    }
    if (!strcmp(key, "output.fps")) {
        return "fps";
    }
    return field->maximum == 1 ? "0–1" : "";
}
static const char *flow_slider_label(Panel *p, size_t index, double value)
{
    const char *key = fields[index].key;
    if (strstr(key, "gain")) {
        return value <= 0 ? "−∞ dB" : format(p, "%+.1f dB", 20 * log10(value));
    }
    const char *unit = flow_slider_unit(&fields[index]);
    return format(p, "%.6g%s%s", value, unit[0] ? " " : "", unit);
}
static void flow_slider(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index < 0) {
        return;
    }
    const FieldSpec *f = &fields[index];
    uint32_t id = 1000 + (uint32_t)index * 3;
    const char *value = flow_value(p, (size_t)index);
    bool enabled = flow_enabled(p, (size_t)index);
    char validation[CAST_ERR];
    bool invalid = p->edit[index].dirty && validate_field(f, value, validation, sizeof validation);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .padding = {0, 0, 6, 8}}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
            {
                label(f->label, 1, secondary);
            }
            if (strstr(f->key, "gain")) {
                CLAY(
                    {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(72),
                                           .height = CLAY_SIZING_FIXED(20)}},
                     .custom = {.customData = (void *)(uintptr_t)(FLOW_DATA_BASE + FLOW_VALUE_DATA +
                                                                  (index << 4))}})
                {
                }
            }
        }
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                         .childGap = 10,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            Widget *w = widget(p, id, W_SLIDER, A_FIELD, enabled);
            if (w) {
                w->index = index;
                w->draft = p->draft_context;
            }
            CLAY({.id = element_id(id),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                        .height = CLAY_SIZING_FIXED(30)}},
                  .border = {.color = p->focus == id ? accent : background,
                             .width = p->focus == id ? outline_width : (Clay_BorderWidth){0}},
                  .custom = {.customData = (void *)(uintptr_t)(FLOW_DATA_BASE + FLOW_SLIDER_DATA +
                                                               (index << 4))}})
            {
            }
            Widget *input = widget(p, id + 2, W_FIELD, A_FIELD, enabled);
            if (input) {
                input->index = index;
                input->draft = p->draft_context;
            }
            CLAY({.id = element_id(id + 2),
                  .layout = {.sizing = {.width = CLAY_SIZING_FIXED(84),
                                        .height = CLAY_SIZING_FIXED(36)}},
                  .backgroundColor = surface,
                  .border = {.color = invalid              ? danger
                                      : p->focus == id + 2 ? accent
                                                           : line,
                             .width = outline_width},
                  .custom = {.customData = (void *)(uintptr_t)(index + 1)}})
            {
            }
            const char *unit = flow_slider_unit(f);
            if (unit[0]) {
                label(unit, 0, enabled ? secondary : muted);
            }
        }
        if (invalid) {
            text_wrapped(format(p, "%s", validation), danger);
        }
        if (!p->draft_context && p->edit[index].dirty && !(f->flags & READ_ONLY)) {
            button(p, id + 1, "Apply", enabled, false, A_APPLY, index);
        }
        flow_note(p, (size_t)index);
    }
}
static void flow_combo(Panel *p, size_t index)
{
    const FieldSpec *f = &fields[index];
    uint32_t id = 1000 + (uint32_t)index * 3;
    bool enabled = flow_enabled(p, index);
    Widget *w = widget(p, id, W_SELECT, A_DROPDOWN, enabled);
    if (w) {
        w->index = (int)index;
        w->draft = p->draft_context;
    }
    float available = p->open_menu ? fminf(360, p->width - 32) - 72 : fminf(p->width, 520) - 90;
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(36)},
                     .padding = {10, 26, 0, 0},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = hot(id) && enabled ? hovered : surface,
          .border = {.color = p->focus == id ? accent : line, .width = outline_width}})
    {
        const char *value = flow_value(p, index);
        const char *empty = !strcmp(f->key, "capture.monitor")        ? "Selected monitor"
                            : !strcmp(f->key, "audio.mic_source")     ? "Default microphone"
                            : !strcmp(f->key, "audio.desktop_source") ? "Choose desktop source"
                                                                      : "Choose camera";
        label(fit_text(p, value[0] ? value : empty, available, 1), 1, enabled ? foreground : muted);
    }
    (void)f;
}
static void flow_color(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index < 0) {
        return;
    }
    const FieldSpec *f = &fields[index];
    uint32_t id = 1000 + (uint32_t)index * 3;
    bool enabled = flow_enabled(p, (size_t)index);
    const char *value = flow_value(p, (size_t)index);
    char validation[CAST_ERR];
    bool invalid = validate_field(f, value, validation, sizeof validation) != 0;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .padding = {0, 0, 6, 6}}})
    {
        label(f->label, 1, secondary);
        Widget *w = widget(p, id, W_COLOR, A_COLOR, enabled);
        if (w) {
            w->index = index;
            w->draft = p->draft_context;
        }
        CLAY({.id = element_id(id),
              .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(36)},
                         .padding = {10, 10, 0, 0},
                         .childGap = 10,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = hot(id) && enabled ? hovered : surface,
              .border = {.color = invalid          ? danger
                                  : p->focus == id ? accent
                                                   : line,
                         .width = outline_width}})
        {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(20),
                                        .height = CLAY_SIZING_FIXED(20)}},
                  .custom = {.customData = (void *)(uintptr_t)(FLOW_DATA_BASE + FLOW_COLOR_DATA +
                                                               (index << 4))}})
            {
            }
            label(fit_text(p, value, fminf(p->width, 520) - 102, 1), 1,
                  enabled ? foreground : muted);
        }
        if (invalid) {
            text_wrapped(format(p, "%s", validation), danger);
        }
        flow_note(p, (size_t)index);
    }
}
/* Coupled connection, capture and encoding fields retain their atomic Apply.
 * Independent composition controls commit at the end of the interaction. */
static bool field_applies_immediately(const FieldSpec *field)
{
    static const char *const grouped[] = {
        "capture.x",
        "capture.y",
        "capture.width",
        "capture.height",
        "zoom.min",
        "zoom.max",
        "composition.layout_order",
        "composition.preset_order",
        "camera.corner_order",
        "record.container",
        "record.video_codec",
        "record.audio_codec",
        "record.preset",
    };
    if (field->flags & (STREAM_LOCK | READ_ONLY)) {
        return false;
    }
    for (size_t i = 0; i < sizeof grouped / sizeof *grouped; i++) {
        if (!strcmp(field->key, grouped[i])) {
            return false;
        }
    }
    return true;
}
static void flow_field_control(Panel *p, size_t index)
{
    const FieldSpec *f = &fields[index];
    if (f->type == FIELD_COLOR) {
        flow_color(p, f->key);
        return;
    }
    if (flow_slider_key(f->key)) {
        flow_slider(p, f->key);
        return;
    }
    uint32_t id = 1000 + (uint32_t)index * 3;
    bool enabled = flow_enabled(p, index);
    const char *value = flow_value(p, index);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .padding = {0, 0, 6, 8}}})
    {
        label(f->label, 1, secondary);
        if (f->type == FIELD_BOOL) {
            CLAY({.layout = {.childGap = 0}})
            {
                flow_choice(p, id, "On", index, "true", !strcmp(value, "true"), 60, 30, W_BUTTON);
                flow_choice(p, id + 2, "Off", index, "false", !strcmp(value, "false"), 60, 30,
                            W_BUTTON);
            }
        } else if (f->type == FIELD_ENUM) {
            flow_combo(p, index);
        } else {
            Widget *w = widget(p, id, W_FIELD, A_FIELD, enabled);
            if (w) {
                w->index = (int)index;
                w->draft = p->draft_context;
            }
            char validation[CAST_ERR];
            bool invalid =
                p->edit[index].dirty && validate_field(f, value, validation, sizeof validation);
            CLAY({.id = element_id(id),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                        .height = CLAY_SIZING_FIXED(36)}},
                  .backgroundColor = surface,
                  .border = {.color = invalid          ? danger
                                      : p->focus == id ? accent
                                                       : line,
                             .width = outline_width},
                  .custom = {.customData = (void *)(uintptr_t)(index + 1)}})
            {
            }
            if (!p->draft_context && p->edit[index].dirty && !(f->flags & READ_ONLY)) {
                button(p, id + 1, "Apply", enabled, false, A_APPLY, (int)index);
            }
        }
        flow_note(p, index);
    }
}
static void flow_chips(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index < 0) {
        return;
    }
    const char *current = flow_value(p, (size_t)index);
    int count = choice_count(fields[index].choices);
    float width = fminf(108, (fminf(p->width, 520) - 44 - (count - 1) * 6) / count);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .padding = {0, 0, 6, 8}}})
    {
        label(fields[index].label, 1, secondary);
        CLAY({.layout = {.childGap = 6}})
        {
            for (int i = 0; i < count; i++) {
                char value[128];
                choice_value(fields[index].choices, i, value, sizeof value);
                char display[sizeof value];
                snprintf(display, sizeof display, "%s", value);
                if (display[0] >= 'a' && display[0] <= 'z') {
                    display[0] -= 'a' - 'A';
                }
                flow_choice(p, 8000 + (uint32_t)index * 16 + (uint32_t)i,
                            fit_text(p, format(p, "%s", display), width - 12, 0), (size_t)index,
                            value, !strcmp(current, value), width, 30, W_BUTTON);
            }
        }
        flow_note(p, (size_t)index);
    }
}
static void flow_device_picker(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index < 0) {
        return;
    }
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .padding = {0, 0, 6, 8}}})
    {
        label(fields[index].label, 1, secondary);
        flow_combo(p, (size_t)index);
        flow_note(p, (size_t)index);
    }
}
static const char *const flow_anchors[] = {
    "top-left", "top", "top-right", "left", NULL, "right", "bottom-left", "bottom", "bottom-right"};
static void flow_anchor_grid(Panel *p)
{
    int index = find_field("camera.anchor");
    const char *current = flow_value(p, (size_t)index);
    CLAY({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4}})
    {
        for (int row = 0; row < 3; row++) {
            CLAY({.layout = {.childGap = 4}})
            {
                for (int col = 0; col < 3; col++) {
                    int cell = row * 3 + col;
                    uint32_t id = 8400 + (uint32_t)cell;
                    if (flow_anchors[cell]) {
                        flow_choice(p, id, !strcmp(current, flow_anchors[cell]) ? "✓" : "",
                                    (size_t)index, flow_anchors[cell],
                                    !strcmp(current, flow_anchors[cell]), 40, 40, W_ANCHOR);
                    } else {
                        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(40),
                                                    .height = CLAY_SIZING_FIXED(40)},
                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                            .y = CLAY_ALIGN_Y_CENTER}},
                              .backgroundColor = surface})
                        {
                            icon_slot(ICON_SCREEN, false, false);
                        }
                    }
                }
            }
        }
    }
    text_wrapped(format(p, "Anchor: %s", current), secondary);
    flow_note(p, (size_t)index);
}
static void flow_layout_thumbnails(Panel *p)
{
    int index = find_field("composition.layout");
    const char *current = flow_value(p, (size_t)index);
    const char *values[] = {"overlay", "stage", "split", "screen", "camera"};
    const char *titles[] = {"Overlay", "Stage", "Split", "Screen", "Camera"};
    int columns = (int)((fminf(p->width, 520) - 32) / 80);
    for (int start = 0; start < 5; start += columns) {
        CLAY({.layout = {.childGap = 8, .padding = {0, 0, 0, 8}}})
        {
            for (int i = start; i < 5 && i < start + columns; i++) {
                uint32_t id = 8500 + (uint32_t)i;
                bool selected = !strcmp(current, values[i]);
                Widget *w = widget(p, id, W_BUTTON, A_SETTING, flow_enabled(p, (size_t)index));
                if (w) {
                    w->index = index;
                    w->draft = p->draft_context;
                    snprintf(w->value, sizeof w->value, "%s", values[i]);
                }
                CLAY({.id = element_id(id),
                      .layout = {.sizing = {.width = CLAY_SIZING_FIXED(72)},
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 6},
                      .border = {.color = selected || p->focus == id ? accent : line,
                                 .width = outline_width}})
                {
                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(72),
                                                .height = CLAY_SIZING_FIXED(44)}},
                          .backgroundColor = hot(id) ? hovered : surface,
                          .custom = {.customData =
                                         (void *)(uintptr_t)(FLOW_DATA_BASE + FLOW_LAYOUT_DATA +
                                                             (i << 4))}})
                    {
                    }
                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                                     .padding = {0, 0, 0, 6},
                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                    {
                        label(titles[i], 0, selected ? foreground : secondary);
                    }
                }
            }
        }
    }
}
static void flow_matrix(Panel *p)
{
    const char *columns[] = {"VC", "Rec", "Stream"};
    const char *keys[] = {"annotations.virtual_clicks", "annotations.record_clicks",
                          "annotations.stream_clicks",  "annotations.virtual_keys",
                          "annotations.record_keys",    "annotations.stream_keys"};
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6}})
    {
        for (int row = -1; row < 2; row++) {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                             .childGap = 8,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
            {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
                {
                    label(row < 0    ? "Output"
                          : row == 0 ? "Click indicators"
                                     : "Keystrokes",
                          0, secondary);
                }
                for (int col = 0; col < 3; col++) {
                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(48)},
                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                    {
                        if (row < 0) {
                            label(columns[col], 0, secondary);
                        } else {
                            int index = find_field(keys[row * 3 + col]);
                            bool selected = !strcmp(flow_value(p, (size_t)index), "true");
                            flow_choice(p, 8700 + (uint32_t)(row * 3 + col), selected ? "✓" : "",
                                        (size_t)index, selected ? "false" : "true", selected, 28,
                                        28, W_BUTTON);
                        }
                    }
                }
            }
        }
    }
    if (!p->snapshot.capabilities.input) {
        text_wrapped("Input annotations are unavailable with this capture backend.", secondary);
    }
}
static void flow_capture_kind(Panel *p)
{
    const char *values[] = {"monitor", "region", "window"};
    const char *titles[] = {"Monitor", "Region", "Window"};
    CLAY({.layout = {.childGap = 6}})
    {
        for (int i = 0; i < 3; i++) {
            bool supported_kind = i == 0 || (i == 1 ? p->snapshot.capabilities.region_selection
                                                    : p->snapshot.capabilities.window_selection);
            button(p, 8800 + (uint32_t)i, titles[i], p->snapshot.connected && supported_kind,
                   !strcmp(p->snapshot.config.capture_kind, values[i]), A_COMMAND, 0);
            Widget *w = &p->widgets[p->widget_count - 1];
            w->arg[0] = "capture";
            w->arg[1] = values[i];
            w->argc = 2;
            if (i) {
                w->arg[2] = "select";
                w->argc = 3;
            }
        }
    }
}
static void flow_overflow(Panel *p, int lane)
{
    uint32_t id = 8600 + (uint32_t)lane;
    Widget *w = widget(p, id, W_BUTTON, A_MENU, true);
    if (w) {
        w->index = lane;
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(28), .height = CLAY_SIZING_FIXED(28)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = hot(id) ? hovered : surface,
          .border = {.color = p->focus == id ? accent : line, .width = outline_width}})
    {
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14),
                                    .height = CLAY_SIZING_FIXED(14)}},
              .custom = {.customData = (void *)(uintptr_t)(FLOW_DATA_BASE + FLOW_OVERFLOW_DATA)}})
        {
        }
    }
}
static void flow_overflow_layout(Panel *p)
{
    if (!p->open_menu) {
        return;
    }
    Clay_ElementData anchor = Clay_GetElementData(element_id(8600 + (uint32_t)p->open_menu - 1));
    if (!anchor.found) {
        p->open_menu = 0;
        return;
    }
    float width = fminf(360, p->width - 32);
    float height = fminf(p->open_menu == 1 ? 186 : p->open_menu == 3 ? 190 : 420, p->height - 150);
    float x = fmaxf(
        16, fminf(anchor.boundingBox.x + anchor.boundingBox.width - width, p->width - width - 16));
    float y = anchor.boundingBox.y + anchor.boundingBox.height + 4;
    if (y + height > p->height - 58) {
        y = fmaxf(16, anchor.boundingBox.y - height - 4);
    }
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsScroll"));
    if (viewport.found) {
        Clay_ElementData draft = Clay_GetElementData(CLAY_ID("DraftBar"));
        if (draft.found && draft.boundingBox.y > viewport.boundingBox.y) {
            viewport.boundingBox.height =
                fminf(viewport.boundingBox.height, draft.boundingBox.y - viewport.boundingBox.y);
        }
        height = fminf(height, viewport.boundingBox.height);
        y = fmaxf(viewport.boundingBox.y,
                  fminf(y, viewport.boundingBox.y + viewport.boundingBox.height - height));
    }
    CLAY({.id = CLAY_ID("LaneMenu"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width),
                                .height = CLAY_SIZING_FIXED(height)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .padding = {12, 12, 12, 12}},
          .backgroundColor = surface,
          .border = {.color = line, .width = outline_width},
          .floating = {.offset = {x, y}, .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 8},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        lane_menu_body(p, p->open_menu - 1);
    }
}

static void named_field(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index >= 0) {
        field_row(p, (size_t)index);
    }
}
static void flow_overflow(Panel *p, int lane);
static void flow_slider(Panel *p, const char *key);
static void flow_device_picker(Panel *p, const char *key);

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
    bool enabled =
        p->snapshot.connected && (lane != 2 || p->snapshot.stream.state == STREAM_STREAMING);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
    {
        command_button(p, freeze_id, frozen ? "Unfreeze" : "Freeze", enabled, commands[lane],
                       frozen ? "unfreeze" : "freeze", NULL);
        command_button(p, blur_id, blurred ? "Blur off" : "Blur", enabled, commands[lane], "blur",
                       blurred ? "off" : "on");
        if (lane != 1) {
            command_button(p, 139 + (uint32_t)lane, lane == 2 ? "Disconnect" : "Stop",
                           p->snapshot.connected, commands[lane], "stop", NULL);
        }
    }
}
static void composition_link(Panel *p)
{
    const Config *c = &p->snapshot.config;
    uint32_t id = 152;
    Widget *w = widget(p, id, W_LINK, A_SECTION, true);
    if (w) {
        w->index = 0;
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {0, 0, 2, 2}},
          .border = {.color = p->focus == id ? accent : surface,
                     .width = p->focus == id ? outline_width : (Clay_BorderWidth){0}}})
    {
        label(fit_text(
                  p, format(p, "%s · %s", c->monitor[0] ? c->monitor : c->capture_kind, c->layout),
                  fminf(p->width, 520) - 64, 0),
              0, hot(id) ? foreground : secondary);
    }
}
static void lane_body(Panel *p, int lane)
{
    const PanelSnapshot *s = &p->snapshot;
    bool connected = s->connected;
    if (lane == 0) {
        composition_link(p);
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
                                                  : "Pause",
                       connected, "virtual",
                       !s->config.virtual_enabled ? "start"
                       : s->state.virtual_paused  ? "resume"
                                                  : "pause",
                       NULL);
        if (s->config.virtual_enabled) {
            effect_buttons(p, lane);
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
            label(fit_text(p, s->config.record_dir, fminf(p->width, 520) - 64, 0), 0, muted);
            text_wrapped("Start begins the configured countdown; cut removes media time.", muted);
            command_button(p, 32, "Start recording", connected, "record", "start", NULL);
        } else {
            uint64_t seconds = s->duration_ns / 1000000000ULL;
            label(fit_text(p,
                           format(p, "%02llu:%02llu · %s", (unsigned long long)(seconds / 60),
                                  (unsigned long long)(seconds % 60), s->state.record_path),
                           fminf(p->width, 520) - 64, 0),
                  0, muted);
            if (s->state.record_cut) {
                text_wrapped("Media clock stopped · file kept. Resume continues the same file.",
                             muted);
                command_button(p, 33,
                               format(p, "Resume — %d s countdown", s->config.record_countdown),
                               connected, "record", "resume", NULL);
                command_button(p, 140, "Stop recording", connected, "record", "stop", NULL);
            } else {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
                {
                    command_button(p, 33, s->state.record_paused ? "Resume" : "Pause", connected,
                                   "record", s->state.record_paused ? "resume" : "pause", NULL);
                    command_button(p, 39, "Cut", connected, "record", "cut", NULL);
                    command_button(p, 140, "Stop", connected, "record", "stop", NULL);
                }
                effect_buttons(p, lane);
            }
        }
    } else if (lane == 2) {
        bool configured = s->config.stream.server_url[0] && s->config.stream.key_file[0];
        if (s->stream.state == STREAM_FAILED) {
            text_wrapped(s->stream.error[0] ? s->stream.error : "Connection failed.", danger);
            command_button(p, 130, "Retry connection", connected && configured, "stream", "start",
                           NULL);
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
            {
                button(p, 150, "Streaming setup…", true, false, A_SETUP, 1);
                command_button(p, 141, "Stop", connected, "stream", "stop", NULL);
            }
        } else if (s->stream.state == STREAM_CONNECTING || s->stream.state == STREAM_RECONNECTING) {
            text_wrapped(s->stream.state == STREAM_RECONNECTING
                             ? format(p, "Retrying %u · current privacy state retained.",
                                      s->stream.retry_attempt)
                             : "Connecting — viewers will receive the paused screen.",
                         muted);
            command_button(p, 130, "Cancel", connected, "stream", "stop", NULL);
        } else if (s->stream.state == STREAM_STOPPING) {
            text_wrapped("Closing the streaming connection.", muted);
            command_button(p, 130, "Disconnecting…", false, "stream", "stop", NULL);
        } else if (s->stream.active) {
            text_wrapped(
                s->state.stream_paused
                    ? "Connected — sending paused screen. Resume streaming reveals the composition."
                : s->state.stream_blurred ? "Connected — sending blurred composition and silence."
                : s->state.stream_frozen  ? "Connected — sending held frame and silence."
                                          : "Connected — sending composition.",
                muted);
            command_button(p, 130, s->state.stream_paused ? "Resume streaming" : "Pause",
                           connected && s->stream.state == STREAM_STREAMING, "stream",
                           s->state.stream_paused ? "resume" : "pause", NULL);
            effect_buttons(p, lane);
        } else if (!configured) {
            text_wrapped("No destination configured yet.", muted);
            button(p, 150, "Set up streaming…", true, true, A_SETUP, 1);
        } else {
            text_wrapped("Start opens privacy-paused. Reveal the composition separately.", muted);
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .childGap = 6}})
            {
                command_button(p, 130, "Start streaming", connected, "stream", "start", NULL);
                button(p, 150, "Edit setup", true, false, A_SETUP, 1);
            }
        }
    } else {
        label(fit_text(p, s->config.mic_source[0] ? s->config.mic_source : "Default microphone",
                       fminf(p->width, 520) - 64, 0),
              0, muted);
        text_wrapped("What the audience hears — check before revealing anything.", muted);
        p->draft_context = false;
        named_field(p, "audio.mic");
        named_field(p, "audio.desktop");
        named_field(p, "audio.virtual");
    }
}
static void lane_menu_body(Panel *p, int lane)
{
    p->draft_context = lane == 1 || lane == 3;
    if (lane == 0) {
        button(p, 151, "Camera composition…", true, false, A_SECTION, 1);
        text_wrapped("Geometry, shape and mirror live on the Camera page — shared by every layout "
                     "that shows the camera.",
                     muted);
    } else if (lane == 1) {
        text_wrapped("Cut omits media time; resume continues the same file.", muted);
        flow_slider(p, "record.countdown");
        named_field(p, "record.directory");
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (!strncmp(fields[i].key, "record.", 7) &&
                strcmp(fields[i].key, "record.countdown") &&
                strcmp(fields[i].key, "record.directory")) {
                field_row(p, i);
            }
        }
    } else if (lane == 2) {
        label(fit_text(p,
                       format(p, "Server: %s",
                              p->snapshot.config.stream.server_url[0]
                                  ? p->snapshot.config.stream.server_url
                                  : "not configured"),
                       fminf(p->width - 32, 360) - 32, 0),
              0, muted);
        label(fit_text(p,
                       format(p, "Key file: %s",
                              p->snapshot.config.stream.key_file[0]
                                  ? p->snapshot.config.stream.key_file
                                  : "not configured"),
                       fminf(p->width - 32, 360) - 32, 0),
              0, muted);
        button(p, 154, "Streaming setup…", true, false, A_SETUP, 1);
    } else {
        flow_device_picker(p, "audio.mic_source");
        flow_device_picker(p, "audio.desktop_source");
        flow_slider(p, "audio.mic_gain");
        flow_slider(p, "audio.desktop_gain");
        named_field(p, "audio.virtual_name");
        text_wrapped("Disappeared sources never fall back to a broad capture. Cast cannot mute a "
                     "microphone the conferencing app selected — use the app's own mute.",
                     muted);
    }
    p->draft_context = false;
}
/* Prototype sections regroup existing controls without discarding advanced fields. */
static int field_section(const FieldSpec *f)
{
    const char *key = f->key;
    if (f->tab == TAB_STREAM) {
        return 7; /* The dedicated setup sheet owns all streaming drafts. */
    }
    if (f->tab == TAB_AUDIO) {
        return 6; /* Operate's Audio card and its one-level overflow. */
    }
    if (f->tab == TAB_CAMERA) {
        return 1;
    }
    if (f->tab == TAB_EFFECTS) {
        return !strncmp(key, "logo.", 5) || !strncmp(key, "text.", 5) ? 3 : 4;
    }
    if (f->tab == TAB_SOURCE) {
        return !strncmp(key, "screen.", 7) || !strncmp(key, "background.", 11) ? 2 : 0;
    }
    if (!strncmp(key, "record.", 7)) {
        return 8; /* Recording card overflow, never the styling page. */
    }
    return f->group >= 4 ? 5 : 0;
}
static bool field_in_section(const FieldSpec *f, int section)
{
    return field_section(f) == section;
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
    (void)p;
    if (section == 2) {
        text_wrapped("Screen margins, radius, borders and camera-slot backdrop use the same "
                     "session-only controls; selections apply immediately.",
                     muted);
    } else if (section == 3) {
        text_wrapped("Overlays sit above sources; pause replaces them, freeze/blur include them. "
                     "Static text is literal; placeholders are not expanded.",
                     muted);
    } else if (section == 4) {
        text_wrapped("Clicks and keystrokes are composed once and shown per output. Input observed "
                     "during privacy pause is discarded, never replayed.",
                     muted);
    } else if (section == 5) {
        text_wrapped("Shared styling: every output that is paused or blurred shows the same text, "
                     "colours and font.",
                     muted);
    }
}
static const char *section_summary(Panel *p, int section)
{
    const Config *c = &p->snapshot.config;
    switch (section) {
    case 0:
        return format(
            p, "%s · %s · %s",
            !strcmp(c->capture_kind, "monitor") && c->monitor[0] ? c->monitor : c->capture_kind,
            c->layout, p->snapshot.current_preset[0] ? p->snapshot.current_preset : "preset —");
    case 1:
        return format(p, "%s · %s · %.0f%%%s", c->shape, c->anchor, c->camera_width_percent,
                      c->mirror ? " · mirror" : "");
    case 2:
        return format(p, "screen %s · screen %.0f%% wide", c->screen_background,
                      c->screen_width_percent);
    case 3:
        return format(p, "Logo %s · Text %s", c->logo_enabled ? "on" : "off",
                      c->text_enabled ? "on" : "off");
    case 4: {
        unsigned clicks = c->annotations_virtual_clicks + c->annotations_record_clicks +
                          c->annotations_stream_clicks;
        unsigned keys =
            c->annotations_virtual_keys + c->annotations_record_keys + c->annotations_stream_keys;
        return format(p, "cursor %s · clicks %u/3 · keys %u/3", c->cursor ? "on" : "off", clicks,
                      keys);
    }
    default:
        return "pause & blur screen text — shared styling for every output";
    }
}
static void section_list(Panel *p)
{
    float width = fminf(p->width, 520) - 32 - 24;
    for (int section = 0; section < 6; section++) {
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
/* Essential controls are explicit; the inventory-driven tail retains every existing knob. */
static const char *compose_value(Panel *p, const char *key)
{
    int index = find_field(key);
    if (index < 0) {
        return "";
    }
    if (p->edit[index].dirty) {
        return p->edit[index].value;
    }
    char value[PATH_MAX];
    field_value(&fields[index], &p->snapshot.config, value, sizeof value);
    return format(p, "%s", value);
}
static void compose_group(const char *title)
{
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}, .padding = {0, 0, 14, 4}},
          .border = {.color = line, .width = {.top = 1}}})
    {
        label(title, 2, foreground);
    }
}
static void essential_field(Panel *p, const char *key, bool shown[FIELD_COUNT])
{
    int index = find_field(key);
    if (index >= 0) {
        shown[index] = true;
        named_field(p, key);
    }
}
static void mark_compose_field(bool shown[FIELD_COUNT], const char *key)
{
    int index = find_field(key);
    if (index >= 0) {
        shown[index] = true;
    }
}
static void missing_resource(Panel *p, const char *key, bool path_only)
{
    const char *path = compose_value(p, key);
    if (!*path || (path_only && !strchr(path, '/')) || access(path, R_OK) == 0) {
        return;
    }
    if (*path == '/') {
        text_wrapped(format(p,
                            "%s — %s. Apply remains available; a rejected resource keeps "
                            "the current configuration and outputs unchanged.",
                            path,
                            errno == ENOENT || errno == ENOTDIR ? "not found" : "not readable"),
                     danger);
    } else {
        text_wrapped(format(p,
                            "%s — not accessible from the panel working directory; the daemon "
                            "resolves the path from its own working directory.",
                            path),
                     muted);
    }
}
static bool active_backdrop_field(Panel *p, const FieldSpec *f)
{
    const char *mode = compose_value(p, "screen.background");
    if (!strncmp(f->key, "background.gradient_", 20)) {
        if (strcmp(mode, "gradient")) {
            return false;
        }
        if ((!strcmp(f->key, "background.gradient_via") ||
             !strcmp(f->key, "background.gradient_waypoint")) &&
            strcmp(compose_value(p, "background.gradient_via_enabled"), "true")) {
            return false;
        }
    }
    if ((!strcmp(f->key, "background.source") || !strcmp(f->key, "screen.background_blur_radius") ||
         !strcmp(f->key, "screen.background_brightness")) &&
        strcmp(mode, "blurred")) {
        return false;
    }
    return strcmp(f->key, "screen.background_color") || !strcmp(mode, "solid");
}
static void source_essentials(Panel *p, bool shown[FIELD_COUNT])
{
    flow_capture_kind(p);
    if (!strcmp(p->snapshot.config.capture_kind, "monitor")) {
        flow_device_picker(p, "capture.monitor");
    }
    mark_compose_field(shown, "capture.monitor");
    text_wrapped("Interactive selection keeps the previous source on Esc.", muted);
    compose_group("Layout");
    flow_layout_thumbnails(p);
    const char *layout = compose_value(p, "composition.layout");
    text_wrapped(!strcmp(layout, "stage")
                     ? "Stage places the screen opposite the camera anchor. When sizes overlap, "
                       "the camera renders in front."
                 : !strcmp(layout, "split")
                     ? "Screen and camera sit side by side; split side and ratio set their shares."
                 : !strcmp(layout, "screen")
                     ? "The Screen layout shows only the screen — the camera is not part of it."
                 : !strcmp(layout, "camera")
                     ? "Camera layout fills the frame with the camera alone."
                     : "Screen fills the frame with the camera overlaid at its chosen anchor.",
                 muted);
    mark_compose_field(shown, "composition.layout");
    CLAY({.layout = {.childGap = 6}})
    {
        command_button(p, 90, "Previous", p->snapshot.connected, "layout", "prev", NULL);
        command_button(p, 91, "Next", p->snapshot.connected, "layout", "next", NULL);
    }
    compose_group("Preset");
    CLAY({.layout = {.childGap = 6}})
    {
        button(p, 40, "Preset", p->snapshot.connected && p->snapshot.config.preset_count > 0, false,
               A_DROPDOWN, -1);
        p->widgets[p->widget_count - 1].type = W_SELECT;
        command_button(p, 92, "Previous", p->snapshot.connected, "preset", "prev", NULL);
        command_button(p, 41, "Next", p->snapshot.connected, "preset", "next", NULL);
    }
    text_wrapped("Composition only — never touches outputs.", muted);
}
static void compose_essentials(Panel *p, int section, bool shown[FIELD_COUNT])
{
    if (section == 0) {
        source_essentials(p, shown);
    } else if (section == 1) {
        essential_field(p, "camera.visible", shown);
        compose_group("Position");
        flow_anchor_grid(p);
        const char *layout = compose_value(p, "composition.layout");
        const char *anchor = compose_value(p, "camera.anchor");
        if (!strcmp(layout, "screen")) {
            text_wrapped("The Screen layout shows only the screen — the camera is not part of "
                         "it. These settings are kept and apply again when you switch layouts.",
                         muted);
        } else if (!strcmp(layout, "camera")) {
            text_wrapped("Camera layout fills the frame with the camera alone. Position and size "
                         "below apply to Overlay, Stage and Split.",
                         muted);
        } else if (!strcmp(layout, "stage")) {
            const char *opposite = !strcmp(anchor, "top-left")       ? "bottom-right"
                                   : !strcmp(anchor, "top")          ? "bottom"
                                   : !strcmp(anchor, "top-right")    ? "bottom-left"
                                   : !strcmp(anchor, "left")         ? "right"
                                   : !strcmp(anchor, "right")        ? "left"
                                   : !strcmp(anchor, "bottom-left")  ? "top-right"
                                   : !strcmp(anchor, "bottom")       ? "top"
                                   : !strcmp(anchor, "bottom-right") ? "top-left"
                                                                     : "automatic";
            text_wrapped(format(p,
                                "Camera sits %s · screen sits opposite (%s). When sizes "
                                "overlap, the camera renders in front.",
                                anchor, opposite),
                         muted);
        } else {
            text_wrapped(!strcmp(layout, "split")
                             ? "Split uses the camera-side and split-ratio controls in All "
                               "settings; the anchor is retained for Overlay and Stage."
                             : "The camera sits at its chosen anchor over the screen; size is its "
                               "width as a percentage of the canvas.",
                         muted);
        }
        mark_compose_field(shown, "camera.anchor");
        compose_group("Size");
        flow_slider(p, "camera.width_percent");
        mark_compose_field(shown, "camera.width_percent");
        compose_group("Shape");
        flow_chips(p, "camera.shape");
        mark_compose_field(shown, "camera.shape");
    } else if (section == 2) {
        compose_group("Screen backdrop");
        flow_chips(p, "screen.background");
        mark_compose_field(shown, "screen.background");
        const char *mode = compose_value(p, "screen.background");
        if (!strcmp(mode, "blurred")) {
            compose_group("Blurred backdrop");
            flow_chips(p, "background.source");
            mark_compose_field(shown, "background.source");
            text_wrapped("Blurs the chosen source behind the composition.", muted);
        } else if (!strcmp(mode, "gradient")) {
            compose_group("Gradient colours");
            essential_field(p, "background.gradient_from", shown);
            essential_field(p, "background.gradient_to", shown);
            essential_field(p, "background.gradient_via_enabled", shown);
            if (!strcmp(compose_value(p, "background.gradient_via_enabled"), "true")) {
                essential_field(p, "background.gradient_via", shown);
            }
        } else {
            compose_group("Solid colour");
            essential_field(p, "screen.background_color", shown);
        }
        compose_group("Stage — screen width");
        flow_slider(p, "screen.width_percent");
        mark_compose_field(shown, "screen.width_percent");
    } else if (section == 3) {
        compose_group("Logo");
        essential_field(p, "logo.enabled", shown);
        essential_field(p, "logo.path", shown);
        missing_resource(p, "logo.path", false);
        compose_group("Static text");
        essential_field(p, "text.enabled", shown);
        essential_field(p, "text.content", shown);
    } else if (section == 4) {
        essential_field(p, "cursor.enabled", shown);
        compose_group("Shown on each output");
        flow_matrix(p);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (!strncmp(fields[i].key, "annotations.", 12)) {
                shown[i] = true;
            }
        }
    } else {
        compose_group("Pause screen");
        essential_field(p, "output.pause_text", shown);
        compose_group("Blur screen");
        essential_field(p, "output.blur_title", shown);
    }
    section_actions(p, section);
}
static void compose_all(Panel *p, int section, const bool shown[FIELD_COUNT])
{
    const char *names[TAB_COUNT][GROUP_COUNT] = {
        {"Capture & zoom", "Capture & exclusion", "Zoom behavior", "Stage sizes",
         "Screen appearance", "Background gradient"},
        {"Camera", "Position & crop", "Camera appearance", "Device"},
        {"Audio", "Virtual microphone"},
        {"Annotations", "Output annotations", "Cursor & clicks", "Keystroke style", "Logo",
         "Static text"},
        {"Recording countdown", "Recording format", "Cycle order", "Output & connection",
         "Pause screen", "Blur screen"},
        {"Streaming setup", "Advanced network"}};
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        for (int group = 0; group < GROUP_COUNT; group++) {
            bool any = false;
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                any |= !shown[i] && fields[i].tab == tab && fields[i].group == group &&
                       field_section(&fields[i]) == section &&
                       (section != 2 || active_backdrop_field(p, &fields[i]));
            }
            if (!any) {
                continue;
            }
            compose_group(names[tab][group] ? names[tab][group] : "More settings");
            if (tab == TAB_SOURCE && group == 1) {
                text_wrapped("Mask covers the panel and preview with your chosen color. For "
                             "unobstructed capture, select an application window or keep the "
                             "controls outside the captured monitor or region.",
                             secondary);
            }
            if (tab == TAB_CAMERA && group == 1) {
                text_wrapped("Free positioning is selected via cast camera anchor free. The "
                             "coordinates below adjust that existing position; drag-in-preview "
                             "is not available.",
                             muted);
            }
            if (section == 2 && tab == TAB_SOURCE && group == 4) {
                text_wrapped("When the screen and camera sizes overlap, the camera renders in "
                             "front. Aspect ratios are preserved.",
                             muted);
            }
            if (tab == TAB_SETTINGS && group >= 4) {
                text_wrapped("Title, subtitle and footer are optional. Templates: {date}, {time}, "
                             "{datetime:%A, %d %B · %H:%M}.",
                             muted);
                text_wrapped(format(p,
                                    "Font is config-only: output.%s_font = %s; reload the "
                                    "config file.",
                                    group == 4 ? "pause" : "blur",
                                    group == 4 ? p->snapshot.config.pause_font
                                               : p->snapshot.config.blur_font),
                             muted);
            }
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (!shown[i] && fields[i].tab == tab && fields[i].group == group &&
                    field_section(&fields[i]) == section &&
                    (section != 2 || active_backdrop_field(p, &fields[i]))) {
                    if (!strcmp(fields[i].key, "camera.device")) {
                        flow_device_picker(p, fields[i].key);
                    } else {
                        field_row(p, i);
                    }
                    if (!strcmp(fields[i].key, "text.font")) {
                        missing_resource(p, "text.font", true);
                    }
                }
            }
        }
    }
    if (section == 0 || section == 5) {
        command_button(p, 85, "Reload config file", p->snapshot.connected, "config", "reload",
                       NULL);
        text_wrapped("Reload preserves each lane's pause/freeze/blur flags and never resumes "
                     "outputs. Navigation and drafts are session only.",
                     muted);
    }
    if (section == 4) {
        command_button(p, 84, "Clear now", p->snapshot.connected, "keys", "clear", NULL);
    }
}
static void compose_body(Panel *p)
{
    if (p->open_section < 0) {
        section_list(p);
        return;
    }
    int section = p->open_section;
    bool shown[FIELD_COUNT] = {false};
    p->draft_context = true;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 10, 18},
                     .childGap = 6,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        compose_essentials(p, section, shown);
        if (p->all_settings[section]) {
            compose_all(p, section, shown);
        }
        button(p, 180 + (uint32_t)section, p->all_settings[section] ? "Essentials" : "All settings",
               true, false, A_ALL, section);
    }
}
static void stream_setup_body(Panel *p)
{
    p->draft_context = true;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {16, 16, 10, 18},
                     .childGap = 6,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        text_wrapped("The key is read from the file at connect time; the panel never displays it.",
                     muted);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (fields[i].tab == TAB_STREAM) {
                field_row(p, i);
            }
        }
        text_wrapped("Keep the stream key in a regular file only you can read (600). Advanced "
                     "network controls are set in [stream] of the configuration file. Stop "
                     "streaming to change connection settings.",
                     muted);
        button(p, 153, "Cancel", true, false, A_SETUP, 0);
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
        label(fit_text(p, title, fminf(p->width, 520) - 170, 3), 3, foreground);
    }
}
static void operate_body(Panel *p)
{
    p->draft_context = false;
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                     .padding = {14, 14, 12, 16},
                     .childGap = 8,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        command_button(p, 34, p->snapshot.state.group_paused ? "Resume outputs" : "Pause all",
                       p->snapshot.connected, p->snapshot.state.group_paused ? "resume" : "pause",
                       NULL, NULL);
        for (int lane = 0; lane < 4; lane++) {
            CLAY({.id = CLAY_IDI("LaneCard", lane),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                             .padding = {14, 14, 12, 12},
                             .childGap = 8,
                             .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .backgroundColor = surface,
                  .border = {.color = line, .width = outline_width}})
            {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                                 .childGap = 6,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
                {
                    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
                    {
                        label(lane_names[lane], 3, foreground);
                    }
                    state_marker(lane < 3 ? state_color(&p->snapshot, lane) : secondary);
                    label(fit_text(p, lane_status(p, lane),
                                   lane == 3 ? 90 : fmaxf(42, fminf(p->width, 520) - 308), 0),
                          0, lane < 3 ? state_color(&p->snapshot, lane) : secondary);
                    flow_overflow(p, lane);
                }
                lane_body(p, lane);
            }
        }
    }
}
static void pinned_drafts(Panel *p)
{
    int section = p->view == VIEW_SETUP     ? 7
                  : p->view == VIEW_SECTION ? p->open_section
                  : p->open_menu == 2       ? 8
                  : p->open_menu == 4       ? 6
                                            : -1;
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
static void status_token(Panel *p, uint32_t id, const char *text, Action action, int target,
                         float budget)
{
    Widget *w = widget(p, id, W_LINK, action, true);
    if (w) {
        w->index = target;
        if (action == A_COMMAND) {
            w->arg[0] = "zoom";
            w->arg[1] = "reset";
            w->argc = 2;
            w->enabled = p->snapshot.connected;
        }
    }
    CLAY({.id = element_id(id),
          .layout = {.sizing = {.height = CLAY_SIZING_FIXED(16)},
                     .childGap = 4,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .border = {.color = p->focus == id ? accent : surface,
                     .width = p->focus == id ? outline_width : (Clay_BorderWidth){0}}})
    {
        if (action == A_LANE) {
            state_marker(state_color(&p->snapshot, target));
        }
        quiet_label(fit_text(p, text, budget - (action == A_LANE ? 11 : 0), FONT_QUIET),
                    hot(id) ? foreground : secondary, false);
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
    const char *message = error[0]           ? error
                          : p->picking_color ? "Pick a screen color · Escape cancels"
                          : s->command_queued > s->command_completed
                              ? "Waiting for daemon acknowledgement…"
                          : p->reply[0] ? p->reply
                                        : "Ready — daemon is running independently.";
    const char *connection = s->connected               ? "Connected"
                             : p->navigation_generation ? "Reconnecting…"
                                                        : "Disconnected";
    uint64_t seconds = s->duration_ns / 1000000000ULL;
    const char *tokens[] = {
        s->config.monitor[0] ? s->config.monitor : s->config.capture_kind,
        s->config.layout,
        s->current_preset[0] ? s->current_preset : "preset —",
        format(p, "zoom %.2f× ×", s->config.zoom_factor),
        format(p, "cam %s", virtual_status(s)),
        s->state.recording ? format(p, "rec %02llu:%02llu", (unsigned long long)(seconds / 60),
                                    (unsigned long long)(seconds % 60))
                           : format(p, "rec %s", record_status(s)),
        format(p, "stream %s",
               s->stream.state == STREAM_STREAMING ? "Connected" : panel_stream_status(s))};
    float budgets[] = {64, 52, 64, 66, 72, 76, 104};
    TTF_Font *font = text_font(p, FONT_QUIET, false);
    for (int i = 0; i < 7; i++) {
        int width = 0, height = 0;
        TTF_GetStringSize(font, tokens[i], strlen(tokens[i]), &width, &height);
        budgets[i] = fminf(budgets[i], (float)width + (i >= 4 ? 11 : 0));
    }
    bool show[] = {true, true, true, s->config.zoom_factor > 1, true, true, true};
    /* Keep composition links useful; shed optional detail before lane shortcuts. */
    const int drop[] = {2, 3, 6, 5, 4};
    for (size_t next = 0; next <= sizeof drop / sizeof drop[0]; next++) {
        float wanted = 0;
        int count = 0;
        for (int i = 0; i < 7; i++) {
            if (show[i]) {
                wanted += budgets[i];
                count++;
            }
        }
        wanted += (count - 1) * 14;
        if (wanted <= available || next == sizeof drop / sizeof drop[0]) {
            break;
        }
        show[drop[next]] = false;
    }
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
                         .childGap = 5,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
        {
            bool first = true;
            for (int i = 0; i < 7; i++) {
                if (!show[i]) {
                    continue;
                }
                if (!first) {
                    quiet_label("·", muted, false);
                }
                first = false;
                status_token(p, 160 + (uint32_t)i, tokens[i],
                             i >= 4   ? A_LANE
                             : i == 3 ? A_COMMAND
                                      : A_SECTION,
                             i >= 4 ? i - 4 : 0, budgets[i]);
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
static int flow_option_count(const Panel *p)
{
    return p->dropdown_field < 0 ? p->snapshot.config.preset_count
           : fields[p->dropdown_field].type == FIELD_TEXT
               ? p->device_count
               : choice_count(fields[p->dropdown_field].choices);
}
static void flow_option_value(Panel *p, int choice, char *value, size_t length)
{
    if (p->dropdown_field < 0) {
        snprintf(value, length, "%s", p->snapshot.config.presets[choice].name);
    } else if (fields[p->dropdown_field].type == FIELD_TEXT) {
        snprintf(value, length, "%s", p->device_values[choice]);
    } else {
        choice_value(fields[p->dropdown_field].choices, choice, value, length);
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
    int count = flow_option_count(p);
    float width = fmaxf(200, anchor.boundingBox.width);
    width = fminf(width, p->width - 32);
    float height = fminf(260, fmaxf(40, 32 * count + 8));
    float x = fmaxf(16, fminf(anchor.boundingBox.x, p->width - width - 16));
    float y = anchor.boundingBox.y + anchor.boundingBox.height + 4;
    if (y + height > p->height - 58) {
        y = fmaxf(16, anchor.boundingBox.y - height - 4);
    }
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsScroll"));
    if (viewport.found) {
        Clay_ElementData draft = Clay_GetElementData(CLAY_ID("DraftBar"));
        if (draft.found && draft.boundingBox.y > viewport.boundingBox.y) {
            viewport.boundingBox.height =
                fminf(viewport.boundingBox.height, draft.boundingBox.y - viewport.boundingBox.y);
        }
        height = fminf(height, viewport.boundingBox.height);
        y = fmaxf(viewport.boundingBox.y,
                  fminf(y, viewport.boundingBox.y + viewport.boundingBox.height - height));
    }
    CLAY({.id = CLAY_ID("Dropdown"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width),
                                .height = CLAY_SIZING_FIXED(height)},
                     .padding = {4, 4, 4, 4},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = surface,
          .border = {.color = line, .width = outline_width},
          .floating = {.offset = {x, y}, .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 12},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        if (!count) {
            text_wrapped(p->device_request ? "Discovering devices…" : "No devices found.",
                         secondary);
        }
        for (int i = 0; i < count; i++) {
            char value[256];
            flow_option_value(p, i, value, sizeof value);
            bool selected = i == p->dropdown_choice;
            const char *name =
                p->dropdown_field >= 0 && fields[p->dropdown_field].type == FIELD_TEXT
                    ? p->device_labels[i]
                    : value;
            Widget *w = widget(p, 400 + (uint32_t)i, W_OPTION, A_OPTION, true);
            if (w) {
                w->index = i;
                snprintf(w->value, sizeof w->value, "%s", value);
            }
            CLAY({.id = element_id(400 + (uint32_t)i),
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(),
                                        .height = CLAY_SIZING_FIXED(32)},
                             .padding = {8, 8, 0, 0},
                             .childGap = 8,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = selected                 ? (Clay_Color){35, 63, 83, 255}
                                     : hot(400 + (uint32_t)i) ? hovered
                                                              : surface})
            {
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()}}})
                {
                    label(fit_text(p, format(p, "%s", name), width - 54, 1), 1, foreground);
                }
                if (selected) {
                    label("✓", 0, accent);
                }
            }
        }
    }
}
static const uint32_t flow_swatches[40] = {
    0x000000, 0x101113, 0x181a1d, 0x272a2e, 0x444951, 0x979ea8, 0xbbc1ca, 0xf8f9fb,
    0x172554, 0x1e3a8a, 0x1d4ed8, 0x2563eb, 0x3b82f6, 0x60a5fa, 0x80c9ff, 0xdbeafe,
    0x052e16, 0x14532d, 0x166534, 0x16a34a, 0x22c55e, 0x4ade80, 0x86efac, 0xdcfce7,
    0x450a0a, 0x7f1d1d, 0xb91c1c, 0xdc2626, 0xef4444, 0xff9693, 0xfecaca, 0xfff1f2,
    0x451a03, 0x78350f, 0xb45309, 0xd97706, 0xf59e0b, 0xfbbf24, 0xfde68a, 0xfffbeb};
static Clay_Color flow_hex_color(uint32_t value)
{
    return (Clay_Color){(float)((value >> 16) & 255), (float)((value >> 8) & 255),
                        (float)(value & 255), 255};
}
static void flow_color_layout(Panel *p)
{
    if (!p->color_popup) {
        return;
    }
    Clay_ElementData anchor = Clay_GetElementData(element_id(p->color_popup));
    if (!anchor.found) {
        p->color_popup = 0;
        return;
    }
    float width = 304, height = 348;
    float x = fmaxf(16, fminf(anchor.boundingBox.x, p->width - width - 16));
    float y = anchor.boundingBox.y + anchor.boundingBox.height + 4;
    if (y + height > p->height - 58) {
        y = fmaxf(16, anchor.boundingBox.y - height - 4);
    }
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsScroll"));
    if (viewport.found) {
        Clay_ElementData draft = Clay_GetElementData(CLAY_ID("DraftBar"));
        if (draft.found && draft.boundingBox.y > viewport.boundingBox.y) {
            viewport.boundingBox.height =
                fminf(viewport.boundingBox.height, draft.boundingBox.y - viewport.boundingBox.y);
        }
        height = fminf(height, viewport.boundingBox.height);
        y = fmaxf(viewport.boundingBox.y,
                  fminf(y, viewport.boundingBox.y + viewport.boundingBox.height - height));
    }
    CLAY({.id = CLAY_ID("ColorPopup"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width),
                                .height = CLAY_SIZING_FIXED(height)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .padding = {12, 12, 12, 12}},
          .backgroundColor = surface,
          .border = {.color = line, .width = outline_width},
          .floating = {.offset = {x, y}, .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 12},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        for (int row = 0; row < 5; row++) {
            CLAY({.layout = {.childGap = 4}})
            {
                for (int col = 0; col < 8; col++) {
                    int swatch = row * 8 + col;
                    uint32_t id = 6000 + (uint32_t)swatch;
                    Widget *w =
                        widget(p, id, W_BUTTON, A_SWATCH, flow_enabled(p, (size_t)p->color_field));
                    if (w) {
                        w->index = p->color_field;
                        w->draft = true;
                        snprintf(w->value, sizeof w->value, "#%06x", flow_swatches[swatch]);
                    }
                    CLAY({.id = element_id(id),
                          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(31),
                                                .height = CLAY_SIZING_FIXED(28)}},
                          .backgroundColor = flow_hex_color(flow_swatches[swatch]),
                          .border = {.color = p->focus == id || hot(id) ? accent : line,
                                     .width = outline_width}})
                    {
                    }
                }
            }
        }
        int index = p->color_field;
        uint32_t id = 6500;
        Widget *w = widget(p, id, W_FIELD, A_FIELD, flow_enabled(p, (size_t)index));
        if (w) {
            w->index = index;
            w->draft = true;
        }
        char validation[CAST_ERR];
        bool invalid = validate_field(&fields[index], p->edit[index].value, validation,
                                      sizeof validation) != 0;
        CLAY({.id = element_id(id),
              .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(36)}},
              .backgroundColor = background,
              .border = {.color = invalid          ? danger
                                  : p->focus == id ? accent
                                                   : line,
                         .width = outline_width},
              .custom = {.customData = (void *)(uintptr_t)(index + 1)}})
        {
        }
        if (invalid) {
            text_wrapped("Enter a valid #RRGGBB color.", danger);
        }
        button(p, 6501, "Pick from screen", flow_enabled(p, (size_t)index), false, A_PICK_COLOR,
               index);
        text_wrapped("Click a screen color; Escape or right-click cancels.", secondary);
    }
}

static void quit_confirmation_layout(Panel *p)
{
    if (!p->quit_confirmation) {
        return;
    }
    float width = fminf(408, p->width - 32);
    CLAY({.id = CLAY_ID("QuitBackdrop"),
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(p->width),
                                .height = CLAY_SIZING_FIXED(p->height)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = {0, 0, 0, 180},
          .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 20}})
    {
        CLAY({.id = CLAY_ID("QuitConfirmation"),
              .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width)},
                         .padding = {20, 20, 20, 20},
                         .childGap = 14,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = surface,
              .border = {.color = line, .width = outline_width}})
        {
            label("Quit Cast?", 3, foreground);
            text_wrapped("This stops the virtual camera and streaming, finishes and saves any "
                         "recording, and closes the daemon, preview and panel.",
                         foreground);
            text_wrapped("Close panel keeps Cast running in the background.", secondary);
            if (p->error[0]) {
                text_wrapped(p->error, danger);
            }
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW()},
                             .childGap = 8,
                             .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
            {
                button(p, 9100, "Cancel", true, false, A_QUIT_CANCEL, 0);
                button(p, 9101, "Stop and quit", !p->daemon_stopping, false, A_QUIT_CONFIRM, 0);
            }
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
                    stream_setup_body(p);
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
    p->menu_start = p->widget_count;
    flow_overflow_layout(p);
    p->menu_end = p->widget_count;
    p->dropdown_start = p->widget_count;
    dropdown_layout(p);
    p->dropdown_end = p->widget_count;
    p->color_start = p->widget_count;
    flow_color_layout(p);
    p->color_end = p->widget_count;
    quit_confirmation_layout(p);
    Clay_RenderCommandArray commands = Clay_EndLayout();
    if (p->restore_scroll) {
        Clay_ScrollContainerData body = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
        if (body.found) {
            body.scrollPosition->y = p->view_scroll[scroll_index(p)];
            p->restore_scroll = false;
        }
    }
    if (p->lane_jump && p->view == VIEW_OPERATE) {
        Clay_ElementData card =
            Clay_GetElementData(CLAY_IDI("LaneCard", (uint32_t)p->lane_jump - 1));
        Clay_ElementData area = Clay_GetElementData(CLAY_ID("SettingsScroll"));
        Clay_ScrollContainerData body = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
        if (card.found && area.found && body.found) {
            body.scrollPosition->y =
                fminf(0, body.scrollPosition->y + area.boundingBox.y - card.boundingBox.y);
            p->lane_jump = 0;
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
    case ICON_CHEVRON_UP:
        L(4, 10, 8, 6);
        L(8, 6, 12, 10);
        break;
    case ICON_EYEDROPPER:
        L(2, 11, 8, 5);
        L(5, 14, 11, 8);
        L(2, 11, 2, 14);
        L(2, 14, 5, 14);
        L(7, 4, 12, 9);
        L(9, 3, 13, 7);
        L(9, 3, 11, 1);
        L(11, 1, 15, 5);
        L(15, 5, 13, 7);
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
    if (p->active_text == id || (field_is_numeric(&fields[index]) && p->active_text == id + 2) ||
        (p->active_text == 6500 && p->color_field == (int)index)) {
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
    draw_text(p, text, strlen(text), 1, value[0] && flow_enabled(p, index) ? foreground : muted, x,
              y);
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
/* Inter's bundled face has proportional default numerals. Keep native value
 * labels stable by centering each cached digit in the widest digit advance;
 * punctuation and units retain their normal advances from the same face. */
static void flow_draw_value(Panel *p, SDL_FRect box, size_t index)
{
    TTF_Font *font = text_font(p, 0, false);
    int digit_width = 0, height;
    for (char digit = '0'; digit <= '9'; digit++) {
        int advance;
        TTF_GetStringSize(font, &digit, 1, &advance, &height);
        if (advance > digit_width) {
            digit_width = advance;
        }
    }
    double value = strtod(p->edit[index].value, NULL);
    const char *text = flow_slider_label(p, index, value);
    size_t length = strlen(text);
    float width = 0;
    for (size_t offset = 0; offset < length;) {
        size_t next = offset + 1;
        while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80) {
            next++;
        }
        int advance;
        TTF_GetStringSize(font, text + offset, next - offset, &advance, &height);
        width += text[offset] >= '0' && text[offset] <= '9' ? digit_width : advance;
        offset = next;
    }
    float x = box.x + box.w - width;
    float y = box.y + (box.h - TTF_GetFontHeight(font)) / 2;
    Clay_Color ink = flow_enabled(p, index) ? foreground : muted;
    for (size_t offset = 0; offset < length;) {
        size_t next = offset + 1;
        while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80) {
            next++;
        }
        int advance;
        TTF_GetStringSize(font, text + offset, next - offset, &advance, &height);
        bool digit = text[offset] >= '0' && text[offset] <= '9';
        float cell = digit ? digit_width : advance;
        draw_text(p, text + offset, next - offset, 0, ink, x + (cell - advance) / 2, y);
        x += cell;
        offset = next;
    }
}

static void flow_draw_control(Panel *p, SDL_FRect box, uintptr_t data)
{
    unsigned value = (unsigned)(data - FLOW_DATA_BASE);
    unsigned kind = value & 15;
    unsigned index = value >> 4;
    if (kind == FLOW_VALUE_DATA && index < FIELD_COUNT) {
        flow_draw_value(p, box, index);
    } else if (kind == FLOW_OVERFLOW_DATA) {
        for (int i = 0; i < 3; i++) {
            rounded(p, (SDL_FRect){box.x + i * 5, box.y + 6, 3, 3}, 1.5f, foreground);
        }
    } else if (kind == FLOW_PENDING_DATA) {
        int bright = (int)((SDL_GetTicks() / 220) % 3);
        for (int i = 0; i < 3; i++) {
            rounded(p, (SDL_FRect){box.x + i * 5, box.y + 6, 3, 3}, 1.5f,
                    i == bright ? accent : muted);
        }
    } else if (kind == FLOW_SLIDER_DATA && index < FIELD_COUNT) {
        double minimum, maximum;
        flow_slider_range(p, index, &minimum, &maximum);
        double current = strtod(p->edit[index].value, NULL);
        float ratio = (float)fmax(0, fmin(1, (current - minimum) / (maximum - minimum)));
        float x = box.x + 7, y = box.y + box.h / 2;
        float track = box.w - 14;
        rounded(p, (SDL_FRect){x, y - 2, track, 4}, 2, control);
        rounded(p, (SDL_FRect){x, y - 2, ratio * track, 4}, 2,
                flow_enabled(p, index) ? accent : muted);
        float thumb = x + ratio * track;
        rounded(p, (SDL_FRect){thumb - 7, y - 7, 14, 14}, 7, surface);
        color(p, flow_enabled(p, index) ? accent : muted);
        circle(p, thumb, y, 6.5f, 1.5f);
    } else if (kind == FLOW_COLOR_DATA && index < FIELD_COUNT) {
        const char *value_text = p->edit[index].value;
        uint32_t ink = (uint32_t)strtoul(value_text + (value_text[0] == '#'), NULL, 16);
        rounded(p, box, 0, flow_hex_color(ink));
        outline(p, box, 0, line);
    } else if (kind == FLOW_LAYOUT_DATA && index < 5) {
        SDL_FRect screen = {box.x + 4, box.y + 4, box.w - 8, box.h - 8};
        SDL_FRect camera = {box.x + box.w * .72f, box.y + box.h * .62f, box.w * .20f, box.h * .30f};
        if (index == 1) {
            screen =
                (SDL_FRect){box.x + box.w * .09f, box.y + box.h * .08f, box.w * .82f, box.h * .62f};
        } else if (index == 2) {
            screen = (SDL_FRect){box.x + 3, box.y + 6, box.w * .56f, box.h - 12};
            camera = (SDL_FRect){box.x + box.w * .66f, box.y + 6, box.w * .30f, box.h - 12};
        } else if (index == 4) {
            camera = (SDL_FRect){box.x + 6, box.y + 4, box.w - 12, box.h - 8};
        }
        if (index != 4) {
            rounded(p, screen, 2, control);
            outline(p, screen, 2, secondary);
        }
        if (index != 3) {
            rounded(p, camera, index == 0 || index == 1 ? fminf(camera.w, camera.h) / 2 : 3,
                    (Clay_Color){35, 63, 83, 255});
            outline(p, camera, index == 0 || index == 1 ? fminf(camera.w, camera.h) / 2 : 3,
                    accent);
        }
    }
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
            if ((uintptr_t)c->renderData.custom.customData >= FLOW_DATA_BASE &&
                (uintptr_t)c->renderData.custom.customData < FLOW_DATA_BASE + 65536) {
                flow_draw_control(p, box, (uintptr_t)c->renderData.custom.customData);
                break;
            }
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
static bool flow_modal_widget(const Panel *p, const Widget *w)
{
    int position = (int)(w - p->widgets);
    if (p->quit_confirmation) {
        return w->id == 9100 || w->id == 9101;
    }
    if (p->color_popup) {
        return position >= p->color_start && position < p->color_end;
    }
    if (p->dropdown) {
        return position >= p->dropdown_start && position < p->dropdown_end;
    }
    if (p->open_menu) {
        return (position >= p->menu_start && position < p->menu_end) ||
               (w->id >= 700 && w->id <= 717);
    }
    return true;
}
static bool widget_in_scroll(const Panel *p, const Widget *w)
{
    (void)p;
    return w->id != 5 && w->id != 6 && w->id != 7 && w->id != 8 && w->id != 9 && w->id != 22 &&
           w->id != 9100 && w->id != 9101 && !(w->id >= 10 && w->id <= 12) && w->id != 99 &&
           !(w->id >= 160 && w->id <= 166) && !(w->id >= 700 && w->id <= 717) &&
           w->type != W_OPTION;
}
static void cancel_editing(Panel *p)
{
    p->active_text = 0;
    p->select_all = false;
    SDL_StopTextInput(p->window);
}
static void stop_editing(Panel *p)
{
    Widget *active = find_widget(p, p->active_text);
    if (active && active->type == W_FIELD && field_applies_immediately(&fields[active->index])) {
        commit_field(p, active->index);
    }
    cancel_editing(p);
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
    if (widget_in_scroll(p, w) || w->type == W_OPTION) {
        Clay_ElementId container = p->color_popup ? CLAY_ID("ColorPopup")
                                   : p->dropdown  ? CLAY_ID("Dropdown")
                                   : p->open_menu ? CLAY_ID("LaneMenu")
                                                  : CLAY_ID("SettingsScroll");
        Clay_ElementData area = Clay_GetElementData(container);
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(container);
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
        if (w->enabled && flow_modal_widget(p, w)) {
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
    p->pending_button = p->focus;
    p->pending_button_request = ++p->snapshot.command_queued;
    if (draft) {
        p->edit[index].pending = p->snapshot.command_queued;
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
static void commit_field(Panel *p, int index)
{
    if (index < 0 || (size_t)index >= FIELD_COUNT || !field_applies_immediately(&fields[index])) {
        return;
    }
    FieldEdit *edit = &p->edit[index];
    if (!edit->dirty || !writable(p, &fields[index])) {
        edit->auto_apply = false;
        return;
    }
    if (validate_field(&fields[index], edit->value, p->error, sizeof p->error)) {
        edit->auto_apply = false;
        return;
    }
    if (edit->pending) {
        edit->auto_apply = edit->revision != edit->submitted_revision;
        return;
    }
    if (p->snapshot.command_queued > p->snapshot.command_completed) {
        edit->auto_apply = true;
        return;
    }
    edit->auto_apply = false;
    /* The settings path preserves camera geometry when changing its width. */
    if (!strcmp(fields[index].key, "camera.width_percent")) {
        if (!panel_client_setting(p->client, fields[index].key, edit->value, p->error,
                                  sizeof p->error)) {
            edit->pending = ++p->snapshot.command_queued;
            edit->submitted_revision = edit->revision;
            p->pending_button = p->focus;
            p->pending_button_request = edit->pending;
            p->reply[0] = 0;
        }
    } else {
        apply_setting(p, index, edit->value, true);
    }
}
static void choose_field(Panel *p, int index, const char *value)
{
    stage_field(p, index, value);
    commit_field(p, index);
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
    p->pending_button = p->focus;
    p->pending_button_request = ++p->snapshot.command_queued;
    for (size_t i = 0; i < submitted; i++) {
        FieldEdit *edit = &p->edit[indexes[i]];
        edit->pending = p->snapshot.command_queued;
        edit->submitted_revision = edit->revision;
    }
}
static void flow_slider_update(Panel *p, Widget *w, float x)
{
    if (!w || !w->enabled || w->type != W_SLIDER) {
        return;
    }
    const FieldSpec *f = &fields[w->index];
    float track = fmaxf(1, w->box.width - 14);
    double ratio = fmax(0, fmin(1, (x - w->box.x - 7) / track));
    double minimum, maximum;
    flow_slider_range(p, (size_t)w->index, &minimum, &maximum);
    double value = minimum + ratio * (maximum - minimum);
    double step = flow_slider_step(f);
    value = fmax(minimum, fmin(maximum, round(value / step) * step));
    char formatted[64];
    snprintf(formatted, sizeof formatted, "%.6g", value);
    if (strcmp(formatted, p->edit[w->index].value)) {
        stage_field(p, w->index, formatted);
    }
}
static void flow_slider_commit(Panel *p, Widget *w)
{
    if (!w || w->type != W_SLIDER || !p->edit[w->index].dirty) {
        return;
    }
    commit_field(p, w->index);
}
static bool flow_popup_contains(Panel *p, float x, float y, const char *name)
{
    (void)p;
    Clay_ElementData data = Clay_GetElementData(Clay_GetElementId(literal(name)));
    return data.found && x >= data.boundingBox.x && y >= data.boundingBox.y &&
           x < data.boundingBox.x + data.boundingBox.width &&
           y < data.boundingBox.y + data.boundingBox.height;
}
static void flow_discover_devices(Panel *p, int index)
{
    p->device_count = 0;
    const char *key = fields[index].key;
    const char *args[] = {!strcmp(key, "camera.device")     ? "camera"
                          : !strcmp(key, "capture.monitor") ? "screen"
                                                            : "audio",
                          "list"};
    if (!panel_client_command(p->client, 2, args, p->error, sizeof p->error)) {
        p->device_request = ++p->snapshot.command_queued;
    }
}
static void flow_read_devices(Panel *p, const PanelSnapshot *fresh)
{
    if (!p->device_request || fresh->command_completed < p->device_request) {
        return;
    }
    p->device_request = 0;
    if (!p->dropdown || p->dropdown_field < 0 || fresh->command_failed) {
        return;
    }
    const char *key = fields[p->dropdown_field].key;
    if (!strcmp(key, "audio.mic_source")) {
        p->device_values[0][0] = 0;
        snprintf(p->device_labels[0], sizeof p->device_labels[0], "Default microphone");
        p->device_count = 1;
    }
    const char *row = fresh->last_reply;
    while (*row && p->device_count < 64) {
        const char *end = strchr(row, '\n');
        if (!end) {
            end = row + strlen(row);
        }
        const char *start = row;
        while (start < end && (*start == ' ' || *start == '*')) {
            start++;
        }
        const char *name_end = start;
        while (name_end < end && *name_end != ' ') {
            name_end++;
        }
        bool matches = name_end > start;
        if (!strcmp(key, "camera.device")) {
            matches = matches && !strncmp(start, "/dev/video", 10);
        } else if (strncmp(key, "audio.", 6) == 0) {
            const char *kind = strstr(start, "class=");
            bool source = kind && !strncmp(kind + 6, "Audio/Source", 12);
            bool desktop = kind && (!strncmp(kind + 6, "Audio/Sink", 10) ||
                                    !strncmp(kind + 6, "Stream/Output/Audio", 19));
            matches = matches && (!strcmp(key, "audio.mic_source") ? source : desktop);
        }
        if (matches) {
            int i = p->device_count++;
            snprintf(p->device_values[i], sizeof p->device_values[i], "%.*s",
                     (int)(name_end - start), start);
            snprintf(p->device_labels[i], sizeof p->device_labels[i], "%.*s",
                     (int)fmin((double)(end - start), 255), start);
            if (!strcmp(p->device_values[i], p->edit[p->dropdown_field].value)) {
                p->dropdown_choice = i;
            }
        }
        row = *end ? end + 1 : end;
    }
}

static void start_color_pick(Panel *p)
{
    p->pick_requested = false;
    if (p->quit || !writable(p, &fields[p->pick_field])) {
        return;
    }
    if (panel_color_pick_begin(p->color_pick, SDL_GetCurrentVideoDriver(), NULL)) {
        snprintf(p->error, sizeof p->error, "%s", panel_color_pick_error(p->color_pick));
    } else {
        p->color_popup = 0;
        p->picking_color = true;
        p->error[0] = 0;
        p->reply[0] = 0;
    }
}

static void activate(Panel *p, Widget *w)
{
    if (!w || !w->enabled) {
        return;
    }
    switch (w->action) {
    case A_ALL:
        if (w->index >= 0 && w->index < 6) {
            p->all_settings[w->index] = !p->all_settings[w->index];
        }
        break;
    case A_MENU:
        stop_editing(p);
        p->dropdown = 0;
        p->color_popup = 0;
        p->open_menu = p->open_menu == w->index + 1 ? 0 : w->index + 1;
        break;
    case A_COLOR:
        stop_editing(p);
        p->dropdown = 0;
        p->color_popup = p->color_popup == w->id ? 0 : w->id;
        p->color_field = w->index;
        break;
    case A_SWATCH:
        stop_editing(p);
        choose_field(p, w->index, w->value);
        break;
    case A_PICK_COLOR:
        stop_editing(p);
        p->pick_field = w->index;
        if (p->mouse_down) {
            /* SDL owns an implicit pointer grab until the opening button is
             * released. The picker uses its own X11 connection. */
            p->pick_requested = true;
        } else {
            start_color_pick(p);
        }
        break;
    case A_CLOSE:
        p->quit = true;
        break;
    case A_QUIT_APP:
        cancel_editing(p);
        p->dropdown = p->color_popup = 0;
        p->open_menu = 0;
        p->quit_confirmation = true;
        p->focus = 9100;
        break;
    case A_QUIT_CANCEL:
        p->quit_confirmation = false;
        p->focus = 9;
        break;
    case A_DAEMON:
    case A_QUIT_CONFIRM:
        cancel_editing(p);
        p->error[0] = 0;
        if (w->action == A_DAEMON && !p->snapshot.connected && !p->lifecycle_state.child_pid) {
            if (!panel_lifecycle_start(p->lifecycle, &p->snapshot.config, p->error,
                                       sizeof p->error)) {
                p->daemon_stopped = false;
                snprintf(p->reply, sizeof p->reply, "Starting daemon…");
            }
            break;
        }
        if (p->snapshot.connected) {
            const char *args[] = {"quit"};
            if (panel_client_command(p->client, 1, args, p->error, sizeof p->error)) {
                break;
            }
            p->shutdown_request = ++p->snapshot.command_queued;
            p->pending_button = w->id;
            p->pending_button_request = p->shutdown_request;
        } else if (p->lifecycle_state.child_pid &&
                   panel_lifecycle_stop_owned(p->lifecycle, p->error, sizeof p->error)) {
            break;
        }
        p->daemon_stopping = true;
        p->quit_after_stop = w->action == A_QUIT_CONFIRM;
        p->shutdown_started = SDL_GetTicks();
        p->quit_confirmation = false;
        snprintf(p->reply, sizeof p->reply, "Stopping outputs and saving recording…");
        break;
    case A_COMMAND:
        p->error[0] = 0;
        if (!panel_client_command(p->client, w->argc, w->arg, p->error, sizeof p->error)) {
            p->pending_button = w->id;
            p->pending_button_request = ++p->snapshot.command_queued;
            p->reply[0] = 0;
        }
        break;
    case A_SETTING:
        choose_field(p, w->index, w->value);
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
        p->color_popup = 0;
        if (w->index >= 0 && fields[w->index].type == FIELD_TEXT) {
            flow_discover_devices(p, w->index);
        } else if (w->index >= 0) {
            char value[PATH_MAX], option[128];
            snprintf(value, sizeof value, "%s", flow_value(p, (size_t)w->index));
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
            if (!panel_client_command(p->client, 2, args, p->error, sizeof p->error)) {
                p->pending_button = p->dropdown;
                p->pending_button_request = ++p->snapshot.command_queued;
            }
        } else {
            choose_field(p, p->dropdown_field, w->value);
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
        p->open_lane = w->index;
        navigate_view(p, VIEW_OPERATE, -1);
        p->lane_jump = w->index + 1;
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
        cancel_editing(p);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (field_in_section(&fields[i], w->index)) {
                field_value(&fields[i], &p->snapshot.config, p->edit[i].value,
                            sizeof p->edit[i].value);
                p->edit[i].dirty = false;
                p->edit[i].auto_apply = false;
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
        if (!panel_client_command(p->client, 3, args, p->error, sizeof p->error)) {
            p->pending_button = w->id;
            p->pending_button_request = ++p->snapshot.command_queued;
        }
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
        activate(p, find_widget(p, 9));
        return;
    }
    if (p->quit_confirmation) {
        if (key == SDLK_ESCAPE) {
            p->quit_confirmation = false;
            p->focus = 9;
        } else if (key == SDLK_TAB) {
            move_focus(p, event->mod & SDL_KMOD_SHIFT ? -1 : 1);
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            activate(p, find_widget(p, p->focus));
        }
        return;
    }
    if (key == SDLK_TAB) {
        move_focus(p, event->mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if ((event->mod & SDL_KMOD_ALT) && key >= SDLK_1 && key <= SDLK_6) {
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
        if (p->color_popup) {
            p->focus = p->color_popup;
            p->color_popup = 0;
            cancel_editing(p);
            return;
        }
        if (p->dropdown) {
            p->focus = p->dropdown;
            p->dropdown = 0;
            return;
        }
        if (p->open_menu) {
            p->focus = 8600 + (uint32_t)p->open_menu - 1;
            p->open_menu = 0;
            cancel_editing(p);
            return;
        }
        cancel_editing(p);
        if (p->view == VIEW_SETUP || p->view == VIEW_SECTION) {
            navigate_view(p, p->view == VIEW_SETUP ? p->return_view : VIEW_COMPOSE,
                          p->view == VIEW_SETUP ? p->return_section : -1);
            p->focus = p->main_tab ? 6 : 5;
        }
        return;
    }
    if (p->dropdown &&
        (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT)) {
        int count = flow_option_count(p);
        if (count) {
            int step = key == SDLK_UP || key == SDLK_LEFT ? -1 : 1;
            p->dropdown_choice = (p->dropdown_choice + step + count) % count;
            set_focus(p, find_widget(p, 400 + (uint32_t)p->dropdown_choice));
        }
        return;
    }
    Widget *focused = find_widget(p, p->focus);
    if (focused && focused->enabled && focused->type == W_SLIDER &&
        (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)) {
        const FieldSpec *f = &fields[focused->index];
        double step = flow_slider_step(f);
        if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
            step *= 10;
        }
        double value = strtod(p->edit[focused->index].value, NULL) +
                       ((key == SDLK_LEFT || key == SDLK_PAGEDOWN) ? -step : step);
        char formatted[64];
        snprintf(formatted, sizeof formatted, "%.6g", fmax(f->minimum, fmin(f->maximum, value)));
        stage_field(p, focused->index, formatted);
        flow_slider_commit(p, focused);
        return;
    }
    if (focused && focused->type == W_ANCHOR &&
        (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN)) {
        int cell = (int)focused->id - 8400;
        int step = key == SDLK_LEFT ? -1 : key == SDLK_RIGHT ? 1 : key == SDLK_UP ? -3 : 3;
        for (int i = 0; i < 9; i++) {
            cell = (cell + step + 9) % 9;
            if (cell != 4) {
                set_focus(p, find_widget(p, 8400 + (uint32_t)cell));
                break;
            }
        }
        return;
    }
    if (focused && focused->type == W_ANCHOR &&
        (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)) {
        activate(p, focused);
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
                if (field_applies_immediately(&fields[index])) {
                    commit_field(p, index);
                } else {
                    apply_section(p, field_section(&fields[index]));
                }
            }
            cancel_editing(p);
        }
        return;
    }
    int section = p->view == VIEW_SETUP ? 7 : p->view == VIEW_SECTION ? p->open_section : -1;
    if (!p->dropdown && !p->color_popup && section >= 0 &&
        (key == SDLK_RETURN || key == SDLK_KP_ENTER) && section_dirty(p, section)) {
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
    if (!p->quit_confirmation) {
        const uint32_t header_ids[] = {22, 7, 8, 9};
        for (size_t i = 0; i < sizeof header_ids / sizeof *header_ids; i++) {
            Widget *header = find_widget(p, header_ids[i]);
            if (header && header->enabled && x >= header->box.x && y >= header->box.y &&
                x < header->box.x + header->box.width && y < header->box.y + header->box.height) {
                if (header->id == 22) {
                    stop_editing(p);
                } else {
                    cancel_editing(p);
                }
                p->dropdown = p->color_popup = 0;
                p->open_menu = 0;
                p->focus = header->id;
                activate(p, header);
                return;
            }
        }
    }
    if (p->color_popup && !flow_popup_contains(p, x, y, "ColorPopup")) {
        p->color_popup = 0;
        stop_editing(p);
        return;
    }
    if (p->dropdown && !flow_popup_contains(p, x, y, "Dropdown")) {
        p->dropdown = 0;
        stop_editing(p);
        return;
    }
    bool draft_hit = false;
    for (int i = 0; i < p->widget_count; i++) {
        const Widget *w = &p->widgets[i];
        if (w->id >= 700 && w->id <= 717 && x >= w->box.x && y >= w->box.y &&
            x < w->box.x + w->box.width && y < w->box.y + w->box.height) {
            draft_hit = true;
        }
    }
    if (!p->dropdown && !p->color_popup && p->open_menu && !draft_hit &&
        !flow_popup_contains(p, x, y, "LaneMenu")) {
        p->open_menu = 0;
        stop_editing(p);
        return;
    }
    for (int i = p->widget_count - 1; i >= 0; i--) {
        Widget *w = &p->widgets[i];
        if (!flow_modal_widget(p, w)) {
            continue;
        }
        if (!w->enabled || x < w->box.x || y < w->box.y || x >= w->box.x + w->box.width ||
            y >= w->box.y + w->box.height) {
            continue;
        }
        if (!p->open_menu && !p->color_popup && widget_in_scroll(p, w)) {
            Clay_ElementData area = Clay_GetElementData(CLAY_ID("SettingsScroll"));
            if (y < area.boundingBox.y || y >= area.boundingBox.y + area.boundingBox.height) {
                continue;
            }
        }
        if (w->action == A_REVERT || w->action == A_APPLY_SECTION || w->action == A_APPLY ||
            w->action == A_QUIT_APP || w->action == A_DAEMON) {
            /* An explicit draft action owns the edit. Focus loss must not submit
             * it first, particularly when the user chose Revert. */
            cancel_editing(p);
        }
        set_focus(p, w);
        if (w->type == W_SLIDER) {
            p->slider_drag = w->id;
            flow_slider_update(p, w, x);
        } else {
            activate(p, w);
        }
        return;
    }
    stop_editing(p);
}
static float wheel_delta(const SDL_MouseWheelEvent *wheel)
{
    float direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
    return wheel->y * direction * 6;
}
static void pointer_position(Panel *p, float x, float y)
{
    p->mouse_x = x / p->input_scale;
    p->mouse_y = y / p->input_scale;
    if (p->pointer_needs_sync) {
        /* An external X11 grab can leave SDL's cached button coordinates at the
         * picker trigger. Use actual desktop coordinates until SDL catches up. */
        float global_x, global_y;
        int window_x, window_y;
        SDL_GetGlobalMouseState(&global_x, &global_y);
        SDL_GetWindowPosition(p->window, &window_x, &window_y);
        float actual_x = (global_x - window_x) / p->input_scale;
        float actual_y = (global_y - window_y) / p->input_scale;
        p->pointer_needs_sync =
            fabsf(actual_x - p->mouse_x) > .5f || fabsf(actual_y - p->mouse_y) > .5f;
        p->mouse_x = actual_x;
        p->mouse_y = actual_y;
    }
}

static void event(Panel *p, const SDL_Event *e)
{
    if (p->picking_color && e->type != SDL_EVENT_QUIT &&
        e->type != SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
            panel_color_pick_cancel(p->color_pick);
            p->picking_color = false;
            p->mouse_down = false;
        }
        return;
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        p->quit = true;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        pointer_position(p, e->motion.x, e->motion.y);
        if (p->slider_drag && p->mouse_down) {
            flow_slider_update(p, find_widget(p, p->slider_drag), p->mouse_x);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            p->mouse_down = true;
            pointer_position(p, e->button.x, e->button.y);
            click_event(p, p->mouse_x, p->mouse_y);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            p->mouse_down = false;
            if (p->pick_requested) {
                start_color_pick(p);
            }
            if (p->slider_drag) {
                flow_slider_commit(p, find_widget(p, p->slider_drag));
                p->slider_drag = 0;
            }
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!p->quit_confirmation) {
            p->scroll += wheel_delta(&e->wheel);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        key_event(p, &e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        insert_text(p, e->text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        p->pick_requested = false;
        p->mouse_down = false;
        if (p->slider_drag) {
            flow_slider_commit(p, find_widget(p, p->slider_drag));
        }
        p->slider_drag = 0;
        p->dropdown = 0;
        p->color_popup = 0;
        p->open_menu = 0;
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
        edit->auto_apply = false;
    }
    edit->pending = 0;
}

static void poll_color_pick(Panel *p)
{
    if (!p->picking_color) {
        /* Finish cancelled portal callbacks even when no chooser is open. */
        panel_color_pick_poll(p->color_pick, NULL);
        return;
    }
    if (!p->snapshot.connected || !writable(p, &fields[p->pick_field])) {
        panel_color_pick_cancel(p->color_pick);
        p->picking_color = false;
        return;
    }
    uint8_t rgb[3];
    PanelColorPickStatus status = panel_color_pick_poll(p->color_pick, rgb);
    if (status == PANEL_COLOR_PICK_PENDING) {
        return;
    }
    p->picking_color = false;
    p->mouse_down = false;
    p->pointer_needs_sync = !strcmp(SDL_GetCurrentVideoDriver(), "x11");
    if (status == PANEL_COLOR_PICK_SUCCESS) {
        char value[8];
        snprintf(value, sizeof value, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
        choose_field(p, p->pick_field, value);
    } else if (status == PANEL_COLOR_PICK_ERROR) {
        snprintf(p->error, sizeof p->error, "%s", panel_color_pick_error(p->color_pick));
    }
}

static void poll_client(Panel *p)
{
    PanelSnapshot fresh;
    if (panel_client_snapshot(p->client, &fresh)) {
        if (!fresh.connected) {
            p->pending_button = 0;
            p->pending_button_request = 0;
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
                p->pending_button = 0;
                p->pending_button_request = 0;
                NavigationMemory saved = p->navigation ? *p->navigation : (NavigationMemory){0};
                bool valid =
                    saved.magic == 0x43565033 && saved.session_cookie == cookie &&
                    saved.view >= VIEW_OPERATE && saved.view <= VIEW_SETUP && saved.section >= -1 &&
                    saved.section < 6 && (saved.view != VIEW_SECTION || saved.section >= 0) &&
                    saved.lane >= -1 && saved.lane < 4 && saved.return_view >= VIEW_OPERATE &&
                    saved.return_view <= VIEW_SECTION &&
                    (saved.return_view != VIEW_SECTION ||
                     (saved.return_section >= 0 && saved.return_section < 6));
                memset(p->view_scroll, 0, sizeof p->view_scroll);
                memset(p->all_settings, 0, sizeof p->all_settings);
                p->restore_scroll = true;
                if (valid) {
                    memcpy(p->view_scroll, saved.scroll, sizeof saved.scroll);
                    memcpy(p->all_settings, saved.all_settings, sizeof saved.all_settings);
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
        if (p->pending_button_request && fresh.command_completed >= p->pending_button_request) {
            p->pending_button = 0;
            p->pending_button_request = 0;
        }
        flow_read_devices(p, &fresh);
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
            p->color_popup = 0;
            cancel_editing(p);
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                p->edit[i].auto_apply = false;
                p->edit[i].pending = 0;
            }
        } else if (!p->daemon_stopping && fresh.command_completed >= fresh.command_queued) {
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (p->edit[i].auto_apply && !p->edit[i].pending) {
                    commit_field(p, (int)i);
                    break;
                }
            }
        }
    }
}

static bool daemon_finished(const Panel *p)
{
    if (p->snapshot.connected || p->lifecycle_state.child_pid) {
        return false;
    }
    struct stat st;
    if (lstat(p->snapshot.config.socket_path, &st) && errno == ENOENT) {
        return true;
    }
    /* Crashes can leave the socket behind. The instance lock stays held through
     * encoder cleanup, and is released by the kernel when its producer exits. */
    char lock_path[PATH_MAX];
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", p->snapshot.config.socket_path) >=
        (int)sizeof lock_path) {
        return false;
    }
    int fd = open(lock_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    bool stopped = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                   st.st_nlink == 1 && !(st.st_mode & 0077) && !flock(fd, LOCK_EX | LOCK_NB);
    if (stopped) {
        flock(fd, LOCK_UN);
    }
    close(fd);
    return stopped;
}

static void poll_lifecycle(Panel *p)
{
    panel_lifecycle_poll(p->lifecycle, p->snapshot.connected, &p->lifecycle_state);
    if (p->lifecycle_state.state == PANEL_DAEMON_FAILED && p->lifecycle_state.error[0]) {
        snprintf(p->error, sizeof p->error, "%s", p->lifecycle_state.error);
    }
    if (!p->daemon_stopping) {
        return;
    }
    if (p->quit_after_stop && !p->shutdown_request && p->snapshot.connected) {
        /* Quit can be requested before asynchronous startup has attached. */
        const char *args[] = {"quit"};
        if (panel_client_command(p->client, 1, args, p->error, sizeof p->error)) {
            p->daemon_stopping = p->quit_after_stop = false;
            return;
        }
        p->shutdown_request = ++p->snapshot.command_queued;
    }
    if (p->snapshot.connected && p->shutdown_request &&
        p->snapshot.command_completed >= p->shutdown_request && p->snapshot.command_failed) {
        p->daemon_stopping = p->quit_after_stop = false;
        p->shutdown_request = 0;
        return;
    }
    /* A disconnect can precede encoder finalization. The daemon removes its
     * socket only after durable recording shutdown; an owned child is reaped too. */
    if (daemon_finished(p)) {
        p->daemon_stopping = false;
        p->daemon_stopped = true;
        p->shutdown_request = 0;
        p->pending_button = 0;
        p->pending_button_request = 0;
        snprintf(p->reply, sizeof p->reply, "Daemon stopped.");
        if (p->quit_after_stop) {
            p->quit = true;
        }
    } else if (SDL_GetTicks() - p->shutdown_started > 15000) {
        p->daemon_stopping = p->quit_after_stop = false;
        snprintf(p->error, sizeof p->error, "Daemon did not stop. Retry Stop daemon or Quit.");
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
    panel_color_pick_free(p->color_pick);
    panel_client_close(p->client);
    panel_lifecycle_destroy(p->lifecycle);
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
        const char *key = (w->action == A_SETTING || w->action == A_FIELD || w->action == A_COLOR ||
                           w->action == A_SWATCH || (w->action == A_DROPDOWN && w->index >= 0)) &&
                                  w->index >= 0 && (size_t)w->index < FIELD_COUNT
                              ? fields[w->index].key
                              : "";
        fprintf(file,
                "%s{\"id\":%u,\"type\":%d,\"action\":%d,\"key\":\"%s\",\"enabled\":%s,"
                "\"box\":[%.1f,%.1f,%.1f,%.1f],\"value\":",
                i ? "," : "", w->id, w->type, w->action, key, w->enabled ? "true" : "false",
                w->box.x, w->box.y, w->box.width, w->box.height);
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
    fprintf(
        file,
        ",\"open_menu\":%d,\"dropdown\":%u,\"color_popup\":%u,\"all_settings\":[%s,%s,%s,%s,%s,%s]",
        p->open_menu, p->dropdown, p->color_popup, p->all_settings[0] ? "true" : "false",
        p->all_settings[1] ? "true" : "false", p->all_settings[2] ? "true" : "false",
        p->all_settings[3] ? "true" : "false", p->all_settings[4] ? "true" : "false",
        p->all_settings[5] ? "true" : "false");
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
    fprintf(file, ",\"color_picking\":%s,\"quit_confirmation\":%s,\"daemon_stopping\":%s",
            p->picking_color ? "true" : "false", p->quit_confirmation ? "true" : "false",
            p->daemon_stopping ? "true" : "false");
    fputs(",\"error\":", file);
    json_string(file, p->error[0] ? p->error : p->snapshot.error);
    fputs(",\"exclusion\":", file);
    json_string(file, p->snapshot.exclusion);
    fputs("}\n", file);
    fclose(file);
}

int panel_run_application(const Config *config, int startup_argc, const char *const *startup_argv,
                          bool auto_start, char *error, size_t n)
{
    void *clay_memory = NULL;
    Panel *p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(error, n, "cannot allocate control panel");
        return -1;
    }
    p->color_pick = panel_color_pick_new();
    if (!p->color_pick) {
        snprintf(error, n, "cannot allocate screen color picker");
        cleanup(p, NULL);
        return -1;
    }
    p->snapshot.config = *config;
    p->lifecycle = panel_lifecycle_create(config, startup_argc, startup_argv, error, n);
    if (!p->lifecycle) {
        cleanup(p, NULL);
        return -1;
    }
    open_navigation(p, config);
    p->tab = -1;
    p->open_lane = -1;
    p->open_section = -1;
    p->density = 1;
    p->input_scale = 1;
    SDL_SetHint(SDL_HINT_APP_ID, "org.cast.Panel");
    /* Returning from screen picking should activate the clicked control on the
     * first click, rather than consuming it just to restore SDL mouse focus. */
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
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
    if (auto_start && panel_lifecycle_start(p->lifecycle, config, p->error, sizeof p->error)) {
        /* Keep the window available to explain startup failure and permit retry. */
        p->daemon_stopped = true;
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
        poll_lifecycle(p);
        poll_color_pick(p);
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

int panel_run(const Config *config, char *error, size_t n)
{
    return panel_run_application(config, 0, NULL, false, error, n);
}
