/* Optional edition-owned settings renderer. */
#ifndef CAST_WORKSPACE_EXTENSION_H
#define CAST_WORKSPACE_EXTENSION_H
#include "app.h"
#if defined(WITH_PRO) && defined(WITH_CUI)
void cast_workspace_snapshot(AppLive *, const void *snapshot);
void cast_workspace_settings_sync(cui_context *, AppState *);
bool cast_workspace_settings(cui_context *, AppState *, const cui_theme *, unsigned category, bool narrow);
#endif
#endif
