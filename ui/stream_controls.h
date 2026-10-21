/* SPDX-License-Identifier: MIT */
#ifndef CAST_UI_STREAM_CONTROLS_H
#define CAST_UI_STREAM_CONTROLS_H
#include "cui/cui_components.h"
typedef enum {
    APP_STREAM_STOPPED, APP_STREAM_CONNECTING, APP_STREAM_STREAMING,
    APP_STREAM_RECONNECTING, APP_STREAM_STOPPING, APP_STREAM_FAILED
} AppStreamPhase;
/* Acknowledged metadata only. Never put key contents or unredacted transport
 * diagnostics here. URL must be a server/application endpoint without secrets. */
typedef struct {
    bool connected, active, paused, frozen, blurred;
    uint64_t generation;
    AppStreamPhase phase;
    char service[16], server_url[1024], key_file[4096];
    char video_encoder[64], encoder_preset[24], error[256];
    int video_bitrate_kbps, audio_bitrate_kbps;
    int reconnect_attempts, reconnect_initial_ms, reconnect_max_ms;
    double outgoing_bitrate_kbps;
    unsigned retry_attempt, queue_depth;
    uint64_t dropped_frames, bytes_written;
} AppStreamSnapshot;
typedef enum {
    APP_STREAM_ACTION, APP_STREAM_SERVICE, APP_STREAM_SERVER_URL, APP_STREAM_KEY_FILE,
    APP_STREAM_VIDEO_BITRATE, APP_STREAM_AUDIO_BITRATE, APP_STREAM_ENCODER_PRESET,
    APP_STREAM_RECONNECT_ATTEMPTS, APP_STREAM_RECONNECT_INITIAL, APP_STREAM_RECONNECT_MAX
} AppStreamIntent;
/* Synchronous, nonretaining callback: copy a single intent into the host queue.
 * True means queued, never that the daemon has applied it. No retries/replay. */
typedef bool (*AppStreamSubmit)(void *, AppStreamIntent, const char *);
bool app_stream_action_allowed(const AppStreamSnapshot *, bool blocked, const char *);
bool app_stream_value_valid(AppStreamIntent, const char *);
/* Each function composes one card into its caller's current vertical container.
 * blocked includes pending/local intent and snapshot contention. CUI resources
 * own edit/confirmation state; separate contexts remain independent. */
void app_stream_destination(cui_context *, const cui_theme *, const AppStreamSnapshot *, bool,
                            AppStreamSubmit, void *);
void app_stream_output(cui_context *, const cui_theme *, const AppStreamSnapshot *, bool,
                       AppStreamSubmit, void *);
void app_stream_encoding(cui_context *, const cui_theme *, const AppStreamSnapshot *, bool,
                         AppStreamSubmit, void *);
#endif
