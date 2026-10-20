#ifndef CAST_UI_LIVE_H
#define CAST_UI_LIVE_H
#include "cui/cui.h"
typedef enum { APP_EDIT_NONE, APP_EDIT_LAYOUT, APP_EDIT_FIT, APP_EDIT_ANCHOR, APP_EDIT_SIZE } AppEdit;
/* Sanitized daemon state. Pixels are borrowed until the next poll. */
typedef struct {
    bool enabled, connected, has_frame, pro, pending;
    cui_bitmap frame;
    int target, width, height, fps;
    bool virtual_enabled, virtual_paused, virtual_frozen, virtual_blurred;
    bool recording, record_paused, record_frozen, record_blurred;
    bool stream_active, stream_paused, stream_frozen, stream_blurred;
    bool mic, desktop, camera_enabled, camera_visible, mirror;
    float camera_width_percent;
    char layout[16], fit[16], anchor[24];
    char backend[16], record_container[24], record_codec[64], stream_service[16];
    char screen_name[128], camera_name[128], mic_name[128], desktop_name[128];
    char status[128], error[256], edit_error[256];
    uint64_t generation, privacy_epoch;
} AppLive;
#endif
