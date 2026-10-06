#ifndef CAST_H
#define CAST_H
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CAST_VERSION "0.10.0"
#ifndef CAST_APPLICATION_NAME
#define CAST_APPLICATION_NAME "cast"
#endif
#define CAST_TEXT 256
#define CAST_ERR 1024
#define CAST_MAX_PRESETS 24
#define CAST_MAX_ARGS 64
#define CAST_IPC_MAX 8192
/* Owned RGBA8 frames. ts_ns is CLOCK_MONOTONIC; stride may exceed width*4. */
typedef struct {
    uint8_t *data;
    int width, height, stride;
    uint64_t ts_ns;
} Frame;
typedef struct {
    char name[64], layout[16], camera_shape[16], camera_anchor[24], camera_aspect[16];
    double camera_width_percent, split_ratio, zoom_factor;
    bool camera_visible;
    unsigned mask;
} Preset;
typedef struct {
    char service[16], server_url[1024], key_file[PATH_MAX], tls_ca_file[PATH_MAX],
        encoder_preset[24], video_encoder[64];
    int video_bitrate_kbps, audio_bitrate_kbps, queue_frames, lag_ms;
    int connect_timeout_ms, write_timeout_ms;
    int reconnect_attempts, reconnect_initial_ms, reconnect_max_ms;
} StreamConfig;
#define CAST_MAX_EXTENSION_SETTINGS 128
typedef struct {
    char section[50], key[64], value[1024];
} ConfigExtensionValue;
typedef struct {
    char backend[16], socket_path[PATH_MAX], output_device[PATH_MAX], camera_device[PATH_MAX];
    int width, height, fps;
    bool virtual_enabled, camera_enabled;
    char layout[16], shape[16], anchor[24], aspect[16], fit[16], split_side[8];
    double camera_width_percent, split_ratio;
    int margin, radius, border_width;
    uint32_t border_color;
    char camera_background[16];
    uint32_t camera_background_color;
    int camera_background_blur_radius;
    double camera_background_brightness;
    double screen_width_percent;
    int screen_margin, screen_radius, screen_border_width;
    uint32_t screen_border_color;
    char screen_background[16];
    uint32_t screen_background_color;
    int screen_background_blur_radius;
    double screen_background_brightness;
    char background_source[16];
    uint32_t gradient_from, gradient_via, gradient_to;
    bool gradient_via_enabled;
    double gradient_angle, gradient_waypoint;
    bool logo_enabled;
    char logo_path[PATH_MAX], logo_anchor[24];
    double logo_width_percent, logo_opacity;
    int logo_margin_x, logo_margin_y;
    bool text_enabled;
    char text_content[256], text_font[256], text_anchor[24];
    int text_size, text_margin_x, text_margin_y;
    uint32_t text_color;
    double text_opacity;
    int camera_x, camera_y, crop_x, crop_y;
    bool camera_visible, mirror;
    char corner_order[128], layout_order[128], preset_order[256], monitor[128];
    char capture_kind[16];
    uint32_t capture_mask_color;
    int region_x, region_y, region_w, region_h;
    double zoom_factor, zoom_min, zoom_max, zoom_step;
    int zoom_transition_ms, zoom_deadzone;
    double zoom_smoothing;
    bool zoom_follow;
    bool cursor, cursor_highlight, clicks, keys;
    int cursor_size, click_duration_ms, click_radius;
    uint32_t cursor_color, click_left_color, click_right_color, click_middle_color;
    bool click_middle;
    char keys_mode[16], keys_position[24], keys_filter[512], keys_navigation[512];
    int keys_font_size, keys_timeout_ms;
    uint32_t keys_color, keys_background;
    bool annotations_virtual_keys, annotations_virtual_clicks, annotations_record_keys,
        annotations_record_clicks, annotations_stream_keys, annotations_stream_clicks;
    bool mic, desktop, virtual_audio;
    char mic_source[256], desktop_source[256], virtual_name[128];
    double mic_gain, desktop_gain;
    char record_dir[PATH_MAX], record_container[24], video_codec[64], audio_codec[64];
    int record_crf, record_countdown, record_queue, record_bitrate_kbps;
    char record_rate_control[16];
    char record_preset[32];
    StreamConfig stream;
    bool preview;
    char preview_target[16];
    uint32_t pause_color;
    char pause_text[128];
    char pause_subtitle[256], pause_footer[256], pause_font[256];
    uint32_t pause_foreground;
    int pause_title_size, pause_subtitle_size, pause_footer_size, pause_text_gap;
    char blur_title[128], blur_subtitle[256], blur_footer[256], blur_font[256];
    uint32_t blur_color, blur_foreground;
    int blur_radius, blur_title_size, blur_subtitle_size, blur_footer_size, blur_text_gap;
    double blur_opacity;
    int ipc_timeout_ms;
    char licensing_file[PATH_MAX], licensing_upgrade_url[1024];
    ConfigExtensionValue extension_settings[CAST_MAX_EXTENSION_SETTINGS];
    unsigned extension_setting_count;
    Preset presets[CAST_MAX_PRESETS];
    int preset_count;
} Config;
typedef struct {
    bool virtual_paused, virtual_frozen, virtual_blurred;
    bool recording, record_paused, record_frozen, record_blurred, record_cut, group_paused;
    bool group_virtual_restore, group_record_restore, group_stream_restore;
    bool stream_active, stream_paused, stream_frozen, stream_blurred;
    int stream_connection_state; /* StreamLifecycle; presentation flags remain independent. */
    uint64_t record_started_ns, record_paused_ns, record_pause_total_ns;
    char record_path[PATH_MAX], last_error[CAST_ERR];
    uint64_t dropped_frames;
} State;
typedef struct {
    bool capture, cursor_metadata, embedded_cursor, input, region_selection, window_selection,
        preview, panel_exclusion;
    char description[256];
} Capabilities;
typedef struct {
    int x, y;
    bool valid;
    uint64_t ts_ns;
} Cursor;
typedef struct Platform Platform;
typedef struct Compositor Compositor;
typedef struct Media Media;
uint64_t cast_now_ns(void);
int frame_alloc(Frame *, int, int);
void frame_free(Frame *);
int frame_copy(Frame *, const Frame *);
void config_defaults(Config *);
/* Fill software encoder quality from the selected service; explicit settings may override. */
void config_stream_preset(Config *);
int config_load(Config *, const char *, bool, char *, size_t);
void config_print_defaults(void);
int config_validate(const Config *, char *, size_t);
/* Assign one schema value to a candidate; validate the complete batch before applying it. */
int config_set_value(Config *, const char *, const char *, char *, size_t);
/* Registered extension values are ordinary owned config data. Read-only lookup
 * returns the schema default when an older config omits a newly added field. */
