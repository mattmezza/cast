#ifndef CAST_PROJECT_H
#define CAST_PROJECT_H
#include "cast.h"
#include "timeline.h"
#define CAST_PROJECT_VERSION 1
#define CAST_PROJECT_LIMIT (1024u * 1024u)
typedef struct {
    unsigned missing_media_count, missing_asset_count;
    uint64_t missing_media_ids[CAST_TIMELINE_MEDIA_MAX];
} CastProjectReport;
/* A project is data only: load never starts capture, changes consent, or reads media.
 * The caller supplies local config defaults/routes; only project-owned settings
 * are overlaid. Both destinations remain unchanged on any error. */
int cast_project_save(const char *, const Config *, const CastTimeline *, char *, size_t);
int cast_project_load(const char *, Config *, CastTimeline *, CastProjectReport *, char *, size_t);
/* Same container/atomic writer; use a distinct caller-chosen autosave path.
 * Recovery is an explicit load, never an implicit overwrite of the main project. */
int cast_project_autosave(const char *, const Config *, const CastTimeline *, char *, size_t);
#endif
