/* Pure input geometry/queue shared by the native adapter and device-free tests. */
#ifndef CAST_MACOS_POINTER_H
#define CAST_MACOS_POINTER_H
#include "cast.h"
#include <math.h>
typedef struct { double x, y; int button; uint64_t at; } MacPointerClick;
typedef struct { MacPointerClick clicks[64]; unsigned count; } MacPointerQueue;
static inline Cursor mac_pointer_map(double x, double y, double left, double top,
                                     double width, double height, int pixels_x, int pixels_y,
                                     uint64_t at)
{
    Cursor cursor = {.ts_ns = at};
    if (isfinite(x) && isfinite(y) && width > 0 && height > 0 && pixels_x > 0 && pixels_y > 0 &&
        x >= left && y >= top && x < left + width && y < top + height) {
        cursor.x = (int)((x - left) * pixels_x / width);
        cursor.y = (int)((y - top) * pixels_y / height);
        cursor.valid = true;
    }
    return cursor;
}
static inline void mac_pointer_push(MacPointerQueue *queue, MacPointerClick click)
{
    if (queue->count < 64) queue->clicks[queue->count++] = click;
}
static inline unsigned mac_pointer_drain(MacPointerQueue *queue, bool privacy,
                                         MacPointerClick output[64])
{
    unsigned count = privacy ? 0 : queue->count;
    for (unsigned i = 0; i < count; ++i) output[i] = queue->clicks[i];
    /* Erase physical observations even when privacy suppressed delivery. */
    for (unsigned i = 0; i < queue->count; ++i) queue->clicks[i] = (MacPointerClick){0};
    queue->count = 0;
    return count;
}
#endif
