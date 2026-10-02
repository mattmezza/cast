#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H
#include "cast.h"
#include "panel_transport.h"

/* Daemon session storage stays stable while asynchronous selection references it. */
typedef struct {
    const char *flag;
    size_t offset, size;
    bool numeric;
    int min, max;
} Option;
typedef struct {
    char config_path[PATH_MAX];
    bool explicit_config;
    int override_count;
    const Option *override[32];
    const char *values[32];
    bool no_live, no_camera;
} Startup;
typedef struct App {
    Config config, defaults;
    Startup startup;
    State state;
    Platform *platform;
    Compositor *compositor;
    Media *media;
    Frame screen, camera, live, record, neutral, frozen;
    Cursor cursor;
    double zoom_last;
    uint64_t source_generation, loop_drops;
    char current_preset[64];
    bool countdown;
    uint64_t countdown_deadline;
    char countdown_path[PATH_MAX];
    bool record_finalizing;
    PanelTransport *panel;
} App;
int app_error(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int app_copy_string(char *, size_t, const char *, char *, size_t);
int app_integer(const char *, int, int, int *, char *, size_t);
int app_apply_overrides(Config *, const Startup *, char *, size_t);
int app_socket_path(Config *, char *, size_t);
void app_request_stop(void);
int app_command(App *, int, char **, char *, size_t);
int app_recording_start(App *, const char *, char *, size_t);
void app_sync_source(App *);
int app_shutdown_privacy(App *, char *, size_t);
#endif
