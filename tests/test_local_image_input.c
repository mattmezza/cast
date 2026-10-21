#define _POSIX_C_SOURCE 200809L
#include "local_image_input.h"
#include "timeline_render.h"
#include "timeline_assets.h"
#include <time.h>
#include <assert.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void png(const char *path)
{
    static const unsigned char bytes[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,4,8,2,0,0,0,38,147,9,41,0,0,0,16,73,68,65,84,120,156,99,248,207,192,0,71,12,196,113,0,174,147,15,241,208,95,35,158,0,0,0,0,73,69,78,68,174,66,96,130};
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(!fclose(f));
}
static void jpeg(const char *path)
{
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MJPEG);assert(codec);
    AVCodecContext *c=avcodec_alloc_context3(codec);assert(c);c->width=16;c->height=16;
    c->pix_fmt=AV_PIX_FMT_YUVJ420P;c->time_base=(AVRational){1,25};assert(!avcodec_open2(c,codec,NULL));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();assert(frame&&packet);
    frame->width=16;frame->height=16;frame->format=c->pix_fmt;assert(!av_frame_get_buffer(frame,32));
    for(unsigned y=0;y<16;++y)memset(frame->data[0]+y*frame->linesize[0],76,16);
    for(unsigned y=0;y<8;++y){memset(frame->data[1]+y*frame->linesize[1],85,8);memset(frame->data[2]+y*frame->linesize[2],255,8);}
    assert(!avcodec_send_frame(c,frame));assert(!avcodec_receive_packet(c,packet));
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(packet->data,1,packet->size,f)==(size_t)packet->size);assert(!fclose(f));
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&c);
}
static CastTimelineRenderer *renderer(const char *path,uint64_t duration,char *error,size_t size)
{
    CastTimeline *t=calloc(1,sizeof *t);Config *c=malloc(sizeof *c);assert(t&&c);
    timeline_init(t);config_defaults(c);c->width=160;c->height=90;uint64_t id;
    assert(!timeline_add_media(t,path,CAST_TIMELINE_IMAGE,duration,&id,error,size));
    assert(!timeline_add_clip(t,id,0,0,0,duration,&id,error,size));
    CastTimelineRenderer *r=timeline_renderer_open(t,c,NULL,NULL,error,size);free(t);free(c);return r;
}
int main(void)
{
    const char *tmp=getenv("TMPDIR");char dir[4096],p[4096],j[4096],link[4096],fifo[4096];
    assert(snprintf(dir,sizeof dir,"%s/cast-images-XXXXXX",tmp?tmp:"/tmp")<(int)sizeof dir);assert(mkdtemp(dir));
    assert(snprintf(p,sizeof p,"%s/literal%%03d.wrong",dir)<(int)sizeof p);
    assert(snprintf(j,sizeof j,"%s/photo.wrong",dir)<(int)sizeof j);
    assert(snprintf(link,sizeof link,"%s/link.png",dir)<(int)sizeof link);
    assert(snprintf(fifo,sizeof fifo,"%s/pipe.jpg",dir)<(int)sizeof fifo);
    png(p);jpeg(j);char error[1024];
    for(unsigned i=0;i<2;++i){
        const char *path=i?j:p;int fd=open(path,O_RDONLY|O_CLOEXEC);assert(fd>=0);
        assert(lseek(fd,2,SEEK_SET)==2);AVFormatContext *format=avformat_alloc_context();assert(format);
        const AVInputFormat *input=NULL;assert(local_image_input(fd,format,&input,error,sizeof error)==1);
        assert(input&&!strcmp(input->name,"image2pipe"));assert(format->video_codec_id==(i?AV_CODEC_ID_MJPEG:AV_CODEC_ID_PNG));
        assert(lseek(fd,0,SEEK_CUR)==2);assert(!close(fd));avformat_free_context(format);
        CastTimelineRenderer *r=renderer(path,5000000000,error,sizeof error);assert(r);Frame frame={0};
        assert(!timeline_renderer_frame(r,0,&frame,error,sizeof error));unsigned center=(45*frame.stride+80*4);
        assert(frame.data[center]>230&&frame.data[center+1]<30&&frame.data[center+2]<30);
        assert(!timeline_renderer_frame(r,4000000000,&frame,error,sizeof error));
        frame_free(&frame);timeline_renderer_close(r);
    }
    CastTimelineAssets *assets=timeline_assets_open();assert(assets);
    CastTimelineAssetRequest request={.media={.id=1,.kind=CAST_TIMELINE_IMAGE},.generation=1,
        .span_ns=5000000000,.thumbnail=true};strcpy(request.media.path,p);uint64_t asset_id;
    assert(!timeline_assets_request(assets,&request,&asset_id));
    CastTimelineAssetResult *result=malloc(sizeof *result);assert(result);
    struct timespec delay={.tv_nsec=1000000};bool ready=false;
    for(unsigned i=0;i<10000;++i){if(!timeline_assets_copy(assets,1,asset_id,result)){ready=true;break;}nanosleep(&delay,NULL);}
    assert(ready&&result->thumbnail_ready);free(result);timeline_assets_close(assets);
    CastTimeline *invalid=calloc(1,sizeof *invalid);assert(invalid);timeline_init(invalid);uint64_t invalid_id;
    assert(timeline_add_media(invalid,p,CAST_TIMELINE_IMAGE,CAST_TIMELINE_LIMIT_NS+1,&invalid_id,error,sizeof error));free(invalid);
    assert(!symlink(p,link));assert(!renderer(link,5000000000,error,sizeof error));
    assert(!mkfifo(fifo,0600));assert(!renderer(fifo,5000000000,error,sizeof error));
    int fd=open(fifo,O_RDONLY|O_NONBLOCK);assert(fd>=0);AVFormatContext *format=avformat_alloc_context();const AVInputFormat *input=NULL;
    assert(local_image_input(fd,format,&input,error,sizeof error)==-1);assert(!close(fd));avformat_free_context(format);
    static const unsigned char huge_png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,255,255,0,0,255,255,8,2,0,0,0,57,103,78,7,0,0,0,16,73,68,65,84,120,156,99,248,207,192,0,71,12,196,113,0,174,147,15,241,208,95,35,158,0,0,0,0,73,69,78,68,174,66,96,130};
    FILE *huge_file=fopen(p,"wb");assert(huge_file);
    assert(fwrite(huge_png,1,sizeof huge_png,huge_file)==sizeof huge_png);assert(!fclose(huge_file));
    CastTimelineRenderer *huge_renderer=renderer(p,5000000000,error,sizeof error);assert(huge_renderer);
    Frame rejected={0};assert(timeline_renderer_frame(huge_renderer,0,&rejected,error,sizeof error));
    frame_free(&rejected);timeline_renderer_close(huge_renderer);
    fd=open(p,O_WRONLY);assert(fd>=0);assert(!ftruncate(fd,CAST_LOCAL_IMAGE_BYTES_MAX+1));assert(!close(fd));
    fd=open(p,O_RDONLY);assert(fd>=0);format=avformat_alloc_context();assert(local_image_input(fd,format,&input,error,sizeof error)==-1);
    assert(strstr(error,"64 MiB"));assert(!close(fd));avformat_free_context(format);
    assert(!unlink(p)&&!unlink(j)&&!unlink(link)&&!unlink(fifo)&&!rmdir(dir));
    puts("safe still images passed: actual PNG/JPEG decode, magic over extensions, literal percent path, held image duration, unchanged FD position, symlink/FIFO and encoded size rejection");
    return 0;
}
