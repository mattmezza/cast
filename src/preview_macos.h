#ifndef CAST_PREVIEW_MACOS_H
#define CAST_PREVIEW_MACOS_H
#include "cast.h"
typedef struct CastMacPreview CastMacPreview;
CastMacPreview *mac_preview_open(void);
void mac_preview_close(CastMacPreview *);
void mac_preview_events(CastMacPreview *);
/* One-shot target: 0 virtual, 1 record, 2 stream, 3 composition, -2 dismissed. */
int mac_preview_target(CastMacPreview *);
int mac_preview_frame(CastMacPreview *, const Frame *, const State *, const Config *, char *, size_t);
int mac_preview_countdown(CastMacPreview *, uint64_t, char *, size_t);
int mac_preview_countdown_frame(CastMacPreview *, const Frame *, const Config *, char *, size_t);
#endif
