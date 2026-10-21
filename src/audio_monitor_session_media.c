#define _POSIX_C_SOURCE 200809L
#include "audio_monitor_session.h"
#include "cast.h"
static void snapshot(void *context,CastMonitorInput *out)
{
    AudioLevels levels;media_audio_levels(context,&levels);
    *out=(CastMonitorInput){.epoch=levels.mic.epoch,.latest_ns=levels.mic.timestamp_ns,.enabled=levels.mic.enabled,.available=levels.supported&&levels.mic.available&&levels.mic.fresh};
}
static int read_microphone(void *context,uint64_t first,float *stereo,unsigned frames,char *error,size_t size)
{
    return media_audio_selection(context,0,first,stereo,frames,error,size);
}
CastMonitorSession *audio_monitor_session_media_create(Media *media)
{
    if(!media)return NULL;
    CastAudioMonitor *monitor=audio_monitor_native_create();if(!monitor)return NULL;
    CastMonitorCapture capture={.snapshot=snapshot,.read=read_microphone};
    CastMonitorSession *session=audio_monitor_session_create(monitor,&capture,media);
    if(!session)audio_monitor_free(monitor);
    return session;
}
