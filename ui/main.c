/* SPDX-License-Identifier: MIT */
#include "cui/cui_components.h"
#include "cui/cui_sdl3.h"
#include "app.h"
#include "cast_theme.h"
#ifdef CAST_CUI_EMBEDDED
#include "workspace.h"
#include "daemon_bridge.h"
#ifdef WITH_X11
#include "../src/edition.h"
#define Cursor X11Cursor
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#undef Cursor
#endif
#endif
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef CAST_CUI_EMBEDDED
static void debug_json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
#endif

/* SDL watchers may run off the UI thread. Only communicate a pending request. */
static bool SDLCALL settings_shortcut(void *user, SDL_Event *event) {
    atomic_bool *pending = user;
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
        event->key.key == SDLK_COMMA &&
        (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))
        atomic_store_explicit(pending, true, memory_order_relaxed);
    return true; /* Watches do not consume SDL events. */
}

#ifndef CAST_CUI_EMBEDDED
/* The bundle places assets beside build/, independent of the working directory. */
static char *font_path(const char *name) {
    const char *base = SDL_GetBasePath();
    const char suffix[] = "../assets/fonts/";
    if (!base || strlen(base) > SIZE_MAX - strlen(name) - sizeof(suffix))
        return NULL;
    size_t size = strlen(base) + sizeof(suffix) + strlen(name);
    char *path = malloc(size);
    if (path)
        snprintf(path, size, "%s%s%s", base, suffix, name);
    return path;
}
#endif
static bool frame_limit(const char *text, unsigned *out) {
    if (!text[0])
        return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || !value || value > UINT_MAX)
        return false;
    *out = (unsigned)value;
    return true;
}
static bool window_size(const char *text, int *width, int *height) {
    const char *separator = strchr(text, 'x');
    if (!separator || separator == text || !separator[1])
        return false;
    for (const char *p = text; *p; ++p)
        if (p != separator && (*p < '0' || *p > '9'))
            return false;
    errno = 0;
    char *end = NULL;
    unsigned long w = strtoul(text, &end, 10);
    if (errno || end != separator || w < 480 || w > 3840)
        return false;
    errno = 0;
    unsigned long h = strtoul(separator + 1, &end, 10);
    if (errno || *end || h < 360 || h > 2160)
        return false;
    *width = (int)w;
    *height = (int)h;
    return true;
}
static void update_appearance(cui_sdl3 *host, AppState *app) {
    cui_platform_appearance appearance = {0};
    app->system_dark = !cui_sdl3_appearance(host, &appearance) ||
                       appearance.appearance != CUI_SYSTEM_APPEARANCE_LIGHT;
}
#ifdef CAST_CUI_EMBEDDED
static bool live_frame_non_uniform(const AppLive *live) {
    if (!live->has_frame || !cui_bitmap_valid(live->frame))
        return false;
    const unsigned char *first = live->frame.pixels;
    for (unsigned y = 0; y < live->frame.height; ++y) {
        const unsigned char *row = first + (size_t)y * live->frame.stride;
        for (unsigned x = 0; x < live->frame.width; ++x) {
            const unsigned char *pixel = row + (size_t)x * 4;
            if (pixel[0] != first[0] || pixel[1] != first[1] || pixel[2] != first[2])
                return true;
        }
    }
    return false;
}
static uint64_t workspace_window(void) {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    uint64_t number = 0;
    for (int i = 0; windows && i < count; ++i) {
        const char *title = SDL_GetWindowTitle(windows[i]);
        if (title && !strcmp(title, "cast — workspace preview")) {
#ifdef WITH_X11
            SDL_PropertiesID props = SDL_GetWindowProperties(windows[i]);
            Display *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
            Window xid = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
            if (display && xid) {
                bool pro = cast_edition_identity()->pro;
                XClassHint hint = {.res_name = pro ? "cast-pro-panel" : "cast-panel",
                                   .res_class = pro ? "CastProPanel" : "CastPanel"};
                XSetClassHint(display, xid, &hint);
                Atom property = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
                Atom normal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
                XChangeProperty(display, xid, property, XA_ATOM, 32, PropModeReplace,
                                (unsigned char *)&normal, 1);
                XSync(display, False);
                number = (uint64_t)xid;
            }
#endif
            break;
        }
    }
    SDL_free(windows);
    return number;
}
#endif
/* Called synchronously at failure sites; never reenter the context here. */
static void diagnostic(void *user, const cui_diagnostic *failure) {
    (void)user;
    fprintf(stderr,
            "CUI %s/%s: %s (error=%d detail=%d id=%llu frame=%llu "
            "used=%zu requested=%zu limit=%zu)\n",
            failure->subsystem, failure->operation, failure->message, (int)failure->error,
            (int)failure->detail, (unsigned long long)failure->id,
            (unsigned long long)failure->frame_sequence, failure->used, failure->requested,
            failure->limit);
}
#ifdef CAST_CUI_EMBEDDED
int cast_workspace_run(const Config *config, int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    const char *regular = NULL, *bold = NULL, *capture = NULL;
    unsigned limit = 0;
    int initial_width = 1360, initial_height = 820;
    bool open_settings = false;
#ifdef CAST_CUI_EMBEDDED
    bool debug_live = false;
    unsigned long long preview_frames_seen = 0;
    bool preview_non_uniform_seen = false;
#endif
    AppState app;
    app_init(&app);
    bool debug_layout = false;
    cui_debug_layout_format debug_format = CUI_DEBUG_LAYOUT_TEXT;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc)
            regular = argv[++i];
        else if (!strcmp(argv[i], "--bold-font") && i + 1 < argc)
            bold = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
            capture = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (!frame_limit(argv[++i], &limit)) {
                fprintf(stderr, "--frames requires an integer from 1 to %u.\n", UINT_MAX);
                return 1;
            }
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (!window_size(argv[++i], &initial_width, &initial_height)) {
                fprintf(stderr, "--size requires WIDTHxHEIGHT within 480..3840 x 360..2160.\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            const char *theme = argv[++i];
            if (!strcmp(theme, "system")) app.appearance = APP_SYSTEM;
            else if (!strcmp(theme, "light")) app.appearance = APP_LIGHT;
            else if (!strcmp(theme, "dark")) app.appearance = APP_DARK;
            else {
                fprintf(stderr, "--theme requires system, light, or dark.\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--workspace") && i + 1 < argc) {
            const char *workspace = argv[++i];
            if (!strcmp(workspace, "camera")) app.workspace = APP_CAMERA;
            else if (!strcmp(workspace, "record")) app.workspace = APP_RECORD;
            else if (!strcmp(workspace, "stream")) app.workspace = APP_STREAM;
            else {
                fprintf(stderr, "--workspace requires camera, record, or stream.\n");
                return 1;
            }
#ifdef CAST_CUI_EMBEDDED
        } else if (!strcmp(argv[i], "--debug-live")) {
            debug_live = true;
#endif
        } else if (!strcmp(argv[i], "--settings")) {
            open_settings = true;
        } else if (!strcmp(argv[i], "--debug-layout") && i + 1 < argc) {
            const char *format = argv[++i];
            if (strcmp(format, "text") && strcmp(format, "json")) {
                fprintf(stderr, "--debug-layout requires text or json.\n");
                return 1;
            }
            debug_layout = true;
            debug_format = !strcmp(format, "json") ? CUI_DEBUG_LAYOUT_JSON : CUI_DEBUG_LAYOUT_TEXT;
        } else {
            fprintf(stderr, "Usage: cast-ui-preview [--font PATH] [--bold-font PATH] "
                            "[--frames N] [--capture BMP] [--debug-layout text|json] "
                            "[--size WIDTHxHEIGHT] [--theme system|light|dark] "
                            "[--workspace camera|record|stream] [--settings]"
#ifdef CAST_CUI_EMBEDDED
                            " [--debug-live]"
#endif
                            "\n");
            return 1;
        }
    }
    if ((regular && !regular[0]) || (bold && !bold[0]) || (capture && (!capture[0] || !limit))) {
        fprintf(stderr, "Font paths must be nonempty; --capture requires a path and --frames.\n");
        return 1;
    }
#ifdef CAST_CUI_EMBEDDED
    char *default_regular = NULL, *default_bold = NULL;
    cui_font *font = regular ? cui_font_open(regular)
        : cui_font_open_memory(cast_ui_regular_start,
                              (size_t)(cast_ui_regular_end - cast_ui_regular_start));
    cui_font *bold_face = NULL, *medium_face = NULL, *semibold_face = NULL;
    cui_font *mono_faces[3] = {0};
#else
    char *default_regular = regular ? NULL : font_path("Inter-Regular.otf");
    char *default_bold = bold ? NULL : font_path("Inter-Bold.otf");
    char *medium_path = font_path("Inter-Medium.ttf");
    char *semibold_path = font_path("Inter-SemiBold.ttf");
    regular = regular ? regular : default_regular;
    bold = bold ? bold : default_bold;
    cui_font *font = regular ? cui_font_open(regular) : NULL;
#endif
#ifndef CAST_CUI_EMBEDDED
    char *mono_paths[3] = {font_path("IBMPlexMono-Regular.ttf"),
        font_path("IBMPlexMono-Medium.ttf"), font_path("IBMPlexMono-SemiBold.ttf")};
#endif
    int result = 2;
    cui_sdl3 *host = NULL;
    cui_context *ui = NULL;
    atomic_bool settings_pending = ATOMIC_VAR_INIT(false);
    bool settings_watch_registered = false;
#ifdef CAST_CUI_EMBEDDED
    DaemonBridge *bridge = NULL;
    Uint64 next_attach = 0, next_poll = 0;
    int previous_target = -1;
    uint64_t window_number = 0;
    char last_connection_error[sizeof app.live.error] = {0};
    char attach_error[sizeof app.live.error] = {0};
    app.live.enabled = true;
#endif
    if (!font) {
        fprintf(stderr, "Cannot load regular font: %s\n",
                regular ? regular : "asset path unavailable");
        goto cleanup;
    }
#ifdef CAST_CUI_EMBEDDED
    bold_face = bold ? cui_font_open(bold)
        : cui_font_open_memory(cast_ui_bold_start,
                              (size_t)(cast_ui_bold_end - cast_ui_bold_start));
    if (!bold_face ||
        !cui_font_set_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_BOLD}, bold_face)) {
#else
    if (!bold ||
        !cui_font_register_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_BOLD}, bold)) {
#endif
        fprintf(stderr, "Cannot load bold font: %s\n", bold ? bold : "asset path unavailable");
        goto cleanup;
    }