const char *config_extension_value(const Config *, const char *section, const char *key);
/* Platform modules keep X11/DBus types private. Platform routines run on daemon thread. */
Platform *platform_open(const Config *, char *, size_t);
void platform_close(Platform *);
Capabilities platform_capabilities(Platform *);
uint64_t platform_source_generation(Platform *);
int platform_capture(Platform *, Frame *, Cursor *, char *, size_t);
int platform_command(Platform *, Config *, int, char **, char *, size_t);
int platform_reconfigure(Platform *, const Config *, char *, size_t);
void platform_events(Platform *, Compositor *, const Config *, bool privacy);
/* Native target selectors return a pending lane index, or -1 when unchanged. */
int platform_preview_target(Platform *);
int platform_preview(Platform *, const Frame *, const State *, const Config *, char *, size_t);
/* Transient local countdown; remaining_ns=0 hides it, return 1 cancels, -1 errors. */
int platform_countdown(Platform *, uint64_t remaining_ns, char *, size_t);
/* Stage the recording composition in the temporary native countdown preview. */
int platform_countdown_frame(Platform *, const Frame *, const Config *, char *, size_t);
void platform_doctor(const Config *, char *, size_t);
/* Only the authenticated panel transport may register its own native window. */
int platform_panel_register(Platform *, uint64_t window, int peer_pid, char *, size_t);
void platform_panel_unregister(Platform *);
void platform_panel_status(Platform *, char *, size_t);
int platform_notes_register(Platform *, uint64_t, int, bool, char *, size_t);
void platform_notes_exclusion(Platform *, bool);
bool platform_notes_registered(Platform *);
bool platform_notes_excluded(Platform *);
/* Optional portal backend selected by the platform factory. */
Platform *wayland_open(const Config *, char *, size_t);
void wayland_close(Platform *);
uint64_t wayland_source_generation(Platform *);
Capabilities wayland_capabilities(Platform *);
int wayland_capture(Platform *, Frame *, Cursor *, char *, size_t);
int wayland_command(Platform *, Config *, int, char **, char *, size_t);
int wayland_reconfigure(Platform *, const Config *, char *, size_t);
void wayland_doctor(const Config *, char *, size_t);
Compositor *compositor_create(void);
void compositor_destroy(Compositor *);
int compositor_render(Compositor *, const Config *, const Frame *, const Frame *, const Cursor *,
                      bool, Frame *, char *, size_t);
