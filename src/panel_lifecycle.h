#ifndef CAST_PANEL_LIFECYCLE_H
#define CAST_PANEL_LIFECYCLE_H
#include "cast.h"
#include <sys/types.h>

typedef enum {
    PANEL_DAEMON_STOPPED,
    PANEL_DAEMON_STARTING,
    PANEL_DAEMON_RUNNING,
    PANEL_DAEMON_FAILED
} PanelDaemonState;
typedef struct {
    PanelDaemonState state;
    pid_t child_pid;
    bool owned;
    int exit_status;
    char error[CAST_ERR];
} PanelLifecycleSnapshot;
typedef struct PanelLifecycle PanelLifecycle;
/* Arguments contain startup flags and values, without the executable or command.
 * The module copies them and preserves them for later config reloads. */
PanelLifecycle *panel_lifecycle_create(const Config *, int startup_argc,
                                       const char *const *startup_argv, char *, size_t);
/* Start from the latest acknowledged configuration; outputs start privacy-paused.
 * Spawn does not wait for backend startup. Existing user-owned producers are retained. */
int panel_lifecycle_start(PanelLifecycle *, const Config *, char *, size_t);
/* Explicit Stop/Quit fallback before IPC attaches. Only the tracked child is signaled. */
int panel_lifecycle_stop_owned(PanelLifecycle *, char *, size_t);
/* Never waits: drains bounded diagnostic bytes and reaps with WNOHANG.
 * connected must come from the authenticated panel transport. */
void panel_lifecycle_poll(PanelLifecycle *, bool connected, PanelLifecycleSnapshot *);
/* Releases bookkeeping. Stop/Quit use the daemon's normal quit IPC first. */
void panel_lifecycle_destroy(PanelLifecycle *);
/* Private sealed same-build handoff, called before opening devices/starting threads.
 * fd is consumed on every path. Closing the panel leaves the daemon running. */
int panel_lifecycle_config_fd(int fd, Config *, char *, size_t);
#endif
