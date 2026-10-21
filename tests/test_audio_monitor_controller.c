/* Slow fake device lifecycle: no native backend or user capture linked. */
#define _POSIX_C_SOURCE 200809L
#include "audio_monitor_controller.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {
    pthread_mutex_t lock;pthread_cond_t wake;
    bool hold_open,open_entered,hold_close,close_entered,fail;
    unsigned opens,closes;
    atomic_uint reads;
    CastMonitorRender render;void *context;
} Fake;
static uint64_t now(void *data){(void)data;return UINT64_C(1000000000);}
static uint64_t wall(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static void delay(void){struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);}
static int open_output(void *data,CastMonitorRender render,void *context,char *error,size_t size)
{
    Fake *f=data;pthread_mutex_lock(&f->lock);++f->opens;f->render=render;f->context=context;f->open_entered=true;pthread_cond_broadcast(&f->wake);
    while(f->hold_open)pthread_cond_wait(&f->wake,&f->lock);
    bool fail=f->fail;pthread_mutex_unlock(&f->lock);if(fail){snprintf(error,size,"fake unavailable");return -1;}return 0;
}
static void close_output(void *data)
{
    Fake *f=data;pthread_mutex_lock(&f->lock);++f->closes;f->close_entered=true;pthread_cond_broadcast(&f->wake);
    while(f->hold_close)pthread_cond_wait(&f->wake,&f->lock);
    f->render=NULL;f->context=NULL;pthread_mutex_unlock(&f->lock);
}
static void snapshot(void *data,CastMonitorInput *input)
{(void)data;*input=(CastMonitorInput){.epoch=1,.latest_ns=1000000000,.enabled=true,.available=true};}
static int read_input(void *data,uint64_t first,float *pcm,unsigned frames,char *error,size_t size)
{
    (void)first;(void)error;(void)size;atomic_fetch_add(&((Fake *)data)->reads,1);
    for(unsigned i=0;i<frames*2;i++)pcm[i]=.5f;
    return 0;
}
static void wait_state(CastMonitorController *c,CastMonitorControllerState expected)
{
    for(unsigned i=0;i<2000;i++){CastMonitorControllerStatus status;audio_monitor_controller_status(c,&status);if(status.state==expected)return;delay();}assert(!"controller state timeout");
}
static void wait_entered(Fake *f,bool closing)
{
    for(unsigned i=0;i<2000;i++){pthread_mutex_lock(&f->lock);bool entered=closing?f->close_entered:f->open_entered;pthread_mutex_unlock(&f->lock);if(entered)return;delay();}assert(!"backend entry timeout");
}
static void *release_start(void *data)
{Fake *f=data;for(unsigned i=0;i<10;i++)delay();pthread_mutex_lock(&f->lock);f->hold_open=false;pthread_cond_broadcast(&f->wake);pthread_mutex_unlock(&f->lock);return NULL;}
int main(void)
{
    Fake fake={.hold_open=true};pthread_mutex_init(&fake.lock,NULL);pthread_cond_init(&fake.wake,NULL);atomic_init(&fake.reads,0);
    CastMonitorBackend backend={.open=open_output,.close=close_output,.now_ns=now};CastMonitorCapture capture={.snapshot=snapshot,.read=read_input};
    CastAudioMonitor *monitor=audio_monitor_create(&backend,&fake);assert(monitor);
    CastMonitorSession *session=audio_monitor_session_create(monitor,&capture,&fake);assert(session);
    CastMonitorController *c=audio_monitor_controller_create_with_session(session);assert(c&&!fake.opens);
    CastMonitorSessionState state={.mic_enabled=true,.route_epoch=1};audio_monitor_controller_update(c,state);
    uint64_t start=wall();audio_monitor_controller_request_start(c);assert(wall()-start<UINT64_C(250000000));
    wait_entered(&fake,false);CastMonitorControllerStatus status;audio_monitor_controller_status(c,&status);assert(status.state==CAST_MONITOR_STARTING&&!atomic_load(&fake.reads));
    start=wall();audio_monitor_controller_request_stop(c);assert(wall()-start<UINT64_C(250000000));
    pthread_mutex_lock(&fake.lock);fake.hold_open=false;pthread_cond_broadcast(&fake.wake);pthread_mutex_unlock(&fake.lock);
    wait_state(c,CAST_MONITOR_OFF);assert(!atomic_load(&fake.reads)); /* Cancelled start never releases PCM. */
    audio_monitor_controller_request_start(c);wait_state(c,CAST_MONITOR_RUNNING);
    for(unsigned i=0;i<2000;i++){audio_monitor_controller_status(c,&status);if(status.session.output.queued_frames)break;delay();}
    assert(status.session.output.queued_frames==480&&atomic_load(&fake.reads));
    state.privacy=true;audio_monitor_controller_update(c,state);audio_monitor_controller_status(c,&status);assert(!status.session.output.queued_frames&&!status.session.capture_ready);
    float output[960];pthread_mutex_lock(&fake.lock);CastMonitorRender render=fake.render;void *context=fake.context;pthread_mutex_unlock(&fake.lock);
    render(context,output,480);for(unsigned i=0;i<960;i++)assert(output[i]==0);
    /* Slow close also stays off the host thread; a new explicit start wins. */
    pthread_mutex_lock(&fake.lock);fake.hold_close=true;fake.close_entered=false;pthread_mutex_unlock(&fake.lock);
    audio_monitor_controller_request_stop(c);wait_entered(&fake,true);audio_monitor_controller_status(c,&status);assert(status.state==CAST_MONITOR_STOPPING);
    audio_monitor_controller_request_start(c);
    pthread_mutex_lock(&fake.lock);fake.hold_close=false;pthread_cond_broadcast(&fake.wake);pthread_mutex_unlock(&fake.lock);
    wait_state(c,CAST_MONITOR_RUNNING);audio_monitor_controller_request_stop(c);wait_state(c,CAST_MONITOR_OFF);
    pthread_mutex_lock(&fake.lock);fake.fail=true;pthread_mutex_unlock(&fake.lock);
    audio_monitor_controller_request_start(c);wait_state(c,CAST_MONITOR_FAILED);audio_monitor_controller_status(c,&status);assert(strstr(status.error,"unavailable"));
    pthread_mutex_lock(&fake.lock);unsigned opens=fake.opens;pthread_mutex_unlock(&fake.lock);
    for(unsigned i=0;i<30;i++)delay();
    pthread_mutex_lock(&fake.lock);assert(fake.opens==opens);fake.fail=false;fake.hold_open=true;fake.open_entered=false;pthread_mutex_unlock(&fake.lock);
    audio_monitor_controller_request_start(c);wait_entered(&fake,false);
    pthread_t release;assert(!pthread_create(&release,NULL,release_start,&fake));audio_monitor_controller_free(c);assert(!pthread_join(release,NULL));
    assert(fake.opens==fake.closes);pthread_cond_destroy(&fake.wake);pthread_mutex_destroy(&fake.lock);
    puts("monitor controller PASS: nonblocking slow start/stop, cancel without capture/replay, cached status, immediate privacy, coalescing, no retry and joined final shutdown");
}
