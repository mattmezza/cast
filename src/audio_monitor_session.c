#define _POSIX_C_SOURCE 200809L
#include "audio_monitor_session.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHUNK 480u
#define CHUNK_NS UINT64_C(10000000)
struct CastMonitorSession {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    CastAudioMonitor *monitor;
    CastMonitorCapture input;
    void *capture;
    CastMonitorSessionState state;
    bool running,stop,capture_ready,observed_available;
    uint64_t generation,monitor_epoch,observed_epoch;
    char error[256];
};
static bool allowed(CastMonitorSession *s){return s->state.mic_enabled&&!s->state.privacy;}
static void barrier(CastMonitorSession *s)
{
    s->monitor_epoch=audio_monitor_barrier(s->monitor);++s->generation;s->capture_ready=false;
}
static void *produce(void *data)
{
    CastMonitorSession *s=data;uint64_t cursor=0,previous_generation=0;
    float pcm[CHUNK*2];
    for(;;){
        pthread_mutex_lock(&s->lock);
        if(s->stop){pthread_mutex_unlock(&s->lock);break;}
        bool permitted=allowed(s);uint64_t generation=s->generation;
        if(generation!=previous_generation){cursor=0;previous_generation=generation;}
        pthread_mutex_unlock(&s->lock);
        if(permitted){
            CastMonitorInput before={0};s->input.snapshot(s->capture,&before);
            pthread_mutex_lock(&s->lock);
            bool available=before.enabled&&before.available;
            if(generation==s->generation&&(s->observed_epoch!=before.epoch||s->observed_available!=available)){
                s->observed_epoch=before.epoch;s->observed_available=available;barrier(s);generation=s->generation;
                previous_generation=generation;cursor=0;
            }
            bool current=!s->stop&&generation==s->generation&&allowed(s);
            pthread_mutex_unlock(&s->lock);
            if(current&&before.enabled&&before.available&&before.latest_ns>CHUNK_NS){
                /* Follow source time, not a fabricated wall-clock silence feed.
                 * A delayed decode/capture must not build playback latency. */
                if(!cursor||before.latest_ns>cursor+CHUNK_NS*4)cursor=before.latest_ns-CHUNK_NS;
                if(cursor<=before.latest_ns-CHUNK_NS){
                    char error[256]="";int result=s->input.read(s->capture,cursor,pcm,CHUNK,error,sizeof error);
                    CastMonitorInput after={0};s->input.snapshot(s->capture,&after);
                    pthread_mutex_lock(&s->lock);
                    if(generation==s->generation&&(before.epoch!=after.epoch||!after.enabled||!after.available)){
                        s->observed_epoch=after.epoch;s->observed_available=after.enabled&&after.available;barrier(s);
                        cursor=0;previous_generation=s->generation;
                    }
                    current=!s->stop&&generation==s->generation&&allowed(s)&&before.epoch==after.epoch&&after.enabled&&after.available;
                    if(current&&result==0){
                        int accepted=audio_monitor_submit(s->monitor,s->monitor_epoch,cursor,pcm,CHUNK);
                        s->capture_ready=accepted==1;
                        if(accepted==1){cursor+=CHUNK_NS;s->error[0]=0;}
                    }else if(current){
                        s->capture_ready=false;
                        if(result<0){
                            barrier(s);cursor=0;previous_generation=s->generation;
                            snprintf(s->error,sizeof s->error,"%s",error[0]?error:"Microphone capture unavailable");
                        }
                    }
                    pthread_mutex_unlock(&s->lock);
                    memset(pcm,0,sizeof pcm);
                }
            }
        }
        pthread_mutex_lock(&s->lock);
        if(!s->stop){
            struct timespec due;clock_gettime(CLOCK_REALTIME,&due);due.tv_nsec+=10000000;
            if(due.tv_nsec>=1000000000){due.tv_nsec-=1000000000;++due.tv_sec;}
            pthread_cond_timedwait(&s->wake,&s->lock,&due);
        }
        pthread_mutex_unlock(&s->lock);
    }
    memset(pcm,0,sizeof pcm);return NULL;
}
CastMonitorSession *audio_monitor_session_create(CastAudioMonitor *monitor,const CastMonitorCapture *ops,void *capture)
{
    if(!monitor||!ops||!ops->snapshot||!ops->read)return NULL;
    CastMonitorSession *s=calloc(1,sizeof *s);if(!s)return NULL;
    if(pthread_mutex_init(&s->lock,NULL)){free(s);return NULL;}
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->lock);free(s);return NULL;}
    s->monitor=monitor;s->input=*ops;s->capture=capture;s->state.privacy=true;s->generation=1;return s;
}
int audio_monitor_session_start(CastMonitorSession *s,char *error,size_t size)
{
    if(!s){if(error&&size)snprintf(error,size,"Monitoring session unavailable");return -1;}
    if(s->running)return 0;
    if(audio_monitor_start(s->monitor,error,size))return -1;
    pthread_mutex_lock(&s->lock);s->stop=false;s->observed_epoch=0;s->error[0]=0;barrier(s);pthread_mutex_unlock(&s->lock);
    if(pthread_create(&s->thread,NULL,produce,s)){audio_monitor_stop(s->monitor);if(error&&size)snprintf(error,size,"Cannot start monitoring producer");return -1;}
    pthread_mutex_lock(&s->lock);s->running=true;pthread_mutex_unlock(&s->lock);return 0;
}
void audio_monitor_session_update(CastMonitorSession *s,CastMonitorSessionState state)
{
    if(!s)return;
    pthread_mutex_lock(&s->lock);
    if(s->state.mic_enabled!=state.mic_enabled||s->state.privacy!=state.privacy||s->state.route_epoch!=state.route_epoch){s->state=state;barrier(s);s->error[0]=0;pthread_cond_signal(&s->wake);}
    pthread_mutex_unlock(&s->lock);
}
void audio_monitor_session_stop(CastMonitorSession *s)
{
    if(!s)return;
    pthread_mutex_lock(&s->lock);s->stop=true;barrier(s);pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->lock);
    if(s->running){pthread_join(s->thread,NULL);pthread_mutex_lock(&s->lock);s->running=false;pthread_mutex_unlock(&s->lock);}
    audio_monitor_stop(s->monitor);
}
void audio_monitor_session_status(CastMonitorSession *s,CastMonitorSessionStatus *out)
{
    if(!out)return;
    memset(out,0,sizeof *out);if(!s)return;
    pthread_mutex_lock(&s->lock);out->running=s->running;out->capture_ready=s->capture_ready;snprintf(out->error,sizeof out->error,"%s",s->error);pthread_mutex_unlock(&s->lock);
    audio_monitor_status(s->monitor,&out->output);
}
void audio_monitor_session_free(CastMonitorSession *s)
{
    if(!s)return;
    audio_monitor_session_stop(s);audio_monitor_free(s->monitor);pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->lock);free(s);
}
