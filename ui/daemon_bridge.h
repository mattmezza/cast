#ifndef CAST_UI_DAEMON_BRIDGE_H
#define CAST_UI_DAEMON_BRIDGE_H
#include "../src/cast.h"
#include "live.h"
typedef struct DaemonBridge DaemonBridge;
/* Attaches to an existing daemon only. window is the native X11 exclusion ID. */
DaemonBridge *bridge_open(const Config *, uint64_t window, char *error, size_t);
/* Nonblocking. target: 0 virtual, 1 record, 2 stream. */
void bridge_poll(DaemonBridge *, int target, AppLive *out);
/* Queue one allowlisted composition edit. Returns 0 queued, -1 refused.
 * No replay after disconnection or generation changes. */
int bridge_edit(DaemonBridge *, AppEdit, const char *value, char *error, size_t);
int bridge_devices(DaemonBridge *, AppDeviceKind, char *error, size_t);
/* Empty camera ID disables it; empty portal screen ID opens desktop consent. */
int bridge_source(DaemonBridge *, AppDeviceKind, const char *id, char *error, size_t);
void bridge_close(DaemonBridge *);
#endif
