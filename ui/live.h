#ifndef CAST_UI_LIVE_H
#define CAST_UI_LIVE_H
#include "cui/cui.h"
typedef enum { APP_EDIT_NONE, APP_EDIT_LAYOUT, APP_EDIT_FIT, APP_EDIT_ANCHOR, APP_EDIT_SIZE,
    APP_EDIT_BACKGROUND_SOURCE, APP_EDIT_SCREEN_BACKGROUND, APP_EDIT_CAMERA_BACKGROUND,
    APP_EDIT_SCREEN_BACKGROUND_COLOR, APP_EDIT_CAMERA_BACKGROUND_COLOR,
    APP_EDIT_GRADIENT_FROM, APP_EDIT_GRADIENT_VIA, APP_EDIT_GRADIENT_TO,
    APP_EDIT_GRADIENT_VIA_ENABLED, APP_EDIT_GRADIENT_ANGLE, APP_EDIT_GRADIENT_WAYPOINT,
    APP_EDIT_ZOOM_FACTOR, APP_EDIT_ZOOM_FOLLOW, APP_EDIT_ZOOM_MIN, APP_EDIT_ZOOM_MAX } AppEdit;
#define APP_DEVICE_ID_MAX 4096
#define APP_DEVICE_MAX 64
typedef enum { APP_DEVICE_NONE, APP_DEVICE_SCREEN, APP_DEVICE_CAMERA } AppDeviceKind;
typedef struct { char id[APP_DEVICE_ID_MAX], label[256]; } AppDeviceChoice;
typedef struct {
    AppDeviceKind kind;
    bool loading;
    char error[256];
    unsigned count;
    AppDeviceChoice choices[APP_DEVICE_MAX];
    uint64_t generation;
} AppDevices;
/* Sanitized daemon state. Pixels are borrowed until the next poll. */
typedef struct {
    bool enabled, connected, has_frame, pro, pending, snapshot_busy;
    cui_bitmap frame;
    int target, width, height, fps;
    bool virtual_enabled, virtual_paused, virtual_frozen, virtual_blurred;
    bool recording, record_paused, record_frozen, record_blurred;
    bool stream_active, stream_paused, stream_frozen, stream_blurred;
    bool mic, desktop, camera_enabled, camera_visible, mirror;
    bool can_select_monitor, can_reselect_portal, can_select_camera;
    char screen_id[128], camera_id[APP_DEVICE_ID_MAX];
    float camera_width_percent;
    char layout[16], fit[16], anchor[24];
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
