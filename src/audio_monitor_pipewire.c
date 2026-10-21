#define _POSIX_C_SOURCE 200809L
#include "audio_monitor.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    CastMonitorRender render;
    void *context;
    enum pw_stream_state state;
    char error[256];
} NativeMonitor;
static pthread_once_t initialized=PTHREAD_ONCE_INIT;
static void initialize(void){pw_init(NULL,NULL);}
static void changed(void *data,enum pw_stream_state old,enum pw_stream_state state,const char *error)
{
    (void)old;NativeMonitor *n=data;n->state=state;
    snprintf(n->error,sizeof n->error,"%s",error?error:"");pw_thread_loop_signal(n->loop,false);
}
static void process(void *data)
{
    NativeMonitor *n=data;struct pw_buffer *buffer=pw_stream_dequeue_buffer(n->stream);if(!buffer)return;
    struct spa_buffer *b=buffer->buffer;
    if(b->n_datas&&b->datas[0].chunk)b->datas[0].chunk->size=0;
    if(b->n_datas&&b->datas[0].data&&b->datas[0].chunk){
        struct spa_data *d=&b->datas[0];unsigned frames=d->maxsize/(2*sizeof(float));
        if(buffer->requested&&frames>buffer->requested)frames=(unsigned)buffer->requested;
        if(frames>CAST_MONITOR_CAPACITY)frames=CAST_MONITOR_CAPACITY;
        n->render(n->context,d->data,frames);
        d->chunk->offset=0;d->chunk->stride=2*sizeof(float);d->chunk->size=frames*2*sizeof(float);
    }
    pw_stream_queue_buffer(n->stream,buffer);
}
static const struct pw_stream_events events={PW_VERSION_STREAM_EVENTS,.state_changed=changed,.process=process};
static void close_output(void *data)
{
    NativeMonitor *n=data;if(!n->loop)return;
    /* No RT_PROCESS flag: callbacks share this loop. The lock excludes the
     * callback, stream destruction removes it, and stop joins the loop. */
    pw_thread_loop_lock(n->loop);
    if(n->stream){pw_stream_destroy(n->stream);n->stream=NULL;}
    pw_thread_loop_unlock(n->loop);pw_thread_loop_stop(n->loop);pw_thread_loop_destroy(n->loop);n->loop=NULL;
    n->render=NULL;n->context=NULL;
}
static int open_output(void *data,CastMonitorRender render,void *context,char *error,size_t size)
{
    NativeMonitor *n=data;pthread_once(&initialized,initialize);n->render=render;n->context=context;n->state=PW_STREAM_STATE_UNCONNECTED;n->error[0]=0;
    n->loop=pw_thread_loop_new("cast-monitor",NULL);
    if(!n->loop||pw_thread_loop_start(n->loop)<0)goto failed;
    pw_thread_loop_lock(n->loop);
    struct pw_properties *properties=pw_properties_new(PW_KEY_MEDIA_TYPE,"Audio",PW_KEY_MEDIA_CATEGORY,"Playback",PW_KEY_MEDIA_ROLE,"Production",PW_KEY_NODE_NAME,"cast.explicit-monitor","node.dont-reconnect","true","node.dont-move","true",NULL);
    if(properties)n->stream=pw_stream_new_simple(pw_thread_loop_get_loop(n->loop),"Cast explicit monitoring",properties,&events,n);
    if(!n->stream){pw_thread_loop_unlock(n->loop);goto failed;}
    unsigned char storage[1024];struct spa_pod_builder builder=SPA_POD_BUILDER_INIT(storage,sizeof storage);
    struct spa_audio_info_raw format={.format=SPA_AUDIO_FORMAT_F32,.rate=CAST_MONITOR_RATE,.channels=2,.position={SPA_AUDIO_CHANNEL_FL,SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *params[]={spa_format_audio_raw_build(&builder,SPA_PARAM_EnumFormat,&format)};
    int result=pw_stream_connect(n->stream,PW_DIRECTION_OUTPUT,PW_ID_ANY,PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS,params,1);
    /* A bounded initial handshake; no background retry or device fallback. */
    struct timespec deadline;pw_thread_loop_get_time(n->loop,&deadline,INT64_C(2000000000));
    while(result>=0&&n->state!=PW_STREAM_STATE_STREAMING&&n->state!=PW_STREAM_STATE_ERROR){
        if(pw_thread_loop_timed_wait_full(n->loop,&deadline)<0)break;
    }
    bool ready=result>=0&&n->state==PW_STREAM_STATE_STREAMING;
    pw_thread_loop_unlock(n->loop);
    if(ready)return 0;
failed:
    close_output(n);
    if(error&&size)snprintf(error,size,"PipeWire monitoring unavailable: %s",n->error[0]?n->error:"output connection failed");
    return -1;
}
static int health(void *data,char *error,size_t size)
{
    NativeMonitor *n=data;if(!n->loop)return -1;
    pw_thread_loop_lock(n->loop);bool ready=n->state==PW_STREAM_STATE_STREAMING;
    if(!ready&&error&&size)snprintf(error,size,"PipeWire monitoring stopped: %s",n->error[0]?n->error:"output disconnected");
    pw_thread_loop_unlock(n->loop);return ready?0:-1;
}
static void destroy(void *data){close_output(data);free(data);}
CastAudioMonitor *audio_monitor_native_create(void)
{
    NativeMonitor *n=calloc(1,sizeof *n);if(!n)return NULL;
    CastMonitorBackend ops={.open=open_output,.close=close_output,.health=health,.destroy=destroy};
    CastAudioMonitor *m=audio_monitor_create(&ops,n);if(!m)free(n);return m;
}
