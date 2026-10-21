#include "live_media.h"
#include "timeline_render.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {
    CastTimelineMedia media;
    Config config;
    uint64_t source_in, duration, anchor, generation;
} Job;
struct CastLiveMedia {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t worker;
    atomic_bool closing;
    atomic_uint_fast64_t generation;
    uint64_t active_generation;
    Job selected, pending, *working;
    bool has_selection, queued, frame_valid;
    uint64_t clock_source, clock_anchor, source_end;
    Frame frame;
    float pcm[CAST_LIVE_AUDIO_FRAMES * 2];
    uint64_t pcm_begin, pcm_end;
    CastLiveMediaStatus status;
    uint64_t (*now)(void *);
    void *clock_user;
};
static uint64_t samples(uint64_t ns)
{ return ns / UINT64_C(1000000000) * CAST_LIVE_AUDIO_RATE + ns % UINT64_C(1000000000) * CAST_LIVE_AUDIO_RATE / UINT64_C(1000000000); }
static uint64_t normal_clock(void *unused)
{ (void)unused; return cast_now_ns(); }
static bool cancelled(void *user)
{
    CastLiveMedia *s=user;
    return atomic_load(&s->closing) || atomic_load(&s->generation)!=s->active_generation;
}
static uint64_t position(CastLiveMedia *s,uint64_t now)
{
    uint64_t elapsed=now>s->clock_anchor?now-s->clock_anchor:0;
    uint64_t remaining=s->source_end-s->clock_source;
    return s->clock_source+(elapsed<remaining?elapsed:remaining);
}
static void audio_barrier(CastLiveMedia *s)
{
    ++s->status.audio_epoch;s->pcm_begin=s->pcm_end=0;
    memset(s->pcm,0,sizeof s->pcm);
}
static bool advance(CastLiveMedia *s,uint64_t generation)
{
    if(!generation || generation<=atomic_load(&s->generation) || atomic_load(&s->closing))return false;
    atomic_store(&s->generation,generation);s->status.generation=generation;
    audio_barrier(s);return true;
}
static void wait_tick(CastLiveMedia *s)
{
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=5000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
    pthread_mutex_lock(&s->mutex);
    if(!cancelled(s))pthread_cond_timedwait(&s->wake,&s->mutex,&until);
    pthread_mutex_unlock(&s->mutex);
}
static void failure(CastLiveMedia *s,const char *error)
{
    pthread_mutex_lock(&s->mutex);
    if(!cancelled(s)){
        s->status.state=CAST_LIVE_FAILED;s->frame_valid=false;audio_barrier(s);
        snprintf(s->status.error,sizeof s->status.error,"%.255s",error&&*error?error:"live media decode failed");
    }
    pthread_mutex_unlock(&s->mutex);
}
static CastTimelineRenderer *open_renderer(CastLiveMedia *s,Job *job,char *error,size_t size)
{
    CastTimeline *t=calloc(1,sizeof *t);if(!t){snprintf(error,size,"live media allocation failed");return NULL;}
    timeline_init(t);t->media[0]=job->media;t->media[0].id=1;t->media_count=1;
    t->clips[0]=(CastTimelineClip){.id=2,.media_id=1,.source_in_ns=job->source_in,
        .duration_ns=job->duration,.gain=1};t->clip_count=1;t->next_id=3;
    CastTimelineRenderer *r=timeline_renderer_open(t,&job->config,cancelled,s,error,size);free(t);return r;
}
static void run(CastLiveMedia *s,Job *job,Frame *scratch)
{
    char error[1024]="";
    CastTimelineRenderer *r=open_renderer(s,job,error,sizeof error);
    if(!r){failure(s,error);return;}
    bool video=job->media.kind!=CAST_TIMELINE_AUDIO,audio=false;
    if(job->media.kind!=CAST_TIMELINE_IMAGE && timeline_renderer_audio_present(r,1,&audio,error,sizeof error)){
        failure(s,error);timeline_renderer_close(r);return;
    }
    uint64_t length=samples(job->duration),cursor=0;
    unsigned prepared=audio?(length>4800?4800:(unsigned)length):0;
    float initial[9600];
    if ((video&&timeline_renderer_frame(r,0,scratch,error,sizeof error)) ||
        (prepared&&timeline_renderer_audio(r,0,prepared,initial,error,sizeof error))) {
        failure(s,error);timeline_renderer_close(r);return;
    }
    uint64_t ready=s->now(s->clock_user);
    if(job->anchor<ready)job->anchor=ready;
    uint64_t base=samples(job->anchor);
    uint64_t interval=UINT64_C(1000000000)/(unsigned)job->config.fps;
    uint64_t next_video=interval;
    pthread_mutex_lock(&s->mutex);
    if(!cancelled(s)){
        s->clock_anchor=job->anchor;
        s->status.has_video=video;s->status.has_audio=audio;s->status.state=CAST_LIVE_PLAYING;
        if(video){Frame old=s->frame;s->frame=*scratch;*scratch=old;s->frame_valid=true;
            s->status.video_source_ns=job->source_in;s->status.video_monotonic_ns=job->anchor;}
        if(prepared){
            for(unsigned i=0;i<prepared;++i)for(unsigned ch=0;ch<2;++ch){
                float value=initial[i*2+ch];if(!isfinite(value))value=0;
                s->pcm[((base+i)%CAST_LIVE_AUDIO_FRAMES)*2+ch]=fmaxf(-1,fminf(1,value));
            }
            s->pcm_begin=base;s->pcm_end=base+prepared;cursor=prepared;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    bool failed=false;
    while(!cancelled(s)){
        uint64_t now=s->now(s->clock_user),elapsed=now>job->anchor?now-job->anchor:0;
        uint64_t elapsed_samples=samples(elapsed);
        if(audio){
            if(elapsed_samples>cursor+CAST_LIVE_AUDIO_FRAMES){
                uint64_t next=elapsed_samples>4800?elapsed_samples-4800:0;
                pthread_mutex_lock(&s->mutex);
                if(!cancelled(s))s->status.dropped_audio_frames+=next-cursor;
                pthread_mutex_unlock(&s->mutex);cursor=next;
            }
            uint64_t goal=elapsed_samples+4800;if(goal>length)goal=length;
            while(cursor<goal&&!cancelled(s)){
                unsigned n=goal-cursor>480?480:(unsigned)(goal-cursor);float pcm[960];
                if(timeline_renderer_audio(r,cursor,n,pcm,error,sizeof error)){failed=true;break;}
                pthread_mutex_lock(&s->mutex);
                if(!cancelled(s)){
                    uint64_t first=base+cursor;
                    if(!s->pcm_end || first!=s->pcm_end)s->pcm_begin=first;
                    for(unsigned i=0;i<n;++i)for(unsigned ch=0;ch<2;++ch){
                        float value=pcm[i*2+ch];if(!isfinite(value))value=0;
                        s->pcm[((first+i)%CAST_LIVE_AUDIO_FRAMES)*2+ch]=fmaxf(-1,fminf(1,value));
                    }
                    s->pcm_end=first+n;
                    if(s->pcm_end-s->pcm_begin>CAST_LIVE_AUDIO_FRAMES)s->pcm_begin=s->pcm_end-CAST_LIVE_AUDIO_FRAMES;
                }
                pthread_mutex_unlock(&s->mutex);cursor+=n;
            }
        }
        if(failed)break;
        if(elapsed>=job->duration){
            pthread_mutex_lock(&s->mutex);
            if(!cancelled(s)){
                s->status.state=CAST_LIVE_ENDED;s->status.position_ns=job->source_in+job->duration;
                s->frame_valid=false; /* Keep timestamped PCM tail for recorder backlog. */
            }
            pthread_mutex_unlock(&s->mutex);break;
        }
        if(video&&now>=job->anchor&&elapsed>=next_video){
            uint64_t target=elapsed;
            if(timeline_renderer_frame(r,target,scratch,error,sizeof error)){failed=true;break;}
            uint64_t completed=s->now(s->clock_user);
            pthread_mutex_lock(&s->mutex);
            if(!cancelled(s)&&completed>=job->anchor+target&&completed-(job->anchor+target)<=UINT64_C(250000000)){
                Frame old=s->frame;s->frame=*scratch;*scratch=old;s->frame_valid=true;
                s->status.video_source_ns=job->source_in+target;s->status.video_monotonic_ns=job->anchor+target;
            }
            pthread_mutex_unlock(&s->mutex);
            next_video=target+interval;
        }
        if(failed)break;
        wait_tick(s);
    }
    if(failed)failure(s,error);
    timeline_renderer_close(r);
}
static void *worker(void *user)
{
    CastLiveMedia *s=user;Frame scratch={0};
    for(;;){
        pthread_mutex_lock(&s->mutex);
        while(!s->queued&&!atomic_load(&s->closing))pthread_cond_wait(&s->wake,&s->mutex);
        if(atomic_load(&s->closing)){pthread_mutex_unlock(&s->mutex);break;}
        *s->working=s->pending;s->queued=false;s->active_generation=s->working->generation;
        pthread_mutex_unlock(&s->mutex);run(s,s->working,&scratch);
    }
    frame_free(&scratch);return NULL;
}
CastLiveMedia *live_media_open_with_clock(uint64_t (*clock)(void *),void *user)
{
    CastLiveMedia *s=calloc(1,sizeof *s);if(!s)return NULL;
    s->working=malloc(sizeof *s->working);if(!s->working){free(s);return NULL;}
    s->now=clock?clock:normal_clock;s->clock_user=user;
    atomic_init(&s->closing,false);atomic_init(&s->generation,0);
    if(pthread_mutex_init(&s->mutex,NULL))goto failed;
    if(pthread_cond_init(&s->wake,NULL)){pthread_mutex_destroy(&s->mutex);goto failed;}
    if(pthread_create(&s->worker,NULL,worker,s)){
        pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);goto failed;
    }
    return s;
failed:free(s->working);free(s);return NULL;
}
CastLiveMedia *live_media_open(void){return live_media_open_with_clock(NULL,NULL);}
int live_media_play(CastLiveMedia *s,const CastTimelineMedia *media,const Config *config,
                    uint64_t in,uint64_t duration,uint64_t anchor,uint64_t generation)
{
    if(!s||!media||!config||!media->id||media->missing||!anchor||anchor>UINT64_MAX-duration||
       !memchr(media->path,0,sizeof media->path)||media->path[0]!='/'||
       media->kind<CAST_TIMELINE_VIDEO||media->kind>CAST_TIMELINE_IMAGE||!duration||
       duration>CAST_TIMELINE_LIMIT_NS||in>CAST_TIMELINE_LIMIT_NS-duration||
       (media->kind!=CAST_TIMELINE_IMAGE&&(!media->duration_ns||in>=media->duration_ns||duration>media->duration_ns-in)))return -1;
    pthread_mutex_lock(&s->mutex);
    if(!advance(s,generation)){pthread_mutex_unlock(&s->mutex);return -1;}
    s->selected.media=*media;s->selected.config=*config;s->selected.source_in=in;
    s->selected.duration=duration;s->selected.anchor=anchor;s->selected.generation=generation;
    s->pending=s->selected;s->has_selection=true;s->queued=true;s->frame_valid=false;
    s->clock_source=in;s->clock_anchor=anchor;s->source_end=in+duration;
    uint64_t epoch=s->status.audio_epoch;
    s->status=(CastLiveMediaStatus){.state=CAST_LIVE_STARTING,.generation=generation,.audio_epoch=epoch,
        .media_id=media->id,.source_in_ns=in,.position_ns=in,.duration_ns=duration};
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);return 0;
}
int live_media_pause(CastLiveMedia *s,uint64_t now,uint64_t generation)
{
    if(!s)return -1;
    pthread_mutex_lock(&s->mutex);
    if((s->status.state!=CAST_LIVE_STARTING&&s->status.state!=CAST_LIVE_PLAYING)||!advance(s,generation)){
        pthread_mutex_unlock(&s->mutex);return -1;
    }
    if(s->status.state==CAST_LIVE_PLAYING)s->status.position_ns=position(s,now);
    s->status.state=CAST_LIVE_PAUSED;s->queued=false;
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);return 0;
}
int live_media_resume(CastLiveMedia *s,uint64_t anchor,uint64_t generation)
{
    if(!s||!anchor)return -1;
    pthread_mutex_lock(&s->mutex);
    uint64_t in=s->status.position_ns;
    if(s->status.state!=CAST_LIVE_PAUSED||!s->has_selection||in>=s->source_end||
       anchor>UINT64_MAX-(s->source_end-in)||!advance(s,generation)){
        pthread_mutex_unlock(&s->mutex);return -1;
    }
    s->pending=s->selected;s->pending.source_in=in;s->pending.duration=s->source_end-in;
    s->pending.anchor=anchor;s->pending.generation=generation;s->clock_source=in;s->clock_anchor=anchor;
    s->queued=true;s->status.state=CAST_LIVE_STARTING;
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);return 0;
}
int live_media_stop(CastLiveMedia *s,uint64_t generation)
{
    if(!s)return -1;
    pthread_mutex_lock(&s->mutex);
    if(!advance(s,generation)){pthread_mutex_unlock(&s->mutex);return -1;}
    s->queued=s->has_selection=s->frame_valid=false;
    uint64_t epoch=s->status.audio_epoch;
    s->status=(CastLiveMediaStatus){.state=CAST_LIVE_IDLE,.generation=generation,.audio_epoch=epoch};
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);return 0;
}
void live_media_status(CastLiveMedia *s,uint64_t now,CastLiveMediaStatus *out)
{
    if(!out)return;
    if(!s){memset(out,0,sizeof *out);return;}
    pthread_mutex_lock(&s->mutex);*out=s->status;
    if(out->state==CAST_LIVE_PLAYING)out->position_ns=position(s,now);
    pthread_mutex_unlock(&s->mutex);
}
int live_media_video(CastLiveMedia *s,uint64_t now,Frame *out)
{
    if(!s||!out||!out->data||pthread_mutex_trylock(&s->mutex))return 1;
    bool paused=s->status.state==CAST_LIVE_PAUSED;
    bool playing=s->status.state==CAST_LIVE_PLAYING;
    if(!s->frame_valid||(!paused&&!playing)||(!paused&&(now<s->status.video_monotonic_ns||
        now-s->status.video_monotonic_ns>UINT64_C(250000000)))||out->width!=s->frame.width||
        out->height!=s->frame.height||out->stride<out->width*4){pthread_mutex_unlock(&s->mutex);return 1;}
    for(int y=0;y<out->height;++y)memcpy(out->data+(size_t)y*out->stride,s->frame.data+(size_t)y*s->frame.stride,(size_t)out->width*4);
    pthread_mutex_unlock(&s->mutex);return 0;
}
int live_media_audio_mix(CastLiveMedia *s,uint64_t start,float *out,unsigned frames)
{
    if(!s||!out||!frames||frames>CAST_LIVE_AUDIO_RATE||pthread_mutex_trylock(&s->mutex))return 1;
    if(!s->status.has_audio||(s->status.state!=CAST_LIVE_PLAYING&&s->status.state!=CAST_LIVE_ENDED)){
        pthread_mutex_unlock(&s->mutex);return 1;
    }
    uint64_t first=samples(start);bool complete=true;
    for(unsigned i=0;i<frames;++i){uint64_t at=first+i;
        if(at<s->pcm_begin||at>=s->pcm_end){complete=false;continue;}
        for(unsigned ch=0;ch<2;++ch){float base=out[i*2+ch];if(!isfinite(base))base=0;
            out[i*2+ch]=fmaxf(-1,fminf(1,base+s->pcm[(at%CAST_LIVE_AUDIO_FRAMES)*2+ch]));}
    }
    pthread_mutex_unlock(&s->mutex);return complete?0:1;
}
void live_media_close(CastLiveMedia *s)
{
    if(!s)return;
    pthread_mutex_lock(&s->mutex);atomic_store(&s->closing,true);
    s->frame_valid=false;audio_barrier(s);pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->mutex);
    pthread_join(s->worker,NULL);frame_free(&s->frame);pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->mutex);
    free(s->working);free(s);
}