#ifdef CAST_CUI_EMBEDDED
    medium_face = cui_font_open_memory(cast_ui_medium_start,
        (size_t)(cast_ui_medium_end - cast_ui_medium_start));
    semibold_face = cui_font_open_memory(cast_ui_semibold_start,
        (size_t)(cast_ui_semibold_end - cast_ui_semibold_start));
    if (!medium_face || !semibold_face ||
        !cui_font_set_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM}, medium_face) ||
        !cui_font_set_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_SEMIBOLD}, semibold_face)) {
#else
    if (!medium_path || !semibold_path ||
        !cui_font_register_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM}, medium_path) ||
        !cui_font_register_style(font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_SEMIBOLD}, semibold_path)) {
#endif
        fprintf(stderr, "Cannot load bundled medium/semibold fonts.\n");
        goto cleanup;
    }
    const cui_text_weight mono_weights[] = {CUI_TEXT_WEIGHT_NORMAL,
        CUI_TEXT_WEIGHT_MEDIUM, CUI_TEXT_WEIGHT_SEMIBOLD};
#ifdef CAST_CUI_EMBEDDED
    const unsigned char *mono_start[] = {cast_ui_mono_regular_start,
        cast_ui_mono_medium_start, cast_ui_mono_semibold_start};
    const unsigned char *mono_end[] = {cast_ui_mono_regular_end,
        cast_ui_mono_medium_end, cast_ui_mono_semibold_end};
