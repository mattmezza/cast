#ifndef CAST_DESKTOP_UPDATE_UI_H
#define CAST_DESKTOP_UPDATE_UI_H
#include "cast.h"
void cast_desktop_update_arguments(int, char **);
int cast_desktop_update_runtime_lock(char *, size_t);
typedef struct CastDesktopUpdater CastDesktopUpdater;
typedef enum { CAST_DESKTOP_IDLE, CAST_DESKTOP_CHECKING, CAST_DESKTOP_AVAILABLE,
               CAST_DESKTOP_CURRENT, CAST_DESKTOP_DOWNLOADING, CAST_DESKTOP_RESTARTING,
               CAST_DESKTOP_FAILED, CAST_DESKTOP_INELIGIBLE } CastDesktopUpdateState;
typedef struct {
    CastDesktopUpdateState state;
    char version[32], message[CAST_ERR];
} CastDesktopUpdateProgress;
CastDesktopUpdater *cast_desktop_updater_open(const Config *);
void cast_desktop_updater_close(CastDesktopUpdater *);
void cast_desktop_updater_poll(CastDesktopUpdater *, CastDesktopUpdateProgress *);
int cast_desktop_updater_check(CastDesktopUpdater *);
int cast_desktop_updater_install(CastDesktopUpdater *);
int cast_desktop_update_cli(int, char **, const Config *, char *, size_t);
int cast_desktop_update_helper(int, char **);
#endif
