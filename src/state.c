#include "cast.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

uint64_t cast_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* Pure output-state transitions. The daemon performs media barriers before ACK. */
int state_command(State *s, const char *output, const char *action, char *err, size_t n)
{
    if (!strcmp(output, "pause")) {
        if (!s->group_paused) {
            s->group_paused = true;
            s->group_live_restore = !s->live_paused;
            s->group_record_restore = s->recording && !s->record_paused;
            s->live_paused = true;
            if (s->recording) {
                s->record_paused = true;
            }
        }
        return 0;
    }
    if (!strcmp(output, "resume")) {
        if (s->group_paused) {
            if (s->group_live_restore) {
                s->live_paused = false;
            }
            if (s->group_record_restore && s->recording) {
                s->record_paused = false;
            }
            s->group_paused = s->group_live_restore = s->group_record_restore = false;
        }
        return 0;
    }
    if (!strcmp(output, "live")) {
        bool solid = true;
        if (!strcmp(action, "pause")) {
            s->live_paused = true;
        } else if (!strcmp(action, "resume")) {
            s->live_paused = false;
        } else if (!strcmp(action, "toggle")) {
            s->live_paused = !s->live_paused;
        } else if (!strcmp(action, "freeze")) {
            s->live_frozen = true;
            solid = false;
        } else if (!strcmp(action, "unfreeze")) {
            s->live_frozen = false;
            solid = false;
        } else if (!strcmp(action, "blur")) {
            s->live_blurred = true;
            solid = false;
        } else if (!strcmp(action, "unblur")) {
            s->live_blurred = false;
            solid = false;
        } else if (!strcmp(action, "blur-toggle")) {
            s->live_blurred = !s->live_blurred;
            solid = false;
        } else {
            goto invalid;
        }
        if (solid) {
            s->group_live_restore = false;
        }
        return 0;
    }
    if (!strcmp(output, "record")) {
        if (!s->recording) {
            snprintf(err, n, "no recording exists; use cast record start [PATH]");
            return -1;
        }
        bool solid = true;
        if (!strcmp(action, "pause")) {
            s->record_paused = true;
        } else if (!strcmp(action, "resume")) {
            if (s->record_cut) {
                s->record_cut = false;
                solid = false;
            } else {
                s->record_paused = false;
            }
        } else if (!strcmp(action, "toggle")) {
            s->record_paused = !s->record_paused;
        } else if (!strcmp(action, "freeze")) {
            s->record_frozen = true;
            solid = false;
        } else if (!strcmp(action, "unfreeze")) {
            s->record_frozen = false;
            solid = false;
        } else if (!strcmp(action, "blur")) {
            s->record_blurred = true;
            solid = false;
        } else if (!strcmp(action, "unblur")) {
            s->record_blurred = false;
            solid = false;
        } else if (!strcmp(action, "blur-toggle")) {
            s->record_blurred = !s->record_blurred;
            solid = false;
        } else if (!strcmp(action, "cut")) {
            s->record_cut = true;
            solid = false;
        } else {
            goto invalid;
        }
        if (solid) {
            s->group_record_restore = false;
        }
        return 0;
    }
invalid:
    snprintf(err, n, "invalid %s state action: %s", output, action ? action : "");
    return -1;
}
