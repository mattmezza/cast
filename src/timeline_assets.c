#include "timeline_assets.h"
#include "timeline_render.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { CastTimelineAssetRequest request; uint64_t id; } Job;
typedef struct { Job job; CastTimelineAssetResult result; uint64_t used; } Entry;
struct CastTimelineAssets {
    pthread_mutex_t mutex; pthread_cond_t wake; pthread_t worker;
    atomic_bool stop; atomic_uint_fast64_t generation;
    uint64_t active_generation, active_deadline, next_id, tick;
    Job queue[CAST_ASSET_QUEUE], active; unsigned pending;
    Entry entries[CAST_ASSET_CACHE]; bool busy;
};
static bool superseded(CastTimelineAssets *s)
{ return atomic_load(&s->stop) || atomic_load(&s->generation)!=s->active_generation; }
static bool cancelled(void *user)
{
    CastTimelineAssets *s=user;
    return superseded(s) || cast_now_ns()>s->active_deadline;
}
static void explain(char *out,size_t size,const char *error)
{ snprintf(out,size,"%.255s",error && *error ? error : "media decode failed"); }
static void decode(CastTimelineAssets *s,const Job *job,CastTimelineAssetResult *out)
{
    const CastTimelineAssetRequest *q=&job->request;
    *out=(CastTimelineAssetResult){.id=job->id,.generation=q->generation,.media_id=q->media.id,
        .source_start_ns=q->source_start_ns,.span_ns=q->span_ns};
    CastTimeline *t=calloc(1,sizeof *t); Config *cfg=malloc(sizeof *cfg);
    char error[CAST_ERR]="allocation failed";
    if (!t || !cfg) goto failed;
    config_defaults(cfg); cfg->width=CAST_ASSET_WIDTH; cfg->height=CAST_ASSET_HEIGHT;
    timeline_init(t); t->media[0]=q->media; t->media_count=1;
    t->clips[0]=(CastTimelineClip){.id=q->media.id+1,.media_id=q->media.id,.gain=1,
        .duration_ns=q->span_ns,.source_in_ns=q->source_start_ns};
    t->clip_count=1; t->next_id=q->media.id+2;
    CastTimelineRenderer *r=timeline_renderer_open(t,cfg,cancelled,s,error,sizeof error);
    if (!r) goto failed;
    if (q->thumbnail) {
        Frame frame={0};
        if (q->media.kind==CAST_TIMELINE_AUDIO) {
            explain(out->thumbnail_error,sizeof out->thumbnail_error,"source is audio only");
        } else if (!timeline_renderer_frame(r,0,&frame,error,sizeof error)) {
            for (unsigned y=0;y<CAST_ASSET_HEIGHT;++y)
                memcpy(out->rgba+y*CAST_ASSET_WIDTH*4,frame.data+(size_t)y*frame.stride,CAST_ASSET_WIDTH*4);
            out->thumbnail_ready=true;
        } else explain(out->thumbnail_error,sizeof out->thumbnail_error,error);
        frame_free(&frame);
    }
    if (q->waveform && !cancelled(s)) {
        if (q->media.kind==CAST_TIMELINE_IMAGE) {
            explain(out->waveform_error,sizeof out->waveform_error,"source is an image");
        } else {
            bool present=false;
            if (timeline_renderer_audio_present(r,q->media.id,&present,error,sizeof error) || !present) {
                explain(out->waveform_error,sizeof out->waveform_error,
                        *error ? error : "source has no audio stream");
                timeline_renderer_close(r);free(t);free(cfg);return;
            }
            uint64_t total=q->span_ns*48000/UINT64_C(1000000000);
            unsigned bins=total<CAST_ASSET_BINS?(unsigned)total:CAST_ASSET_BINS;
            float samples[960]; bool ok=true;
            for (unsigned bin=0;bin<bins && ok;++bin) {
                uint64_t first=total*bin/bins,last=total*(bin+1)/bins;
                double sum[2]={0};
                for (uint64_t position=first;position<last;) {
                    unsigned n=last-position>480?480:(unsigned)(last-position);
                    if (cancelled(s) || timeline_renderer_audio(r,position,n,samples,error,sizeof error)) {ok=false;break;}
                    for (unsigned i=0;i<n;++i) for (unsigned ch=0;ch<2;++ch) {
                        float value=samples[i*2+ch];
                        if (!isfinite(value)) value=0;
                        value=fmaxf(-1,fminf(1,value));
                        out->peak[bin][ch]=fmaxf(out->peak[bin][ch],fabsf(value));
                        sum[ch]+=(double)value*value;
                    }
                    position+=n;
                }
                for (unsigned ch=0;ch<2;++ch) out->rms[bin][ch]=(float)sqrt(sum[ch]/(last-first));
            }
            if (ok && !cancelled(s)) {out->bins=bins;out->waveform_ready=true;}
            else {memset(out->peak,0,sizeof out->peak);memset(out->rms,0,sizeof out->rms);explain(out->waveform_error,sizeof out->waveform_error,error);}
        }
    }
    if(q->waveform&&!out->waveform_ready&&!out->waveform_error[0]&&!superseded(s))
        explain(out->waveform_error,sizeof out->waveform_error,"media asset decode timed out");
    timeline_renderer_close(r); free(t);free(cfg);return;
failed:
    if (q->thumbnail) explain(out->thumbnail_error,sizeof out->thumbnail_error,error);
    if (q->waveform) explain(out->waveform_error,sizeof out->waveform_error,error);
    free(t);free(cfg);
}
static void *worker(void *user)
{
    CastTimelineAssets *s=user; CastTimelineAssetResult storage, *result=&storage;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->pending && !atomic_load(&s->stop)) pthread_cond_wait(&s->wake,&s->mutex);
        if (atomic_load(&s->stop)) {pthread_mutex_unlock(&s->mutex);break;}
        Job job=s->queue[0]; memmove(s->queue,s->queue+1,(--s->pending)*sizeof(Job));
        s->active=job;s->busy=true;s->active_generation=job.request.generation;s->active_deadline=cast_now_ns()+UINT64_C(5000000000);
        pthread_mutex_unlock(&s->mutex);
        if (result) decode(s,&job,result);
        pthread_mutex_lock(&s->mutex);
        if (!superseded(s) && result) {
            unsigned oldest=0;
            for (unsigned i=1;i<CAST_ASSET_CACHE;++i) if(s->entries[i].used<s->entries[oldest].used) oldest=i;
            s->entries[oldest].job=job;s->entries[oldest].result=*result;s->entries[oldest].used=++s->tick;
        }
        s->busy=false;memset(&s->active,0,sizeof s->active);
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
CastTimelineAssets *timeline_assets_open(void)
{
    CastTimelineAssets *s=calloc(1,sizeof *s);if(!s)return NULL;
    if(pthread_mutex_init(&s->mutex,NULL)){free(s);return NULL;}
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->mutex);free(s);return NULL;}
    atomic_init(&s->stop,false);atomic_init(&s->generation,0);
    if(pthread_create(&s->worker,NULL,worker,s)){pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);free(s);return NULL;}
    return s;
}
static void advance(CastTimelineAssets *s,uint64_t generation)
{
    atomic_store(&s->generation,generation);s->pending=0;
    memset(s->entries,0,sizeof s->entries);
}
static bool same(const Job *job,const CastTimelineAssetRequest *q)
{
    const CastTimelineAssetRequest *p=&job->request;
    return job->id && p->generation==q->generation && p->media.id==q->media.id &&
        p->media.kind==q->media.kind && p->media.duration_ns==q->media.duration_ns &&
        !strcmp(p->media.path,q->media.path) && p->source_start_ns==q->source_start_ns &&
        p->span_ns==q->span_ns && p->thumbnail==q->thumbnail && p->waveform==q->waveform;
}
int timeline_assets_request(CastTimelineAssets *s,const CastTimelineAssetRequest *q,uint64_t *id)
{
    if(!s||!q||!id||!q->generation||!q->media.id||q->media.id>UINT64_MAX-2||
       !memchr(q->media.path,0,sizeof q->media.path)||q->media.path[0]!='/'||q->media.missing||
       q->media.kind<CAST_TIMELINE_VIDEO||q->media.kind>CAST_TIMELINE_IMAGE||!q->span_ns||q->span_ns>CAST_ASSET_SPAN_MAX_NS||
       q->span_ns<20834||q->media.duration_ns>CAST_TIMELINE_LIMIT_NS||
       q->source_start_ns>=q->media.duration_ns||q->span_ns>q->media.duration_ns-q->source_start_ns||
       (!q->thumbnail&&!q->waveform))return -1;
    pthread_mutex_lock(&s->mutex);
    if(q->generation<atomic_load(&s->generation)){pthread_mutex_unlock(&s->mutex);return -1;}
    if(q->generation>atomic_load(&s->generation))advance(s,q->generation);
    for(unsigned i=0;i<CAST_ASSET_CACHE;++i) if(same(&s->entries[i].job,q)){
        *id=s->entries[i].job.id;s->entries[i].used=++s->tick;pthread_mutex_unlock(&s->mutex);return 0;}
    if(s->busy&&same(&s->active,q)){*id=s->active.id;pthread_mutex_unlock(&s->mutex);return 0;}
    for(unsigned i=0;i<s->pending;++i)if(same(&s->queue[i],q)){*id=s->queue[i].id;pthread_mutex_unlock(&s->mutex);return 0;}
    if(s->pending==CAST_ASSET_QUEUE){pthread_mutex_unlock(&s->mutex);return 1;}
    if(s->next_id==UINT64_MAX){pthread_mutex_unlock(&s->mutex);return -1;}
    *id=++s->next_id;s->queue[s->pending++]=(Job){.request=*q,.id=*id};
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);return 0;
}
int timeline_assets_copy(CastTimelineAssets *s,uint64_t generation,uint64_t id,CastTimelineAssetResult *out)
{
    if(!s||!out)return 1;
    pthread_mutex_lock(&s->mutex);
    if(generation==atomic_load(&s->generation))for(unsigned i=0;i<CAST_ASSET_CACHE;++i)
        if(s->entries[i].job.id==id&&id){*out=s->entries[i].result;s->entries[i].used=++s->tick;pthread_mutex_unlock(&s->mutex);return 0;}
    pthread_mutex_unlock(&s->mutex);return 1;
}
void timeline_assets_status(CastTimelineAssets *s,CastTimelineAssetStatus *out)
{
    if(!s||!out)return;
    pthread_mutex_lock(&s->mutex);
    *out=(CastTimelineAssetStatus){.generation=atomic_load(&s->generation),.pending=s->pending,.busy=s->busy};
    for(unsigned i=0;i<CAST_ASSET_CACHE;++i)out->cached+=s->entries[i].job.id!=0;
    pthread_mutex_unlock(&s->mutex);
}
void timeline_assets_cancel(CastTimelineAssets *s,uint64_t generation)
{if(!s)return;pthread_mutex_lock(&s->mutex);if(generation>atomic_load(&s->generation))advance(s,generation);pthread_mutex_unlock(&s->mutex);}
void timeline_assets_close(CastTimelineAssets *s)
{
    if(!s)return;
    pthread_mutex_lock(&s->mutex);atomic_store(&s->stop,true);pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);
    pthread_join(s->worker,NULL);
    pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);free(s);
}
