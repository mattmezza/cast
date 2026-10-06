#ifndef CAST_MEDIA_CODEC_H
#define CAST_MEDIA_CODEC_H
#include "cast.h"
#include <libavcodec/avcodec.h>
/* This layer opens a real encoder context before returning success. No capture,
 * output state or filesystem mutation is involved in an availability probe. */
typedef struct {
    char encoder[64];
    char detail[CAST_ERR];
} MediaCodecSelection;
int media_video_open(const Config *, bool streaming, bool global_header, AVCodecContext **,
                     MediaCodecSelection *, char *, size_t);
int media_codec_check(const Config *, bool streaming, char *, size_t);
int media_profile_validate(bool require_lgpl, char *, size_t);
void media_profile_report(char *, size_t);
typedef struct MediaVideoConverter MediaVideoConverter;
/* Synchronous conversion: no source or destination buffer is retained after
 * return. Worker privacy epochs and queue ownership remain with the caller. */
int media_video_converter_prepare(MediaVideoConverter **, int source_width, int source_height,
                                  const AVFrame *destination, bool bt709);
int media_video_convert(MediaVideoConverter **, const Frame *, AVFrame *, bool bt709);
void media_video_converter_free(MediaVideoConverter **);
#endif
