#define _POSIX_C_SOURCE 200809L
#include "timeline_assets.h"
#include <assert.h>
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void le(FILE *f,unsigned v,unsigned n){while(n--){fputc(v&255,f);v>>=8;}}
static void wave(const char *path)
{
    FILE *f=fopen(path,"wb");assert(f);fwrite("RIFF",1,4,f);le(f,36+48000*4,4);
    fwrite("WAVEfmt ",1,8,f);le(f,16,4);le(f,1,2);le(f,2,2);le(f,48000,4);le(f,192000,4);
    le(f,4,2);le(f,16,2);fwrite("data",1,4,f);le(f,48000*4,4);
    for(unsigned i=0;i<48000;++i){le(f,i<24000?8192:16384,2);le(f,i<24000?4096:0,2);}assert(!fclose(f));
}
static void image(const char *path)
{
    AVFormatContext *format=NULL;
    assert(!avformat_alloc_output_context2(&format,NULL,"matroska",path));
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_FFV1);assert(codec);
    AVCodecContext *c=avcodec_alloc_context3(codec);assert(c);
    c->width=16;c->height=16;c->pix_fmt=AV_PIX_FMT_YUV420P;c->time_base=(AVRational){1,30};
    if(format->oformat->flags&AVFMT_GLOBALHEADER)c->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    assert(!avcodec_open2(c,codec,NULL));
    AVStream *stream=avformat_new_stream(format,NULL);assert(stream);stream->time_base=c->time_base;
    assert(!avcodec_parameters_from_context(stream->codecpar,c));
    assert(!avio_open(&format->pb,path,AVIO_FLAG_WRITE));assert(!avformat_write_header(format,NULL));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();assert(frame&&packet);
    frame->width=16;frame->height=16;frame->format=c->pix_fmt;assert(!av_frame_get_buffer(frame,32));
    for(unsigned y=0;y<16;++y)memset(frame->data[0]+y*frame->linesize[0],81,16);
    for(unsigned y=0;y<8;++y){memset(frame->data[1]+y*frame->linesize[1],90,8);memset(frame->data[2]+y*frame->linesize[2],240,8);}
    frame->pts=0;assert(!avcodec_send_frame(c,frame));
    assert(!avcodec_receive_packet(c,packet));av_packet_rescale_ts(packet,c->time_base,stream->time_base);
    packet->stream_index=stream->index;assert(!av_interleaved_write_frame(format,packet));assert(!av_write_trailer(format));
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&c);assert(!avio_closep(&format->pb));avformat_free_context(format);
}
static void wait_result(CastTimelineAssets *s,uint64_t generation,uint64_t id,CastTimelineAssetResult *r)
{
    struct timespec delay={.tv_nsec=1000000};
    for(unsigned i=0;i<10000;++i){if(!timeline_assets_copy(s,generation,id,r))return;nanosleep(&delay,NULL);}
    assert(!"asset worker timed out");
}
int main(void)
{
    const char *tmp=getenv("TMPDIR");char dir[4096],wav[4096],bmp[4096];
    assert(snprintf(dir,sizeof dir,"%s/cast-assets-XXXXXX",tmp?tmp:"/tmp")<(int)sizeof dir);
    assert(mkdtemp(dir));assert(snprintf(wav,sizeof wav,"%s/a.wav",dir)<(int)sizeof wav);
    assert(snprintf(bmp,sizeof bmp,"%s/a.mkv",dir)<(int)sizeof bmp);wave(wav);image(bmp);
    CastTimelineAssets *s=timeline_assets_open();assert(s);
    CastTimelineAssetStatus status;timeline_assets_status(s,&status);assert(!status.pending&&!status.cached&&!status.busy);
    CastTimelineAssetRequest q={.media={.id=1,.kind=CAST_TIMELINE_AUDIO,.duration_ns=1000000000},
        .generation=1,.span_ns=1000000000,.waveform=true};strcpy(q.media.path,wav);
    uint64_t id,duplicate;assert(!timeline_assets_request(s,&q,&id));assert(!timeline_assets_request(s,&q,&duplicate)&&id==duplicate);
    CastTimelineAssetResult *r=malloc(sizeof *r);assert(r);wait_result(s,1,id,r);
    assert(r->waveform_ready&&r->bins==256&&!r->thumbnail_ready);
    assert(fabsf(r->peak[0][0]-.25f)<.001f&&fabsf(r->rms[0][1]-.125f)<.001f);
    assert(fabsf(r->peak[255][0]-.5f)<.001f&&r->rms[255][1]==0);
    q.source_start_ns=500000000;q.span_ns=250000000;
    assert(!timeline_assets_request(s,&q,&id));wait_result(s,1,id,r);
    assert(r->source_start_ns==500000000&&r->span_ns==250000000);
    assert(fabsf(r->rms[0][0]-.5f)<.001f&&r->peak[0][1]==0);
    q.media.kind=CAST_TIMELINE_VIDEO;strcpy(q.media.path,bmp);q.thumbnail=true;q.waveform=true;
    assert(!timeline_assets_request(s,&q,&id));wait_result(s,1,id,r);assert(r->thumbnail_ready);
    assert(!r->waveform_ready&&strstr(r->waveform_error,"source has no audio stream"));
    unsigned center=(45*160+80)*4;assert(r->rgba[center]>240&&r->rgba[center+1]<10&&r->rgba[center+2]<10);
    strcpy(q.media.path,"/cast-assets-definitely-missing.mkv");assert(!timeline_assets_request(s,&q,&id));wait_result(s,1,id,r);
    assert(!r->thumbnail_ready&&strstr(r->thumbnail_error,"cannot open local source"));
    q.media.kind=CAST_TIMELINE_AUDIO;strcpy(q.media.path,wav);q.thumbnail=false;q.waveform=true;
    q.source_start_ns=0;q.span_ns=1000000000;q.generation=2;
    assert(!timeline_assets_request(s,&q,&id));timeline_assets_cancel(s,3);
    assert(timeline_assets_copy(s,2,id,r));assert(timeline_assets_request(s,&q,&id)==-1);
    q.generation=3;assert(!timeline_assets_request(s,&q,&id));wait_result(s,3,id,r);assert(r->waveform_ready);
    uint64_t evicted=id;
    for(unsigned i=0;i<CAST_ASSET_CACHE+2;++i){
        q.source_start_ns=(i+1)*1000000;q.span_ns=100000000;
        assert(!timeline_assets_request(s,&q,&id));wait_result(s,3,id,r);assert(r->waveform_ready);
    }
    assert(timeline_assets_copy(s,3,evicted,r));
    q.span_ns=CAST_ASSET_SPAN_MAX_NS+1;assert(timeline_assets_request(s,&q,&id)==-1);
    timeline_assets_status(s,&status);assert(status.cached<=CAST_ASSET_CACHE&&status.pending<=CAST_ASSET_QUEUE);
    q.generation=4;q.source_start_ns=0;q.span_ns=CAST_ASSET_SPAN_MAX_NS;
    q.media.duration_ns=CAST_ASSET_SPAN_MAX_NS;
    uint64_t burst[64];unsigned accepted=0;
    for(unsigned i=0;i<64;++i){
        q.media.id=i+10;int result=timeline_assets_request(s,&q,&id);
        assert(result==0||result==1);if(!result)burst[accepted++]=id;
        timeline_assets_status(s,&status);assert(status.pending<=CAST_ASSET_QUEUE&&status.cached<=CAST_ASSET_CACHE);
    }
    assert(accepted);timeline_assets_cancel(s,5);
    for(unsigned i=0;i<accepted;++i)assert(timeline_assets_copy(s,4,burst[i],r));
    timeline_assets_close(s);free(r);assert(!unlink(wav)&&!unlink(bmp)&&!rmdir(dir));
    puts("timeline assets passed: real decoded thumbnail and stereo RMS/peak, visible offset/span, deduplication, missing file, generation cancellation, bounded requests; no devices or disk cache");
    return 0;
}
