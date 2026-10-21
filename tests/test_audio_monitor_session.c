/* Real Media binding with isolated fake capture/output; no device API linked. */
#define _POSIX_C_SOURCE 200809L
#include "audio_monitor_session.h"
#include "cast.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {
    pthread_mutex_t lock;pthread_cond_t wake;
    uint64_t latest,epoch;bool enabled,available,blocked,entered,release;
    int result;float value;unsigned reads,opens,closes;
    atomic_uint_fast64_t now;
    CastMonitorRender render;void *context;
} Fake;
static Fake fake;
static uint64_t clock_ns(void *data){return atomic_load(&((Fake *)data)->now);}
static int open_output(void *data,CastMonitorRender render,void *context,char *error,size_t size)
{(void)error;(void)size;Fake *f=data;f->opens++;f->render=render;f->context=context;return 0;}
static void close_output(void *data){Fake *f=data;f->closes++;f->render=NULL;f->context=NULL;}
CastAudioMonitor *audio_monitor_native_create(void)
{
    CastMonitorBackend backend={.open=open_output,.close=close_output,.now_ns=clock_ns};
    return audio_monitor_create(&backend,&fake);
}
void media_audio_levels(Media *media,AudioLevels *levels)
{
    assert((void *)media==&fake);pthread_mutex_lock(&fake.lock);memset(levels,0,sizeof *levels);
    levels->supported=true;levels->mic.enabled=fake.enabled;levels->mic.available=fake.available;levels->mic.fresh=fake.available;
    levels->mic.epoch=fake.epoch;levels->mic.timestamp_ns=fake.latest;pthread_mutex_unlock(&fake.lock);
}
int media_audio_selection(Media *media,int source,uint64_t first,float *samples,unsigned frames,char *error,size_t size)
{
    assert((void *)media==&fake&&source==0&&frames==480&&first>0);
    pthread_mutex_lock(&fake.lock);fake.reads++;fake.entered=true;pthread_cond_broadcast(&fake.wake);
    while(fake.blocked&&!fake.release)pthread_cond_wait(&fake.wake,&fake.lock);
    for(unsigned i=0;i<frames*2;i++)samples[i]=fake.value;
    int result=fake.result;if(result<0)snprintf(error,size,"fake capture unavailable");
    pthread_mutex_unlock(&fake.lock);return result;
}
static void delay(void){struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);}
static void ready(CastMonitorSession *s)
{
    for(unsigned i=0;i<1000;i++){CastMonitorSessionStatus status;audio_monitor_session_status(s,&status);if(status.capture_ready&&status.output.queued_frames)return;delay();}assert(!"producer timeout");
}
static void entered(void)
{
    for(unsigned i=0;i<1000;i++){pthread_mutex_lock(&fake.lock);bool value=fake.entered;pthread_mutex_unlock(&fake.lock);if(value)return;delay();}assert(!"capture entry timeout");
}
static void zero(const float *out){for(unsigned i=0;i<960;i++)assert(out[i]==0);}
int main(void)
{
    pthread_mutex_init(&fake.lock,NULL);pthread_cond_init(&fake.wake,NULL);atomic_init(&fake.now,UINT64_C(1000000000));
    fake.latest=UINT64_C(1000000000);fake.epoch=1;fake.enabled=fake.available=true;fake.value=.25f;
    CastMonitorSession *s=audio_monitor_session_media_create((Media *)&fake);assert(s&&fake.opens==0);
    char error[256];assert(!audio_monitor_session_start(s,error,sizeof error)&&fake.opens==1);
    for(unsigned i=0;i<20;i++)delay();
    pthread_mutex_lock(&fake.lock);assert(!fake.reads);pthread_mutex_unlock(&fake.lock); /* Initial privacy blocks capture reads. */
    CastMonitorSessionState state={.mic_enabled=true,.route_epoch=1};audio_monitor_session_update(s,state);ready(s);
    float out[960];fake.render(fake.context,out,480);for(unsigned i=0;i<960;i++)assert(out[i]==.25f);
    state.privacy=true;audio_monitor_session_update(s,state);fake.render(fake.context,out,480);zero(out);
    /* A privacy update while a read is in flight retires that completion. */
    pthread_mutex_lock(&fake.lock);fake.blocked=true;fake.entered=false;fake.release=false;fake.latest+=UINT64_C(10000000);atomic_store(&fake.now,fake.latest);pthread_mutex_unlock(&fake.lock);
    state.privacy=false;audio_monitor_session_update(s,state);entered();
    state.privacy=true;audio_monitor_session_update(s,state);
    pthread_mutex_lock(&fake.lock);fake.release=true;pthread_cond_broadcast(&fake.wake);pthread_mutex_unlock(&fake.lock);
    for(unsigned i=0;i<20;i++)delay();
    fake.render(fake.context,out,480);zero(out);
    /* Pending capture never becomes a fabricated successful PCM batch. */
    pthread_mutex_lock(&fake.lock);fake.blocked=false;fake.entered=false;fake.result=1;fake.latest+=UINT64_C(10000000);atomic_store(&fake.now,fake.latest);pthread_mutex_unlock(&fake.lock);
    state.privacy=false;audio_monitor_session_update(s,state);entered();
    for(unsigned i=0;i<20;i++)delay();
    CastMonitorSessionStatus pending;audio_monitor_session_status(s,&pending);
    assert(!pending.capture_ready&&!pending.output.queued_frames);fake.render(fake.context,out,480);zero(out);
    state.privacy=true;audio_monitor_session_update(s,state);
    /* Epoch/source replacement discards the old route and uses only new PCM. */
    pthread_mutex_lock(&fake.lock);fake.blocked=false;fake.result=0;fake.epoch++;fake.latest+=UINT64_C(10000000);fake.value=.75f;atomic_store(&fake.now,fake.latest);pthread_mutex_unlock(&fake.lock);
    state.privacy=false;state.route_epoch++;audio_monitor_session_update(s,state);ready(s);fake.render(fake.context,out,480);for(unsigned i=0;i<960;i++)assert(out[i]==.75f);
    state.mic_enabled=false;audio_monitor_session_update(s,state);fake.render(fake.context,out,480);zero(out);
    audio_monitor_session_stop(s);unsigned reads=fake.reads;for(unsigned i=0;i<20;i++)delay();assert(fake.reads==reads&&fake.closes==1);
    CastMonitorSessionStatus status;audio_monitor_session_status(s,&status);assert(!status.running&&!status.output.enabled&&!status.output.queued_frames);
    audio_monitor_session_free(s);assert(fake.closes==1);pthread_cond_destroy(&fake.wake);pthread_mutex_destroy(&fake.lock);
    puts("monitor session PASS: inert default, microphone-only real binding, worker playback, in-flight privacy retirement, route epoch, mute and joined shutdown");
}
