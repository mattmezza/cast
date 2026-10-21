#ifndef CAST_UI_LIVE_H
#define CAST_UI_LIVE_H
#include "cui/cui.h"
#include "../src/project_session.h"
#include "stream_controls.h"
#include "../src/encoder_capabilities.h"
#include "../src/audio_monitor_controller.h"
typedef enum { APP_EDIT_NONE, APP_EDIT_LAYOUT, APP_EDIT_FIT, APP_EDIT_ANCHOR, APP_EDIT_SIZE,
    APP_EDIT_BACKGROUND_SOURCE, APP_EDIT_SCREEN_BACKGROUND, APP_EDIT_CAMERA_BACKGROUND,
    APP_EDIT_SCREEN_BACKGROUND_COLOR, APP_EDIT_CAMERA_BACKGROUND_COLOR,
    APP_EDIT_GRADIENT_FROM, APP_EDIT_GRADIENT_VIA, APP_EDIT_GRADIENT_TO,
    APP_EDIT_GRADIENT_VIA_ENABLED, APP_EDIT_GRADIENT_ANGLE, APP_EDIT_GRADIENT_WAYPOINT,
    APP_EDIT_ZOOM_FACTOR, APP_EDIT_ZOOM_FOLLOW, APP_EDIT_ZOOM_MIN, APP_EDIT_ZOOM_MAX,
    APP_EDIT_RECORD_ACTION, APP_EDIT_RECORD_CONTAINER, APP_EDIT_RECORD_VIDEO_CODEC,
    APP_EDIT_RECORD_AUDIO_CODEC, APP_EDIT_RECORD_CRF, APP_EDIT_RECORD_COUNTDOWN, APP_EDIT_RECORD_DIRECTORY,
    APP_EDIT_AUDIO_MIC, APP_EDIT_AUDIO_DESKTOP, APP_EDIT_AUDIO_VIRTUAL,
    APP_EDIT_AUDIO_MIC_GAIN, APP_EDIT_AUDIO_DESKTOP_GAIN, APP_EDIT_NOTES_ACTION } AppEdit;
#define APP_DEVICE_ID_MAX 4096
#define APP_DEVICE_MAX 64
typedef enum { APP_DEVICE_NONE, APP_DEVICE_SCREEN, APP_DEVICE_CAMERA, APP_DEVICE_MIC, APP_DEVICE_DESKTOP } AppDeviceKind;
typedef struct { char id[APP_DEVICE_ID_MAX], label[256]; } AppDeviceChoice;
typedef struct {
    AppDeviceKind kind;
    bool loading;
    char error[256];
    unsigned count;
    AppDeviceChoice choices[APP_DEVICE_MAX];
    uint64_t generation;
} AppDevices;
#define APP_SETTING_MAX 128
typedef struct { float peak, rms; bool fresh, clipped, available; } AppAudioLevel;
typedef struct { char section[50], key[64], value[1024]; } AppSetting;
/* Sanitized daemon state. Pixels are borrowed until the next poll. */
typedef struct {
    bool enabled, connected, has_frame, pro, pending, snapshot_busy;
    CastProjectSnapshot project;
    AppStreamSnapshot stream;
    CastEncoderCapabilities encoders;
    CastMonitorControllerStatus monitor;
    bool audio_levels_supported;
    AppAudioLevel mic_level, desktop_level;
    bool notes_available, notes_window_open, license_valid;
    char license_id[32];
    unsigned preset_count;
    char preset_names[24][64], current_preset[64];
    unsigned setting_count;
    AppSetting settings[APP_SETTING_MAX];
    cui_bitmap frame;
    int target, width, height, fps;
    bool group_paused;
    bool virtual_enabled, virtual_paused, virtual_frozen, virtual_blurred;
    bool recording, record_paused, record_frozen, record_blurred;
    bool stream_active, stream_paused, stream_frozen, stream_blurred;
    bool mic, desktop, camera_enabled, camera_visible, mirror;
    bool record_cut, record_countdown_active, record_finalizing, virtual_audio, can_virtual_audio;
    bool can_select_audio, audio_available, mic_ready, desktop_ready, virtual_ready, virtual_silent;
    uint64_t record_duration_ns, record_countdown_remaining_ns;
    char record_path[4096], record_directory[4096], record_audio_codec[64], record_encoder[64];
    int record_crf, record_countdown;
    float mic_gain, desktop_gain;
    char mic_source[256], desktop_source[256], mic_id[256], desktop_id[256], audio_status[4096];
    bool can_select_monitor, can_reselect_portal, can_select_camera;
    char screen_id[128], camera_id[APP_DEVICE_ID_MAX];
    float camera_width_percent;
    char layout[16], fit[16], anchor[24], camera_shape[16];
    int screen_radius, screen_border_width;
    bool cursor, clicks, keys;
    char background_source[16], screen_background[16], camera_background[16];
    uint32_t screen_background_color, camera_background_color;
    uint32_t gradient_from, gradient_via, gradient_to;
    bool gradient_via_enabled;
    float gradient_angle, gradient_waypoint;
    float zoom_factor, zoom_min, zoom_max;
    bool zoom_follow, can_zoom_follow, cinematic_zoom_available, cinematic_zoom_active;
    char backend[16], record_container[24], record_codec[64], stream_service[16];
    char screen_name[128], camera_name[128], mic_name[128], desktop_name[128];
    char status[128], error[256], edit_error[256];
    uint64_t generation, privacy_epoch;
    /* Last, so the bridge can retain compact acknowledged metadata separately. */
    AppDevices devices;
} AppLive;
#endif
