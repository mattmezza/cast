/* Explicit default-output playback only. No mic or desktop capture is opened.
 * AVAudioSourceNode exists since10.15; Cast's supported deployment is macOS13+. */
#include "audio_monitor.h"
#import <AVFoundation/AVFoundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
@interface CastMonitorCallbackToken : NSObject {
@public
    _Atomic(void *) owner;
    atomic_uint in_flight;
}
@end
@implementation CastMonitorCallbackToken
@end
typedef struct {
    AVAudioEngine *engine;
    AVAudioSourceNode *source;
    CastMonitorCallbackToken *token;
    CastMonitorRender render;
    void *context;
    float scratch[CAST_MONITOR_CAPACITY*2];
} NativeMonitor;
static void close_output(void *data)
{
    NativeMonitor *n=data;CastMonitorCallbackToken *token=n->token;
    if(token)atomic_store_explicit(&token->owner,NULL,memory_order_seq_cst);
    [n->engine stop];
    if(n->source){[n->engine disconnectNodeOutput:n->source];[n->engine detachNode:n->source];}
    /* Callback increments its token before loading owner. A callback entering
     * after this point sees NULL; an earlier callback is joined before free.
     * The block retains its own token, so even a late native invocation is safe. */
    if(token)while(atomic_load_explicit(&token->in_flight,memory_order_seq_cst)){
        struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);
    }
    n->source=nil;n->engine=nil;n->token=nil;n->render=NULL;n->context=NULL;
    memset(n->scratch,0,sizeof n->scratch);
}
static int open_output(void *data,CastMonitorRender render,void *context,char *error,size_t size)
{
    NativeMonitor *n=data;n->render=render;n->context=context;
    n->engine=[AVAudioEngine new];n->token=[CastMonitorCallbackToken new];
    CastMonitorCallbackToken *token=n->token;
    atomic_init(&token->owner,n);atomic_init(&token->in_flight,0);
    AVAudioFormat *format=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:CAST_MONITOR_RATE channels:2];
    n->source=[[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL *silent,const AudioTimeStamp *timestamp,AVAudioFrameCount count,AudioBufferList *buffers){
        (void)timestamp;
        for(UInt32 i=0;i<buffers->mNumberBuffers;i++)if(buffers->mBuffers[i].mData)memset(buffers->mBuffers[i].mData,0,buffers->mBuffers[i].mDataByteSize);
        atomic_fetch_add_explicit(&token->in_flight,1,memory_order_seq_cst);
        NativeMonitor *current=atomic_load_explicit(&token->owner,memory_order_seq_cst);
        bool rendered=false;
        if(current&&buffers->mNumberBuffers==2&&buffers->mBuffers[0].mData&&buffers->mBuffers[1].mData){
            unsigned available=buffers->mBuffers[0].mDataByteSize/sizeof(float);
            if(available>buffers->mBuffers[1].mDataByteSize/sizeof(float))available=buffers->mBuffers[1].mDataByteSize/sizeof(float);
            if(available>count)available=count;
            for(unsigned offset=0;offset<available;){
                unsigned frames=available-offset;if(frames>CAST_MONITOR_CAPACITY)frames=CAST_MONITOR_CAPACITY;
                current->render(current->context,current->scratch,frames);
                for(unsigned i=0;i<frames;i++)for(unsigned channel=0;channel<2;channel++)((float *)buffers->mBuffers[channel].mData)[offset+i]=current->scratch[i*2+channel];
                offset+=frames;
            }
            rendered=true;
        }
        if(silent)*silent=!rendered;
        atomic_fetch_sub_explicit(&token->in_flight,1,memory_order_seq_cst);return noErr;
    }];
    if(!n->engine||!n->source){if(error&&size)snprintf(error,size,"Audio monitoring allocation failed");close_output(n);return -1;}
    [n->engine attachNode:n->source];[n->engine connect:n->source to:n->engine.mainMixerNode format:format];
    NSError *failure=nil;
    if(![n->engine startAndReturnError:&failure]){
        if(error&&size)snprintf(error,size,"Audio monitoring unavailable: %s",failure.localizedDescription.UTF8String?:"output start failed");
        close_output(n);return -1;
    }
    return 0;
}
static int health(void *data,char *error,size_t size)
{
    NativeMonitor *n=data;if(n->engine.running)return 0;
    if(error&&size)snprintf(error,size,"Audio monitoring output stopped");return -1;
}
static void destroy(void *data){close_output(data);free(data);}
CastAudioMonitor *audio_monitor_native_create(void)
{
    NativeMonitor *n=calloc(1,sizeof *n);if(!n)return NULL;
    CastMonitorBackend ops={.open=open_output,.close=close_output,.health=health,.destroy=destroy};
    CastAudioMonitor *m=audio_monitor_create(&ops,n);if(!m)free(n);return m;
}
