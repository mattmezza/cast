#include "local_image_input.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail(char *error,size_t size,const char *message)
{if(error&&size)snprintf(error,size,"%s",message);return -1;}
int local_image_input(int fd,AVFormatContext *format,const AVInputFormat **input,char *error,size_t size)
{
    if(error&&size)*error=0;
    if(!format||!input)return fail(error,size,"invalid local image input request");
    *input=NULL;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0)
        return fail(error,size,"image input requires a nonempty local regular file");
    unsigned char header[8];ssize_t count;
    do {count=pread(fd,header,sizeof header,0);} while(count<0&&errno==EINTR);
    if(count<0)return fail(error,size,"cannot read local image signature");
    enum AVCodecID codec=AV_CODEC_ID_NONE;
    if(count==8&&!memcmp(header,"\x89PNG\r\n\x1a\n",8))codec=AV_CODEC_ID_PNG;
    else if(count>=3&&header[0]==0xff&&header[1]==0xd8&&header[2]==0xff)codec=AV_CODEC_ID_MJPEG;
    if(codec==AV_CODEC_ID_NONE)return 0;
    if((uint64_t)st.st_size>CAST_LOCAL_IMAGE_BYTES_MAX)
        return fail(error,size,"encoded image exceeds the 64 MiB local image limit");
    *input=av_find_input_format("image2pipe");
    if(!*input)return fail(error,size,"controlled image2pipe demuxer is unavailable");
    format->video_codec_id=codec;
    return 1;
}

int local_image_stream_info(AVFormatContext *format)
{
    if(!format || format->nb_streams!=1)return AVERROR(EINVAL);
    AVDictionary *options=NULL;
    av_dict_set_int(&options,"max_pixels",CAST_LOCAL_IMAGE_PIXELS_MAX,0);
    av_dict_set(&options,"threads","2",0);
    int result=avformat_find_stream_info(format,&options);
    av_dict_free(&options);return result;
}
