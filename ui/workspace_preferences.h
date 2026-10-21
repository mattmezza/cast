#ifndef CAST_WORKSPACE_PREFERENCES_H
#define CAST_WORKSPACE_PREFERENCES_H
#include <stdbool.h>
#include <stddef.h>
/* Local interface choices only. Deliberately excludes Config, project paths,
 * media, devices, output activation, stream credentials and licensing. */
typedef enum { WORKSPACE_THEME_SYSTEM, WORKSPACE_THEME_LIGHT, WORKSPACE_THEME_DARK } WorkspaceTheme;
typedef struct {
    WorkspaceTheme theme;
    bool show_sources, show_inspector;
    bool timeline_camera, timeline_record, timeline_stream;
    int window_width, window_height;
} WorkspacePreferences;
void workspace_preferences_defaults(WorkspacePreferences *);
bool workspace_preferences_valid(const WorkspacePreferences *);
/* XDG_CONFIG_HOME/cast/workspace.ini, HOME fallback; macOS Application Support.
 * Only resolves a path; never creates or reads anything. */
bool workspace_preferences_path(char *, size_t);
/* 0 loaded, 1 absent, -1 invalid/unreadable. Destination unchanged unless loaded.
 * A future/unknown file is rejected rather than partially interpreted. */
int workspace_preferences_load(const char *, WorkspacePreferences *, char *, size_t);
/* Explicit save only. Creates missing parent dirs with0700; private0600 atomic
 * replacement, fsync(file+parent). No fallback path, retries or config writes. */
int workspace_preferences_save(const char *, const WorkspacePreferences *, char *, size_t);
#endif
