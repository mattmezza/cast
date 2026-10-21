/* Intercept encoder lookup/open: exercise production policy without device IO. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <stdio.h>
#include <string.h>
static unsigned opened;
static bool succeed;
static const AVCodec *descriptor;
static const AVCodec *cpu_h264_descriptor(void)
{
    void *cursor=NULL;
    const AVCodec *codec;
    while ((codec=av_codec_iterate(&cursor))) {
        if (!av_codec_is_encoder(codec) || codec->id != AV_CODEC_ID_H264) continue;
        const enum AVPixelFormat *formats=NULL; int count=0;
        if (avcodec_get_supported_config(NULL,codec,AV_CODEC_CONFIG_PIX_FORMAT,0,
                                        (const void **)&formats,&count) < 0) continue;
        for (int i=0;i<count;++i) if (formats[i]==AV_PIX_FMT_YUV420P) return codec;
    }
    return NULL;
}
static const AVCodec *lookup(const char *name)
{
    assert(!strcmp(name, "h264_videotoolbox"));
    /* Borrow a real CPU descriptor for allocation/format discovery only. */
    return descriptor;
}
static int open_boundary(AVCodecContext *context, const AVCodec *codec, AVDictionary **options)
{
    (void)codec;
    assert(context->pix_fmt == AV_PIX_FMT_YUV420P);
    assert(context->bit_rate == 6000000);
    assert(context->max_b_frames == 0);
    assert(!strcmp(av_dict_get(*options, "allow_sw", NULL, 0)->value, "0"));
    assert(!strcmp(av_dict_get(*options, "require_sw", NULL, 0)->value, "0"));
    assert(!strcmp(av_dict_get(*options, "realtime", NULL, 0)->value, "1"));
    assert(!av_dict_get(*options, "crf", NULL, 0));
    assert(!av_dict_get(*options, "preset", NULL, 0));
    ++opened;
    /* Simulate unsupported hardware: explicit request must fail without fallback. */
    av_dict_free(options);
    return succeed ? 0 : AVERROR(ENOSYS);
}
#define avcodec_find_encoder_by_name lookup
#define avcodec_open2 open_boundary
#include "../src/media_codec.c"
int main(void)
{
    descriptor=cpu_h264_descriptor();
    if (!descriptor) {
        puts("SKIP VideoToolbox policy: no registered H.264 descriptor supporting CPU YUV420P; no device opened");
        return 0;
    }
    Config cfg = {.width=3840, .height=2160, .fps=30, .record_bitrate_kbps=6000};
    strcpy(cfg.video_codec,"h264_videotoolbox");
    strcpy(cfg.stream.video_encoder,"h264_videotoolbox");
    cfg.stream.video_bitrate_kbps=6000;
    AVCodecContext *context=NULL; MediaCodecSelection selection; char error[CAST_ERR];
    for (unsigned stream=0;stream<2;++stream) {
        strcpy(cfg.record_rate_control,"bitrate");
        assert(media_video_open(&cfg,stream,false,&context,&selection,error,sizeof error));
        assert(!context && strstr(error,"cannot initialize encoder 'h264_videotoolbox'"));
    }
    assert(opened==2);
    strcpy(cfg.record_rate_control,"crf");
    assert(media_video_open(&cfg,false,false,&context,&selection,error,sizeof error));
    assert(strstr(error,"does not support record.rate_control=crf"));
    assert(opened==2);
    succeed=true;
    strcpy(cfg.record_rate_control,"auto");
    assert(!media_video_open(&cfg,false,false,&context,&selection,error,sizeof error));
    assert(context && !strcmp(selection.encoder,"h264_videotoolbox"));
    assert(strstr(selection.detail,"hardware required"));
    assert(strstr(selection.detail,"CPU YUV420P upload"));
    avcodec_free_context(&context);
    puts("VideoToolbox policy passed: hardware required, realtime, CPU input, CRF rejection, explicit failure without fallback; no devices opened");
    return 0;
}
