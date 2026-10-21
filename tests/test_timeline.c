#include "timeline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SECOND UINT64_C(1000000000)
int main(void)
{
    CastTimeline *t = malloc(sizeof *t), *before = malloc(sizeof *before);
    assert(t && before);
    char error[256];
    timeline_init(t);
    assert(!timeline_validate(t, error, sizeof error));
    uint64_t video, audio, clip, sound, right;
    assert(!timeline_add_media(t, "demo.mp4", CAST_TIMELINE_VIDEO, 30 * SECOND,
                               &video, error, sizeof error));
    assert(!timeline_add_media(t, "music.wav", CAST_TIMELINE_AUDIO, 60 * SECOND,
                               &audio, error, sizeof error));
    assert(!timeline_add_clip(t, video, 0, 0, 2 * SECOND, 20 * SECOND,
                              &clip, error, sizeof error));
    assert(!timeline_add_clip(t, audio, 1, 0, 0, 30 * SECOND, &sound, error, sizeof error));
    assert(timeline_duration(t) == 30 * SECOND);
    *before = *t;
    assert(timeline_add_clip(t, video, 0, 10 * SECOND, 0, SECOND, NULL, error, sizeof error));
    assert(!memcmp(before, t, sizeof *t));
    assert(timeline_trim(t, clip, 29 * SECOND, 2 * SECOND, error, sizeof error));
    assert(!memcmp(before, t, sizeof *t));
    assert(timeline_move(t, clip, 16, 0, error, sizeof error));
    assert(timeline_gain(t, sound, NAN, false, error, sizeof error));
    assert(timeline_trim(t, clip, 0, UINT64_MAX, error, sizeof error));
    assert(!memcmp(before, t, sizeof *t));
    assert(!timeline_split(t, clip, 8 * SECOND, &right, error, sizeof error));
    assert(timeline_clip(t, right)->source_in_ns == 10 * SECOND);
    assert(timeline_clip(t, right)->duration_ns == 12 * SECOND);
    assert(timeline_clip(t, clip)->duration_ns == 8 * SECOND);
    assert(!timeline_move(t, right, 0, 12 * SECOND, error, sizeof error));
    assert(!timeline_trim(t, right, 12 * SECOND, 8 * SECOND, error, sizeof error));
    assert(!timeline_gain(t, sound, .25f, true, error, sizeof error));
    assert(timeline_clip(t, sound)->muted && timeline_clip(t, sound)->gain == .25f);
    assert(!timeline_validate(t, error, sizeof error));

    CastTimelineHistory *h = timeline_history_open(2);
    assert(h && !timeline_history_open(33));
    assert(!timeline_checkpoint(h, t, error, sizeof error));
    uint64_t saved_revision = t->revision;
    assert(!timeline_remove(t, right, error, sizeof error));
    assert(!timeline_undo(h, t, error, sizeof error));
    assert(timeline_clip(t, right) && t->revision > saved_revision);
    assert(!timeline_redo(h, t, error, sizeof error));
    assert(!timeline_clip(t, right));
    assert(!timeline_undo(h, t, error, sizeof error));
    assert(!timeline_checkpoint(h, t, error, sizeof error));
    assert(!timeline_gain(t, sound, .5f, false, error, sizeof error));
    assert(timeline_redo(h, t, error, sizeof error));
    timeline_history_close(h);

    /* The writer's live clip cannot be split, moved, muted or removed. Other
     * tracks remain editable and the writer can finalize its growing range. */
    timeline_init(t);
    assert(!timeline_add_media(t, "live.mkv", CAST_TIMELINE_VIDEO, 0, &video, error, sizeof error));
    assert(!timeline_add_clip(t, video, 0, 0, 0, SECOND, &clip, error, sizeof error));
    assert(!timeline_capture_update(t, clip, 3 * SECOND, true, error, sizeof error));
    *before = *t;
    assert(timeline_split(t, clip, SECOND, NULL, error, sizeof error));
    assert(timeline_move(t, clip, 1, SECOND, error, sizeof error));
    assert(timeline_gain(t, clip, 0, true, error, sizeof error));
    assert(timeline_remove(t, clip, error, sizeof error));
    assert(!memcmp(before, t, sizeof *t));
    assert(!timeline_add_media(t, "music.wav", CAST_TIMELINE_AUDIO, 30 * SECOND,
                               &audio, error, sizeof error));
    assert(!timeline_add_clip(t, audio, 1, 0, 0, 3 * SECOND, &sound, error, sizeof error));
    assert(!timeline_trim(t, sound, SECOND, SECOND, error, sizeof error));
    assert(!timeline_capture_update(t, clip, 5 * SECOND, false, error, sizeof error));
    assert(!timeline_split(t, clip, 2 * SECOND, NULL, error, sizeof error));
    assert(!timeline_validate(t, error, sizeof error));
    t->media[0].id = t->media[1].id;
    assert(timeline_validate(t, error, sizeof error));
    *t = *before;
    t->clip_count = CAST_TIMELINE_CLIP_MAX + 1;
    assert(timeline_validate(t, error, sizeof error));
    free(before);
    free(t);
    puts("Timeline: source bounds, track collisions, edits, history and live capture protection passed");
    return 0;
}
