#include "audio_monitor_controller.h"
CastMonitorController *audio_monitor_controller_create(struct Media *media)
{
    CastMonitorSession *session=audio_monitor_session_media_create(media);if(!session)return NULL;
    CastMonitorController *controller=audio_monitor_controller_create_with_session(session);
    if(!controller)audio_monitor_session_free(session);
    return controller;
}
