#define _POSIX_C_SOURCE 200809L
#include "audio_monitor_controller.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct CastMonitorController {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    CastMonitorSession *session;
    CastMonitorSessionState input;
    CastMonitorControllerStatus status;
    bool desired,active,closing;
    uint64_t generation,revision;
};
static void gate(CastMonitorController *c)
{
    CastMonitorSessionState state=c->input;
    if(c->closing||!c->desired||!c->active||c->status.state!=CAST_MONITOR_RUNNING)state.privacy=true;
    audio_monitor_session_update(c->session,state);
    if(state.privacy||!state.mic_enabled){c->status.session.capture_ready=false;c->status.session.output.queued_frames=0;}
}
static void *lifecycle(void *data)
{
    CastMonitorController *c=data;pthread_mutex_lock(&c->lock);
    for(;;){
        if(c->closing){
            pthread_mutex_unlock(&c->lock);audio_monitor_session_stop(c->session);return NULL;
        }
        if(!c->desired&&!c->active&&c->status.state==CAST_MONITOR_STOPPING)c->status.state=CAST_MONITOR_OFF;
        if(c->desired&&c->active&&c->status.state==CAST_MONITOR_STARTING){c->status.state=CAST_MONITOR_RUNNING;gate(c);}
        if(c->desired&&!c->active){
            uint64_t generation=c->generation;c->status.state=CAST_MONITOR_STARTING;gate(c);
            pthread_mutex_unlock(&c->lock);char error[256]="";
            int result=audio_monitor_session_start(c->session,error,sizeof error);
            CastMonitorSessionStatus started={0};
            if(!result)audio_monitor_session_status(c->session,&started);
            pthread_mutex_lock(&c->lock);
            bool current=!c->closing&&c->desired&&generation==c->generation;
            if(!result&&current){c->active=true;c->status.session=started;c->status.state=CAST_MONITOR_RUNNING;c->status.error[0]=0;gate(c);}
            else {
                /* A cancelled startup never releases the privacy gate. */
                if(!result)c->status.state=CAST_MONITOR_STOPPING;
                pthread_mutex_unlock(&c->lock);audio_monitor_session_stop(c->session);pthread_mutex_lock(&c->lock);
                c->active=false;memset(&c->status.session,0,sizeof c->status.session);
                if(result&&current&&generation==c->generation&&!c->closing){
                    c->desired=false;c->status.state=CAST_MONITOR_FAILED;
                    snprintf(c->status.error,sizeof c->status.error,"%s",error[0]?error:"Audio monitoring startup failed");
                }else {c->status.state=c->desired?CAST_MONITOR_STARTING:CAST_MONITOR_OFF;c->status.error[0]=0;}
                continue;
            }
        }
        if(!c->desired&&c->active){
            c->status.state=CAST_MONITOR_STOPPING;gate(c);
            pthread_mutex_unlock(&c->lock);audio_monitor_session_stop(c->session);pthread_mutex_lock(&c->lock);
            c->active=false;memset(&c->status.session,0,sizeof c->status.session);
            c->status.state=c->desired?CAST_MONITOR_STARTING:CAST_MONITOR_OFF;c->status.error[0]=0;continue;
        }
        if(c->active){
            uint64_t generation=c->generation,revision=c->revision;pthread_mutex_unlock(&c->lock);
            CastMonitorSessionStatus snapshot;audio_monitor_session_status(c->session,&snapshot);
            pthread_mutex_lock(&c->lock);
            if(generation==c->generation&&revision==c->revision&&c->status.state==CAST_MONITOR_RUNNING){
                c->status.session=snapshot;
                if(!snapshot.output.healthy){
                    c->desired=false;c->status.state=CAST_MONITOR_STOPPING;gate(c);
                    pthread_mutex_unlock(&c->lock);audio_monitor_session_stop(c->session);pthread_mutex_lock(&c->lock);
                    c->active=false;memset(&c->status.session,0,sizeof c->status.session);
                    if(generation==c->generation&&!c->desired){
                        c->status.state=CAST_MONITOR_FAILED;
                        snprintf(c->status.error,sizeof c->status.error,"%s",snapshot.output.error[0]?snapshot.output.error:"Audio monitoring output stopped");
                    }else c->status.state=c->desired?CAST_MONITOR_STARTING:CAST_MONITOR_OFF;
                    continue;
                }
            }
        }
        if(c->active){
            struct timespec due;clock_gettime(CLOCK_REALTIME,&due);due.tv_nsec+=20000000;
            if(due.tv_nsec>=1000000000){due.tv_nsec-=1000000000;++due.tv_sec;}
            pthread_cond_timedwait(&c->wake,&c->lock,&due);
        }else if(!c->closing&&!c->desired)pthread_cond_wait(&c->wake,&c->lock);
    }
}
CastMonitorController *audio_monitor_controller_create_with_session(CastMonitorSession *session)
{
    if(!session)return NULL;
    CastMonitorController *c=calloc(1,sizeof *c);if(!c)return NULL;
    if(pthread_mutex_init(&c->lock,NULL)){free(c);return NULL;}
    if(pthread_cond_init(&c->wake,NULL)){pthread_mutex_destroy(&c->lock);free(c);return NULL;}
    c->session=session;c->input.privacy=true;c->generation=1;
    if(pthread_create(&c->thread,NULL,lifecycle,c)){pthread_cond_destroy(&c->wake);pthread_mutex_destroy(&c->lock);free(c);return NULL;}
    return c;
}
void audio_monitor_controller_request_start(CastMonitorController *c)
{
    if(!c)return;
    pthread_mutex_lock(&c->lock);
    if(!c->closing&&!c->desired){c->desired=true;++c->generation;c->status.state=CAST_MONITOR_STARTING;c->status.error[0]=0;gate(c);pthread_cond_signal(&c->wake);}
    pthread_mutex_unlock(&c->lock);
}
void audio_monitor_controller_request_stop(CastMonitorController *c)
{
    if(!c)return;
    pthread_mutex_lock(&c->lock);c->desired=false;++c->generation;
    c->status.state=c->active||c->status.state==CAST_MONITOR_STARTING||c->status.state==CAST_MONITOR_STOPPING?CAST_MONITOR_STOPPING:CAST_MONITOR_OFF;
    c->status.error[0]=0;gate(c);pthread_cond_signal(&c->wake);pthread_mutex_unlock(&c->lock);
}
void audio_monitor_controller_update(CastMonitorController *c,CastMonitorSessionState state)
{
    if(!c)return;
    pthread_mutex_lock(&c->lock);
    if(c->input.mic_enabled!=state.mic_enabled||c->input.privacy!=state.privacy||c->input.route_epoch!=state.route_epoch){
        c->input=state;++c->revision;c->status.session.capture_ready=false;c->status.session.output.queued_frames=0;gate(c);pthread_cond_signal(&c->wake);
    }
    pthread_mutex_unlock(&c->lock);
}
void audio_monitor_controller_status(CastMonitorController *c,CastMonitorControllerStatus *out)
{
    if(!out)return;
    memset(out,0,sizeof *out);if(!c)return;
    pthread_mutex_lock(&c->lock);*out=c->status;pthread_mutex_unlock(&c->lock);
}
void audio_monitor_controller_free(CastMonitorController *c)
{
    if(!c)return;
    pthread_mutex_lock(&c->lock);c->closing=true;c->desired=false;++c->generation;gate(c);pthread_cond_signal(&c->wake);pthread_mutex_unlock(&c->lock);
    pthread_join(c->thread,NULL);audio_monitor_session_free(c->session);pthread_cond_destroy(&c->wake);pthread_mutex_destroy(&c->lock);free(c);
}