#endif
    for (unsigned i = 0; i < 3; ++i) {
        /* Family 1 is IBM Plex Mono; keep the monospace flag part of its key. */
        cui_text_style style = {.family = 1, .weight = mono_weights[i], .monospace = true};
#ifdef CAST_CUI_EMBEDDED
        mono_faces[i] = cui_font_open_memory(mono_start[i], (size_t)(mono_end[i] - mono_start[i]));
        bool loaded = mono_faces[i] && cui_font_set_style(font, style, mono_faces[i]);
#else
        bool loaded = mono_paths[i] && cui_font_register_style(font, style, mono_paths[i]);
#endif
        if (!loaded) {
            fprintf(stderr, "Cannot load bundled IBM Plex Mono weight %u.\n", i);
            goto cleanup;
        }
    }
#ifdef CAST_CUI_EMBEDDED
#ifdef WITH_X11
    if (config && (!strcmp(config->backend, "xorg") || !strcmp(config->backend, "x11")) &&
        !SDL_getenv("SDL_VIDEODRIVER") && !SDL_getenv("SDL_VIDEO_DRIVER"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
    SDL_SetHint(SDL_HINT_APP_ID, "org.cast.Panel");
    SDL_SetHint(SDL_HINT_X11_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE_NORMAL");
#endif
    host = cui_sdl3_create("cast — workspace preview", initial_width, initial_height, font);
    if (!host) {
        fprintf(stderr, "Cannot start SDL3 host: %s\n", cui_sdl3_error());
        goto cleanup;
    }
    ui = cui_create((cui_config){.width = initial_width,
                                 .height = initial_height,
                                 .measure = cui_font_measure,
                                 .measure_styled = cui_font_measure_styled,
                                 .text_geometry = cui_font_text_geometry,
                                 .text_geometry_line = cui_font_text_geometry_line,
                                 .font_metrics = cui_font_get_metrics,
                                 .measure_user = font});
    if (!ui) {
        fprintf(stderr, "Cannot create CUI context.\n");
        goto cleanup;
    }
    if (!SDL_AddEventWatch(settings_shortcut, &settings_pending)) {
        fprintf(stderr, "Cannot register Settings shortcut: %s\n", SDL_GetError());
        goto cleanup;
    }
    settings_watch_registered = true;
    cui_set_diagnostic_callback(ui, diagnostic, NULL);
    cui_set_layout_debug(ui, debug_layout);
    update_appearance(host, &app);
#ifdef CAST_CUI_EMBEDDED
    window_number = workspace_window();
#endif
    unsigned rendered = 0;
    result = 0;
    while (!app.quit_requested && (!limit || rendered < limit)) {
        if (!cui_sdl3_poll(host, ui)) {
            if (limit) {
                fprintf(stderr, "Window closed after %u of %u requested frames.\n", rendered,
                        limit);
                result = 3;
            }
            break;
        }
        if (atomic_exchange_explicit(&settings_pending, false, memory_order_relaxed)) {
            open_settings = true;
            cui_invalidate(ui);
        }
        update_appearance(host, &app);
#ifdef CAST_CUI_EMBEDDED
        Uint64 now = SDL_GetTicks();
        if (!bridge && now >= next_attach) {
            attach_error[0] = '\0';
            bridge = bridge_open(config, window_number, attach_error, sizeof attach_error);
            next_attach = now + 1000;
        }
        if (now >= next_poll || previous_target != (int)app.workspace) {
            bridge_poll(bridge, (int)app.workspace, &app.live);
            if (app.live.has_frame) {
                ++preview_frames_seen;
                preview_non_uniform_seen |= live_frame_non_uniform(&app.live);
            }
            app.live.enabled = true;
            if (!bridge && attach_error[0])
                snprintf(app.live.error, sizeof app.live.error, "%s", attach_error);
            if (app.live.error[0] && strcmp(app.live.error, last_connection_error)) {
                fprintf(stderr, "Workspace daemon: %s\n", app.live.error);
                snprintf(last_connection_error, sizeof last_connection_error, "%s", app.live.error);
            }
            previous_target = (int)app.workspace;
            next_poll = now + 30;
            cui_invalidate(ui);
        }
#endif
        if (rendered && !limit && !cui_needs_redraw(ui)) {
#ifdef CAST_CUI_EMBEDDED
            (void)cui_sdl3_wait_events(host, 30);
#else
            (void)cui_sdl3_wait_events(host, 250);
#endif
            continue;
        }
        float width, height;
        cui_sdl3_size(host, &width, &height);
        if (!cui_begin_frame(ui, width, height, cui_sdl3_frame_delta(host))) {
            result = 3;
            break;
        }
        if (open_settings) {
            if (!app.settings.overlay.open && !app.panel_sheet.overlay.open)
                app_open_settings(ui, &app, cui_focused(ui));
            open_settings = false;
        }
        app_frame(ui, &app, width, height);
#ifdef CAST_CUI_EMBEDDED
        if (app.device_request_requested) {
            char device_error[CAST_ERR] = {0};
            int queued = bridge_devices(bridge, app.device_request_kind,
                                        device_error, sizeof device_error);
            if (queued < 0) {
                app.live.devices.kind = app.device_request_kind;
                app.live.devices.loading = false;
                snprintf(app.live.devices.error, sizeof app.live.devices.error, "%.*s",
                         (int)sizeof app.live.devices.error - 1, device_error);
            }
            app.device_request_requested = false;
            app.device_request_kind = APP_DEVICE_NONE;
            next_poll = 0;
            cui_invalidate(ui);
        }
        if (app.source_choice_requested) {
            char source_error[CAST_ERR] = {0};
            int queued = bridge_source(bridge, app.source_choice_kind,
                                       app.source_choice_value, source_error, sizeof source_error);
            app.live.pending = queued == 0;
            if (queued < 0)
                snprintf(app.live.edit_error, sizeof app.live.edit_error, "%.*s",
                         (int)sizeof app.live.edit_error - 1, source_error);
            app.source_choice_requested = false;
            app.source_choice_kind = APP_DEVICE_NONE;
            app.source_choice_value[0] = 0;
            next_poll = 0;
            cui_invalidate(ui);
        }
        if (app.edit_requested) {
            char edit_error[CAST_ERR] = {0};
            int queued = bridge_edit(bridge, app.edit, app.edit_value, edit_error, sizeof edit_error);
            app.live.pending = queued == 0;
            if (queued < 0)
                snprintf(app.live.edit_error, sizeof app.live.edit_error, "%.*s",
                         (int)sizeof app.live.edit_error - 1, edit_error);
            app.edit_requested = false;
            app.edit = APP_EDIT_NONE;
            app.edit_value[0] = 0;
            next_poll = 0;
            cui_invalidate(ui);
        }
#endif
        cui_theme theme = cast_theme(app_is_dark(&app));
        cui_draw_list list = cui_end_frame(ui);
        if (cui_last_error(ui) != CUI_OK) {
            fprintf(stderr, "CUI layout failed (error %d).\n", (int)cui_last_error(ui));
            result = 3;
            break;
        }
        if (debug_layout) {
            size_t required = 0;
            (void)cui_debug_layout_export(ui, debug_format, NULL, 0, &required);
            char *dump = required ? malloc(required) : NULL;
            if (!dump || !cui_debug_layout_export(ui, debug_format, dump, required, &required)) {
                free(dump);
                fprintf(stderr, "Cannot export completed layout.\n");
                result = 3;
                break;
            }
            puts(dump);
            free(dump);
            debug_layout = false; /* Print the first completed frame once. */
        }
        (void)cui_sdl3_set_cursor(host, cui_pointer_cursor(ui));
        bool painted = capture && rendered + 1 == limit
                           ? cui_sdl3_render_capture(host, &list, theme.background, capture)
                           : cui_sdl3_render(host, &list, theme.background);
        if (!painted) {
            fprintf(stderr, "Cannot render%s: %s\n", capture ? " or capture frame" : " frame",
                    cui_sdl3_error());
            result = 3;
            break;
        }
        ++rendered; /* Only successful renders count toward the requested bound. */
    }
cleanup:
#ifdef CAST_CUI_EMBEDDED
    if (debug_live) {
        printf("{\"connected\":%s,\"has_frame\":%s,\"generation\":%llu,"
               "\"privacy_epoch\":%llu,\"frame_non_uniform\":%s,"
               "\"virtual_paused\":%s,\"recording\":%s,\"stream_active\":%s,"
               "\"layout\":\"%s\",\"fit\":\"%s\",\"anchor\":\"%s\","
               "\"camera_width_percent\":%.2f,\"ui_layout\":\"%s\","
               "\"ui_fit_cover\":%s,\"ui_camera_anchor\":%u,\"ui_camera_size\":%.2f,",
               app.live.connected ? "true" : "false", app.live.has_frame ? "true" : "false",
               (unsigned long long)app.live.generation,
               (unsigned long long)app.live.privacy_epoch,
               live_frame_non_uniform(&app.live) ? "true" : "false",
               app.live.virtual_paused ? "true" : "false",
               app.live.recording ? "true" : "false",
               app.live.stream_active ? "true" : "false",
               app.live.layout, app.live.fit, app.live.anchor, app.live.camera_width_percent,
               (const char *const[]){"overlay", "split", "screen", "camera", "stage"}[app.layout],
               app.fit_cover ? "true" : "false", app.camera_anchor, app.camera_size);
        printf("\"preview_frames_seen\":%llu,\"preview_non_uniform_seen\":%s,",
               preview_frames_seen, preview_non_uniform_seen ? "true" : "false");
        printf("\"screen_id\":"); debug_json_string(app.live.screen_id);
        printf(",\"camera_id\":"); debug_json_string(app.live.camera_id);
        printf(",\"camera_enabled\":%s,\"devices_kind\":%u,\"devices_count\":%u,"
               "\"devices_loading\":%s,\"picker_open\":%s,\"picker_kind\":%u,\"devices_error\":",
               app.live.camera_enabled ? "true" : "false", (unsigned)app.live.devices.kind,
               app.live.devices.count, app.live.devices.loading ? "true" : "false",
               app.source_picker.popup.overlay.open ? "true" : "false", (unsigned)app.source_picker.kind);
        debug_json_string(app.live.devices.error);
        puts("}");
    }
#endif
    if (settings_watch_registered)
        SDL_RemoveEventWatch(settings_shortcut, &settings_pending);
    cui_destroy(ui);
    cui_sdl3_destroy(host);
#ifdef CAST_CUI_EMBEDDED
    bridge_close(bridge);
#endif
    cui_font_close(font);
#ifdef CAST_CUI_EMBEDDED
    cui_font_close(bold_face);
    cui_font_close(medium_face);
    cui_font_close(semibold_face);
    for (unsigned i = 0; i < 3; ++i) cui_font_close(mono_faces[i]);
#else
    free(medium_path);
    free(semibold_path);
    for (unsigned i = 0; i < 3; ++i) free(mono_paths[i]);
#endif
    free(default_regular);
    free(default_bold);
    return result;
}