/* Validate/load candidate fonts atomically before applying configuration. */
int compositor_prepare(Compositor *, const Config *, char *, size_t);
int compositor_neutral(Compositor *, const Config *, Frame *, char *, size_t);
/* In-place output effect; callers select/copy their unmodified or frozen lane first. */
int compositor_blur(Compositor *, const Config *, Frame *, char *, size_t);
void compositor_clear(Compositor *);
void compositor_click(Compositor *, int, int, int, uint64_t);
/* Keep only bounded transient display labels; adjacent repeats refresh one row. */
void compositor_key(Compositor *, const char *, uint64_t, int timeout_ms);
int compositor_geometry(const Config *, int, int, int *, int *, int *, int *, char *, size_t);
/* Media owns bounded recording/audio workers; barriers synchronously flush pending samples. */
Media *media_open(const Config *, char *, size_t);
void media_close(Media *);
void media_camera_list(char *, size_t);
int media_camera(Media *, Frame *, char *, size_t);
int media_virtual_enabled(Media *, const Config *, bool, char *, size_t);
int media_virtual(Media *, const Frame *, bool, char *, size_t);
int media_record_start(Media *, const Config *, const char *, char *, size_t);
int media_record_stop(Media *, char *, size_t);
bool media_record_finalizing(Media *);
void media_record_error(Media *, char *, size_t);
int media_record_pause(Media *, bool, char *, size_t);
int media_record_silence(Media *, bool, char *, size_t);
int media_record_frame(Media *, const Frame *, char *, size_t);
int media_reconfigure(Media *, const Config *, bool, char *, size_t);
void media_barrier(Media *);
void media_record_barrier(Media *);
int media_privacy(Media *, bool virtual_silent, bool record_silent, bool record_cut, char *,
                  size_t);
int media_audio_command(Media *, Config *, int, char **, char *, size_t);
void media_status(Media *, bool *, bool *, uint64_t *, char *, size_t);
uint64_t media_record_duration(Media *);
void media_record_path(Media *, char *, size_t);
void media_record_encoder(Media *, char *, size_t, char *, size_t);
void media_audio_status(Media *, char *, size_t);
int media_audio_selection(Media *, int, uint64_t, float *, unsigned, char *, size_t);
void media_doctor(const Config *, char *, size_t);
typedef struct StreamSnapshot StreamSnapshot;
int media_stream_start(Media *, const Config *, char *, size_t);
int media_stream_stop(Media *, char *, size_t);
int media_stream_frame(Media *, const Frame *, bool silent, char *, size_t);
void media_stream_privacy(Media *, bool silent);
void media_stream_barrier(Media *);
void media_stream_status(Media *, StreamSnapshot *);
int state_command(State *, const char *, const char *, char *, size_t);
#endif
