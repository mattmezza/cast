#define _POSIX_C_SOURCE 200809L
#include "live_media.h"
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static atomic_uint_fast64_t now;
static atomic_bool hold_clock, clock_entered;
static uint64_t clock_value(void *unused)
{
    (void)unused;
    if(atomic_load(&hold_clock)){
        atomic_store(&clock_entered,true);
        struct timespec t={.tv_nsec=1000000};
        while(atomic_load(&hold_clock))nanosleep(&t,NULL);
    }
    return atomic_load(&now);
}
static void delay(void){struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);}
static void le(FILE *f,unsigned v,unsigned n){while(n--){fputc(v&255,f);v>>=8;}}
static void wave(const char *path)
{
    FILE *f=fopen(path,"wb");assert(f);fwrite("RIFF",1,4,f);le(f,36+144000*4,4);
    fwrite("WAVEfmt ",1,8,f);le(f,16,4);le(f,1,2);le(f,2,2);le(f,48000,4);le(f,192000,4);
    le(f,4,2);le(f,16,2);fwrite("data",1,4,f);le(f,144000*4,4);
    for(unsigned i=0;i<144000;++i){le(f,i<24000?8192:16384,2);le(f,4096,2);}assert(!fclose(f));
}
static void movie(const char *path)
{
    AVFormatContext *format=NULL;assert(!avformat_alloc_output_context2(&format,NULL,"matroska",path));
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_FFV1);assert(codec);
    AVCodecContext *c=avcodec_alloc_context3(codec);assert(c);c->width=16;c->height=16;
    c->pix_fmt=AV_PIX_FMT_YUV420P;c->time_base=(AVRational){1,30};
    if(format->oformat->flags&AVFMT_GLOBALHEADER)c->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    assert(!avcodec_open2(c,codec,NULL));AVStream *stream=avformat_new_stream(format,NULL);assert(stream);
    stream->time_base=c->time_base;assert(!avcodec_parameters_from_context(stream->codecpar,c));
    assert(!avio_open(&format->pb,path,AVIO_FLAG_WRITE));assert(!avformat_write_header(format,NULL));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();assert(frame&&packet);
    frame->width=16;frame->height=16;frame->format=c->pix_fmt;assert(!av_frame_get_buffer(frame,32));
    for(unsigned i=0;i<30;++i){
        assert(!av_frame_make_writable(frame));
        for(unsigned y=0;y<16;++y)memset(frame->data[0]+y*frame->linesize[0],i<15?81:41,16);
        for(unsigned y=0;y<8;++y){memset(frame->data[1]+y*frame->linesize[1],i<15?90:240,8);memset(frame->data[2]+y*frame->linesize[2],i<15?240:110,8);}
        frame->pts=i;assert(!avcodec_send_frame(c,frame));assert(!avcodec_receive_packet(c,packet));
        av_packet_rescale_ts(packet,c->time_base,stream->time_base);packet->stream_index=stream->index;
        assert(!av_interleaved_write_frame(format,packet));
    }
    assert(!av_write_trailer(format));
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&c);assert(!avio_closep(&format->pb));avformat_free_context(format);
}
static CastLiveMediaStatus wait_state(CastLiveMedia *s,CastLiveMediaState wanted)
{
    CastLiveMediaStatus status;
    for(unsigned i=0;i<10000;++i){live_media_status(s,atomic_load(&now),&status);if(status.state==wanted)return status;
        if(status.state==CAST_LIVE_FAILED&&wanted!=CAST_LIVE_FAILED){fprintf(stderr,"live failed: %s\n",status.error);assert(0);}delay();}
    assert(!"live worker timeout");return status;
}
static void wait_audio(CastLiveMedia *s,uint64_t stamp,float expected)
{
    for(unsigned i=0;i<5000;++i){float mix[960]={0};if(!live_media_audio_mix(s,stamp,mix,480)){
        assert(fabsf(mix[0]-expected)<.001f&&fabsf(mix[1]-.125f)<.001f);return;}delay();}
    assert(!"live audio missing");
}
int main(void)
{
    CastLiveMediaStatus absent, zero={0};
    memset(&absent,0xa5,sizeof absent);
    live_media_status(NULL,123,&absent);
    assert(!memcmp(&absent,&zero,sizeof absent));
    assert(absent.state==CAST_LIVE_IDLE);

    const char *tmp=getenv("TMPDIR");char dir[4096],wav[4096],video[4096];
    assert(snprintf(dir,sizeof dir,"%s/cast-live-XXXXXX",tmp?tmp:"/tmp")<(int)sizeof dir);assert(mkdtemp(dir));
    assert(snprintf(wav,sizeof wav,"%s/input.wav",dir)<(int)sizeof wav);
    assert(snprintf(video,sizeof video,"%s/input.mkv",dir)<(int)sizeof video);wave(wav);movie(video);
    Config *config=malloc(sizeof *config);assert(config);config_defaults(config);config->width=160;config->height=90;
    atomic_init(&hold_clock,false);atomic_init(&clock_entered,false);
    atomic_init(&now,UINT64_C(1000000000));CastLiveMedia *s=live_media_open_with_clock(clock_value,NULL);assert(s);
    CastLiveMediaStatus status;live_media_status(s,atomic_load(&now),&status);assert(status.state==CAST_LIVE_IDLE);
    CastTimelineMedia media={.id=42,.kind=CAST_TIMELINE_AUDIO,.duration_ns=1000000000};strcpy(media.path,wav);
    assert(!live_media_play(s,&media,config,0,1000000000,atomic_load(&now)-100000000,1));
    status=wait_state(s,CAST_LIVE_PLAYING);assert(status.has_audio&&!status.has_video&&status.position_ns==0);
    wait_audio(s,atomic_load(&now),.25f);
    float clipped[960];for(unsigned i=0;i<960;++i)clipped[i]=.9f;
    assert(!live_media_audio_mix(s,atomic_load(&now),clipped,480));assert(clipped[0]==1&&clipped[1]==1);
    for(unsigned i=0;i<960;++i)clipped[i]=.3f;
    assert(live_media_audio_mix(s,900000000,clipped,480));assert(clipped[0]==.3f);
    atomic_store(&now,UINT64_C(1200000000));wait_audio(s,atomic_load(&now),.25f);
    assert(!live_media_pause(s,atomic_load(&now),2));status=wait_state(s,CAST_LIVE_PAUSED);
    assert(status.position_ns==200000000);uint64_t epoch=status.audio_epoch;
    float silent[960]={0};assert(live_media_audio_mix(s,1000000000,silent,480));assert(silent[0]==0);
    atomic_store(&now,UINT64_C(2000000000));assert(wait_state(s,CAST_LIVE_PAUSED).position_ns==200000000);
    assert(!live_media_resume(s,atomic_load(&now),3));wait_state(s,CAST_LIVE_PLAYING);
    wait_audio(s,2000000000,.25f);assert(live_media_audio_mix(s,1000000000,silent,480));
    atomic_store(&now,UINT64_C(2800000000));status=wait_state(s,CAST_LIVE_ENDED);
    assert(status.position_ns==1000000000&&status.audio_epoch>epoch);wait_audio(s,2700000000,.5f);
    assert(!live_media_stop(s,4));assert(live_media_audio_mix(s,2700000000,silent,480));
    assert(live_media_resume(s,atomic_load(&now),5)); /* privacy stop is not resumable */
    media.kind=CAST_TIMELINE_VIDEO;strcpy(media.path,video);atomic_store(&now,UINT64_C(3000000000));
    assert(!live_media_play(s,&media,config,0,1000000000,atomic_load(&now),5));
    status=wait_state(s,CAST_LIVE_PLAYING);assert(status.has_video&&!status.has_audio);
    Frame frame={0};assert(!frame_alloc(&frame,160,90));frame.ts_ns=12345;
    for(unsigned i=0;i<5000&&live_media_video(s,atomic_load(&now),&frame);++i)delay();
    unsigned center=(45*frame.stride+80*4);assert(frame.data[center]>240&&frame.data[center+1]<15&&frame.ts_ns==12345);
    assert(status.video_source_ns==0&&status.video_monotonic_ns==3000000000);
    atomic_store(&now,UINT64_C(3600000000));bool blue=false;
    for(unsigned i=0;i<5000;++i){
        if(!live_media_video(s,atomic_load(&now),&frame)&&frame.data[center+2]>230&&frame.data[center]<30){blue=true;break;}
        delay();
    }
    assert(blue);live_media_status(s,atomic_load(&now),&status);assert(status.video_source_ns==600000000);
    assert(!live_media_pause(s,atomic_load(&now),6));atomic_store(&now,UINT64_C(5000000000));
    assert(!live_media_video(s,atomic_load(&now),&frame));assert(!live_media_stop(s,7));assert(live_media_video(s,atomic_load(&now),&frame));
    strcpy(media.path,"/cast-live-definitely-missing.mkv");assert(!live_media_play(s,&media,config,0,1000000000,atomic_load(&now),8));
    assert(strstr(wait_state(s,CAST_LIVE_FAILED).error,"cannot open local source"));
    assert(live_media_play(s,&media,config,0,1000000000,atomic_load(&now),8));
    assert(!live_media_stop(s,9));
    media.kind=CAST_TIMELINE_AUDIO;strcpy(media.path,wav);atomic_store(&now,UINT64_C(6000000000));
    atomic_store(&hold_clock,true);atomic_store(&clock_entered,false);
    uint64_t started=cast_now_ns();
    assert(!live_media_play(s,&media,config,0,1000000000,atomic_load(&now),10));
    assert(cast_now_ns()-started<250000000);
    for(unsigned i=0;i<5000&&!atomic_load(&clock_entered);++i)delay();
    assert(atomic_load(&clock_entered));
    live_media_status(s,atomic_load(&now),&status);assert(status.state==CAST_LIVE_STARTING&&status.position_ns==0);
    started=cast_now_ns();assert(!live_media_stop(s,11));assert(cast_now_ns()-started<250000000);
    assert(live_media_audio_mix(s,6000000000,silent,480));assert(live_media_video(s,atomic_load(&now),&frame));
    atomic_store(&hold_clock,false);
    assert(!live_media_play(s,&media,config,500000000,250000000,atomic_load(&now),12));
    status=wait_state(s,CAST_LIVE_PLAYING);assert(status.position_ns==500000000&&status.duration_ns==250000000);
    wait_audio(s,6000000000,.5f);assert(!live_media_stop(s,13));
    media.duration_ns=3000000000;atomic_store(&now,UINT64_C(7000000000));
    assert(!live_media_play(s,&media,config,0,3000000000,atomic_load(&now),14));wait_state(s,CAST_LIVE_PLAYING);
    for(unsigned i=0;i<30;++i){
        atomic_store(&now,UINT64_C(7000000000)+(uint64_t)i*100000000);
        wait_audio(s,atomic_load(&now),i<5?.25f:.5f);
    }
    atomic_store(&now,UINT64_C(10000000000));wait_state(s,CAST_LIVE_ENDED);
    wait_audio(s,8100000000,.5f);wait_audio(s,9900000000,.5f);
    assert(live_media_audio_mix(s,7900000000,silent,480));
    assert(!live_media_stop(s,15));
    frame_free(&frame);live_media_close(s);free(config);
    assert(!unlink(wav)&&!unlink(video)&&!rmdir(dir));
    puts("live media passed: inert worker, decoded stereo PCM/video, additive timestamp mixing, explicit pause/resume, no privacy restart, ended PCM tail, stale generations and missing-file failure; no devices");
    return 0;
}
