/* White-box contention test intentionally holds the queue mutex while the
 * independent fake device callback runs. Never opens a real output device. */
#include "../src/audio_monitor.c"
#include <assert.h>
typedef struct {CastMonitorRender render;void *context;uint64_t now;unsigned opens,closes,destroys;bool fail,broken;} Fake;
static uint64_t fake_clock(void *p){return ((Fake *)p)->now;}
static int fake_open(void *p,CastMonitorRender callback,void *context,char *error,size_t size)
{
    Fake *f=p;f->opens++;f->render=callback;f->context=context;
    float samples[8];for(unsigned i=0;i<8;i++)samples[i]=99;
    callback(context,samples,4);for(unsigned i=0;i<8;i++)assert(samples[i]==0); /* Not acknowledged yet. */
    if(f->fail){snprintf(error,size,"fake output unavailable");return -1;}return 0;
}
static void fake_close(void *p){Fake *f=p;f->closes++;f->render=NULL;f->context=NULL;}
static void fake_destroy(void *p){((Fake *)p)->destroys++;}
static int fake_health(void *p,char *error,size_t size)
{if(!((Fake *)p)->broken)return 0;snprintf(error,size,"fake disconnected");return -1;}
static void zeros(const float *samples,unsigned frames)
{for(unsigned i=0;i<frames*2;i++)assert(samples[i]==0);}
static void *contended(void *p)
{Fake *f=p;float samples[16];f->render(f->context,samples,8);zeros(samples,8);return NULL;}
int main(void)
{
    Fake fake={.now=UINT64_C(1000000000)};
    CastMonitorBackend backend={.open=fake_open,.close=fake_close,.destroy=fake_destroy,.health=fake_health,.now_ns=fake_clock};
    CastAudioMonitor *m=audio_monitor_create(&backend,&fake);assert(m&&fake.opens==0);
    CastMonitorStatus status;audio_monitor_status(m,&status);assert(!status.enabled&&!status.healthy);
    float input[CAST_MONITOR_CAPACITY*2],out[16];for(unsigned i=0;i<CAST_MONITOR_CAPACITY*2;i++)input[i]=.25f;
    assert(audio_monitor_submit(m,status.epoch,fake.now,input,8)==0);
    char error[256];assert(!audio_monitor_start(m,error,sizeof error)&&fake.opens==1);
    assert(!audio_monitor_start(m,error,sizeof error)&&fake.opens==1);audio_monitor_status(m,&status);
    uint64_t epoch=status.epoch;assert(status.enabled&&status.healthy);
    assert(audio_monitor_submit(m,epoch,fake.now,input,8)==1);
    assert(audio_monitor_submit(m,epoch,fake.now,input,8)==-1); /* No duplicate windows. */
    fake.render(fake.context,out,8);for(unsigned i=0;i<16;i++)assert(out[i]==.25f);
    fake.render(fake.context,out,8);zeros(out,8);
    assert(audio_monitor_submit(m,epoch,fake.now,input,8)==-1); /* No replay after consumption. */
    fake.now+=UINT64_C(1000000);
    input[0]=NAN;input[1]=INFINITY;input[2]=-2;input[3]=2;
    assert(audio_monitor_submit(m,epoch,fake.now,input,2)==1);fake.render(fake.context,out,2);
    assert(out[0]==0&&out[1]==0&&out[2]==-1&&out[3]==1);
    fake.now+=UINT64_C(1000000);
    for(unsigned i=0;i<CAST_MONITOR_CAPACITY*2;i++)input[i]=.25f;
    assert(audio_monitor_submit(m,epoch,fake.now+UINT64_C(10000000),input,8)==1);
    fake.render(fake.context,out,8);zeros(out,8);audio_monitor_status(m,&status);assert(status.queued_frames==8);
    fake.now+=UINT64_C(10000000);fake.render(fake.context,out,8);assert(out[0]==.25f);
    fake.now+=UINT64_C(1000000);
    assert(audio_monitor_submit(m,epoch,fake.now-STALE_NS*2,input,8)==0);
    assert(audio_monitor_submit(m,epoch,fake.now+FUTURE_NS+1,input,8)==-1);
    assert(audio_monitor_submit(m,epoch,UINT64_MAX,input,8)==-1);
    assert(audio_monitor_submit(m,epoch,fake.now,input,CAST_MONITOR_CAPACITY+1)==-1);
    assert(audio_monitor_submit(m,epoch,fake.now,input,8)==1);fake.now+=STALE_NS*2;
    fake.render(fake.context,out,8);zeros(out,8);audio_monitor_status(m,&status);assert(!status.queued_frames);
    epoch=audio_monitor_barrier(m);
    assert(audio_monitor_submit(m,epoch,fake.now-80000000,input,3000)==1);
    assert(audio_monitor_submit(m,epoch,fake.now-17500000,input,3000)==1);
    audio_monitor_status(m,&status);assert(status.queued_frames==CAST_MONITOR_CAPACITY&&status.dropped_frames>=1208);
    uint64_t previous=epoch;epoch=audio_monitor_barrier(m);assert(epoch!=previous);
    assert(audio_monitor_submit(m,previous,fake.now,input,8)==0);fake.render(fake.context,out,8);zeros(out,8);
    /* The device callback must never wait on the producer/privacy mutex. */
    pthread_mutex_lock(&m->lock);pthread_t thread;assert(!pthread_create(&thread,NULL,contended,&fake));assert(!pthread_join(thread,NULL));pthread_mutex_unlock(&m->lock);
    fake.broken=true;audio_monitor_status(m,&status);assert(status.enabled&&!status.healthy&&strstr(status.error,"disconnected")&&fake.opens==1);
    audio_monitor_stop(m);assert(fake.closes==1);audio_monitor_status(m,&status);assert(!status.enabled&&!status.queued_frames&&status.epoch!=epoch);
    assert(audio_monitor_submit(m,epoch,fake.now,input,8)==0);
    fake.fail=true;assert(audio_monitor_start(m,error,sizeof error)==-1&&fake.closes==2&&strstr(error,"unavailable"));
    fake.fail=false;fake.broken=false;assert(!audio_monitor_start(m,error,sizeof error));
    audio_monitor_free(m);assert(fake.closes==3&&fake.destroys==1);
    puts("audio monitoring PASS: inert creation, explicit start, finite stereo, bounded overflow, stale/future clocks, epoch cancellation, nonblocking render and quiescent close");
}
