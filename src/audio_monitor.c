#define _POSIX_C_SOURCE 200809L
#include "audio_monitor.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define STALE_NS UINT64_C(100000000)
#define FUTURE_NS UINT64_C(50000000)
struct CastAudioMonitor {
    pthread_mutex_t lock;
    CastMonitorBackend ops;
    void *backend;
    bool enabled;
    uint64_t epoch,played,dropped,next_ns;
    unsigned begin,count;
    float samples[CAST_MONITOR_CAPACITY*2];
    uint64_t timestamps[CAST_MONITOR_CAPACITY];
};
static uint64_t now_ns(CastAudioMonitor *m)
{
    if(m->ops.now_ns)return m->ops.now_ns(m->backend);
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static uint64_t offset_ns(unsigned frames){return (uint64_t)frames*UINT64_C(1000000000)/CAST_MONITOR_RATE;}
static void clear(CastAudioMonitor *m)
{
    m->dropped+=m->count;m->begin=m->count=0;m->next_ns=0;
    memset(m->samples,0,sizeof m->samples);memset(m->timestamps,0,sizeof m->timestamps);
    ++m->epoch;
}
static void retire(CastAudioMonitor *m)
{
    unsigned at=m->begin;m->samples[at*2]=m->samples[at*2+1]=0;m->timestamps[at]=0;
    m->begin=(at+1)%CAST_MONITOR_CAPACITY;--m->count;
}
static void render(void *context,float *out,unsigned frames)
{
    CastAudioMonitor *m=context;
    if(!out||!frames)return;
    memset(out,0,(size_t)frames*2*sizeof *out);
    /* A producer/privacy transition cannot stall the device callback. */
    if(pthread_mutex_trylock(&m->lock))return;
    uint64_t now=now_ns(m);
    if(m->enabled)for(unsigned i=0;i<frames;i++){
        uint64_t due=now+offset_ns(i);
        while(m->count&&due>m->timestamps[m->begin]&&due-m->timestamps[m->begin]>STALE_NS){retire(m);++m->dropped;}
        if(!m->count)break;
        if(m->timestamps[m->begin]>due)continue;
        out[i*2]=m->samples[m->begin*2];out[i*2+1]=m->samples[m->begin*2+1];retire(m);++m->played;
    }
    pthread_mutex_unlock(&m->lock);
}
CastAudioMonitor *audio_monitor_create(const CastMonitorBackend *ops,void *backend)
{
    if(!ops||!ops->open||!ops->close)return NULL;
    CastAudioMonitor *m=calloc(1,sizeof *m);if(!m)return NULL;
    if(pthread_mutex_init(&m->lock,NULL)){free(m);return NULL;}
    m->ops=*ops;m->backend=backend;m->epoch=1;return m;
}
int audio_monitor_start(CastAudioMonitor *m,char *error,size_t size)
{
    if(!m){if(error&&size)snprintf(error,size,"Audio monitor unavailable");return -1;}
    if(error&&size)error[0]=0;
    pthread_mutex_lock(&m->lock);bool enabled=m->enabled;pthread_mutex_unlock(&m->lock);
    if(enabled)return 0;
    if(m->ops.open(m->backend,render,m,error,size)){
        m->ops.close(m->backend);return -1;
    }
    pthread_mutex_lock(&m->lock);clear(m);m->enabled=true;pthread_mutex_unlock(&m->lock);return 0;
}
void audio_monitor_stop(CastAudioMonitor *m)
{
    if(!m)return;
    pthread_mutex_lock(&m->lock);bool was=m->enabled;m->enabled=false;clear(m);pthread_mutex_unlock(&m->lock);
    if(was)m->ops.close(m->backend);
}
uint64_t audio_monitor_barrier(CastAudioMonitor *m)
{
    if(!m)return 0;
    pthread_mutex_lock(&m->lock);clear(m);uint64_t epoch=m->epoch;pthread_mutex_unlock(&m->lock);return epoch;
}
int audio_monitor_submit(CastAudioMonitor *m,uint64_t epoch,uint64_t first,const float *in,unsigned frames)
{
    if(!m||!in||!frames||frames>CAST_MONITOR_CAPACITY||!first||first>UINT64_MAX-offset_ns(frames))return -1;
    uint64_t now=now_ns(m),end=first+offset_ns(frames);
    if(first>now&&first-now>FUTURE_NS)return -1;
    pthread_mutex_lock(&m->lock);
    if(!m->enabled||epoch!=m->epoch||(now>end&&now-end>STALE_NS)){pthread_mutex_unlock(&m->lock);return 0;}
    /* Out-of-order/overlapping batches are not spliced into a live route. */
    if(m->next_ns&&first<m->next_ns){pthread_mutex_unlock(&m->lock);return -1;}
    while(m->count+frames>CAST_MONITOR_CAPACITY){retire(m);++m->dropped;}
    for(unsigned i=0;i<frames;i++){
        unsigned at=(m->begin+m->count)%CAST_MONITOR_CAPACITY;
        for(unsigned channel=0;channel<2;channel++){float sample=in[i*2+channel];m->samples[at*2+channel]=isfinite(sample)?fmaxf(-1,fminf(1,sample)):0;}
        m->timestamps[at]=first+offset_ns(i);++m->count;
    }
    m->next_ns=end;
    pthread_mutex_unlock(&m->lock);return 1;
}
void audio_monitor_status(CastAudioMonitor *m,CastMonitorStatus *out)
{
    if(!out)return;
    memset(out,0,sizeof *out);if(!m)return;
    pthread_mutex_lock(&m->lock);out->enabled=m->enabled;out->epoch=m->epoch;out->queued_frames=m->count;out->played_frames=m->played;out->dropped_frames=m->dropped;pthread_mutex_unlock(&m->lock);
    out->healthy=out->enabled&&(!m->ops.health||!m->ops.health(m->backend,out->error,sizeof out->error));
}
void audio_monitor_free(CastAudioMonitor *m)
{
    if(!m)return;
    audio_monitor_stop(m);if(m->ops.destroy)m->ops.destroy(m->backend);pthread_mutex_destroy(&m->lock);free(m);
}
