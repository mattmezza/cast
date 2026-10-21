/* Deterministic, in-memory encoder policy acceptance. No capture or network.
 * Measure both easy and deliberately impossible bitrate content; never infer
 * a hard bitrate guarantee from AVCodecContext.bit_rate alone. */
#include "media_codec.h"
#include <assert.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>
#define FRAMES 300
static double encode(bool noise, bool stream)
{
    Config cfg={.width=640,.height=360,.fps=30,.record_bitrate_kbps=500};
    strcpy(cfg.video_codec,"libopenh264");strcpy(cfg.record_rate_control,"bitrate");
    strcpy(cfg.stream.video_encoder,"libopenh264");cfg.stream.video_bitrate_kbps=500;
    AVCodecContext *encoder=NULL;MediaCodecSelection selection;char error[CAST_ERR];
    assert(!media_video_open(&cfg,stream,false,&encoder,&selection,error,sizeof error));
    int64_t profile=-1,skip=-1,rc=-1;
    assert(!av_opt_get_int(encoder->priv_data,"profile",0,&profile));
    assert(!av_opt_get_int(encoder->priv_data,"allow_skip_frames",0,&skip));
    assert(!av_opt_get_int(encoder->priv_data,"rc_mode",0,&rc));
    assert(profile==AV_PROFILE_H264_HIGH&&skip==0&&rc==1);
    assert(strstr(selection.detail,"bitrate may vary"));
    AVCodecContext *decoder=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    assert(decoder&&!avcodec_open2(decoder,decoder->codec,NULL));
    AVFrame *input=av_frame_alloc(),*decoded=av_frame_alloc();AVPacket *packet=av_packet_alloc();
    assert(input&&decoded&&packet);input->width=640;input->height=360;input->format=AV_PIX_FMT_YUV420P;
    assert(!av_frame_get_buffer(input,32));
    unsigned random=1,packets=0,frames=0;uint64_t bytes=0;int64_t previous=-1;
    for(unsigned i=0;i<=FRAMES;i++){
        if(i<FRAMES){
            assert(!av_frame_make_writable(input));
            for(int y=0;y<360;y++)for(int x=0;x<640;x++){
                random=random*1664525u+1013904223u;
                input->data[0][y*input->linesize[0]+x]=noise?(random>>24):128;
            }
            for(unsigned p=1;p<3;p++)for(int y=0;y<180;y++)memset(input->data[p]+y*input->linesize[p],128,320);
            input->pts=av_rescale_q(i,(AVRational){1,30},encoder->time_base);
        }
        assert(!avcodec_send_frame(encoder,i<FRAMES?input:NULL));
        int result;
        while((result=avcodec_receive_packet(encoder,packet))==0){
            assert(packet->pts>previous);previous=packet->pts;bytes+=packet->size;packets++;
            assert(!avcodec_send_packet(decoder,packet));av_packet_unref(packet);
            while((result=avcodec_receive_frame(decoder,decoded))==0){
                assert(decoded->width==640&&decoded->height==360&&!decoded->decode_error_flags);
                assert(decoder->profile==AV_PROFILE_H264_HIGH);frames++;av_frame_unref(decoded);
            }
            assert(result==AVERROR(EAGAIN));
        }
        assert(result==AVERROR(EAGAIN)||result==AVERROR_EOF);
    }
    assert(!avcodec_send_packet(decoder,NULL));
    while(avcodec_receive_frame(decoder,decoded)==0){frames++;av_frame_unref(decoded);}
    assert(packets==FRAMES&&frames==FRAMES);
    double kbps=bytes*8.0/(FRAMES/30.0)/1000;
    printf("OpenH264 %s %s: %u submitted/packets/decoded; target500kbps actual%.1fkbps\n",stream?"stream":"record",noise?"noise":"silence",frames,kbps);
    av_frame_free(&input);av_frame_free(&decoded);av_packet_free(&packet);avcodec_free_context(&encoder);avcodec_free_context(&decoder);
    return kbps;
}
static void colorspace(void)
{
    /* The diagnostic 'No accelerated colorspace conversion' selects a CPU
     * implementation; verify the actual YUV422P -> RGBA output, not the log. */
    AVFrame *in=av_frame_alloc(),*out=av_frame_alloc();assert(in&&out);
    in->width=out->width=64;in->height=out->height=32;
    in->format=AV_PIX_FMT_YUV422P;out->format=AV_PIX_FMT_RGBA;
    assert(!av_frame_get_buffer(in,32)&&!av_frame_get_buffer(out,32));
    for(int y=0;y<32;y++){
        memset(in->data[0]+y*in->linesize[0],126,64);
        for(int p=1;p<3;p++)memset(in->data[p]+y*in->linesize[p],128,32);
    }
    struct SwsContext *scale=sws_getContext(64,32,in->format,64,32,out->format,SWS_FAST_BILINEAR,NULL,NULL,NULL);assert(scale);
    assert(sws_scale(scale,(const unsigned char *const *)in->data,in->linesize,0,32,out->data,out->linesize)==32);
    for(int y=0;y<32;y++)for(int x=0;x<64;x++){
        unsigned char *pixel=out->data[0]+y*out->linesize[0]+x*4;
        assert(pixel[0]>=125&&pixel[0]<=130&&pixel[1]>=125&&pixel[1]<=130&&pixel[2]>=125&&pixel[2]<=130&&pixel[3]==255);
    }
    sws_freeContext(scale);av_frame_free(&in);av_frame_free(&out);
    puts("YUV422P -> RGBA CPU fallback: correct complete output");
}
int main(void)
{
    colorspace();double easy=encode(false,false),hard=encode(true,false);
    assert(easy<500&&hard>500); /* Best-effort target, never a hard-cap claim. */
    encode(true,true);puts("encoder policy PASS: High profile, bitrate mode, all frames retained");
}
