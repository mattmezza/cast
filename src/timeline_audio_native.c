#include "timeline_audio.h"
CastTimelineAudio *timeline_audio_native_open(const CastTimeline *timeline, const Config *config)
{
    CastAudioMonitor *monitor = audio_monitor_native_create();
    if (!monitor) {
        return NULL;
    }
    CastTimelineAudio *audio = timeline_audio_open(timeline, config, monitor);
    if (!audio) {
        audio_monitor_free(monitor);
    }
    return audio;
}
