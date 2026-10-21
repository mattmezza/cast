#ifndef CAST_TIMELINE_EXPORT_STATUS_H
#define CAST_TIMELINE_EXPORT_STATUS_H
#include <stdint.h>
typedef enum {
    CAST_TIMELINE_EXPORT_QUEUED,
    CAST_TIMELINE_EXPORT_RUNNING,
    CAST_TIMELINE_EXPORT_COMPLETE,
    CAST_TIMELINE_EXPORT_CANCELLED,
    CAST_TIMELINE_EXPORT_FAILED
} CastTimelineExportState;
typedef struct {
    CastTimelineExportState state;
    uint64_t completed_ns, duration_ns;
    char path[4096], error[1024];
} CastTimelineExportSnapshot;
#endif
