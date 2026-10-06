#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_codec.h"
#include <dlfcn.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MediaVideoConverter {
    struct SwsContext *scale;
    AVFrame *source;
    int source_width, source_height, width, height, format, threads;
    bool bt709;
};

void media_video_converter_free(MediaVideoConverter **converter)
{
    if (!converter || !*converter) {
        return;
    }
    sws_freeContext((*converter)->scale);
    av_frame_free(&(*converter)->source);
    free(*converter);
    *converter = NULL;
}

int media_video_converter_prepare(MediaVideoConverter **converter, int source_width,
                                  int source_height, const AVFrame *destination, bool bt709)
{
    if (!converter || !destination || source_width < 1 || source_width > INT_MAX / 4 ||
        source_height < 1 || destination->width < 1 || destination->height < 1) {
        return AVERROR(EINVAL);
    }
    MediaVideoConverter *current = *converter;
    if (current && current->source_width == source_width &&
        current->source_height == source_height && current->width == destination->width &&
        current->height == destination->height && current->format == destination->format &&
        current->bt709 == bt709) {
        return 0;
    }
    MediaVideoConverter *next = calloc(1, sizeof(*next));
    if (!next) {
        return AVERROR(ENOMEM);
    }
    next->source_width = source_width;
    next->source_height = source_height;
    next->width = destination->width;
    next->height = destination->height;
    next->format = destination->format;
    next->bt709 = bt709;
    next->threads = 1;
#ifdef CAST_MEDIA_PROFILE_LGPL
    /* The portable LGPL tree may lack x86 assembly. Use FFmpeg's supported
     * slice workers, rather than changing resolution, chroma or scaler quality.
     * System Community keeps its existing single-threaded conversion path. */
    if ((int64_t)next->width * next->height >= 1280 * 720) {
        next->threads = 4;
    }
#endif
    int result = 0;
    if (next->threads == 1) {
        next->scale =
            sws_getContext(source_width, source_height, AV_PIX_FMT_RGBA, next->width, next->height,
                           next->format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!next->scale) {
            result = AVERROR(ENOMEM);
        }
    } else {
        next->scale = sws_alloc_context();
        next->source = av_frame_alloc();
        if (!next->scale || !next->source) {
            result = AVERROR(ENOMEM);
        } else {
            const struct {
                const char *name;
                int value;
            } options[] = {{"srcw", source_width},           {"srch", source_height},
                           {"src_format", AV_PIX_FMT_RGBA},  {"dstw", next->width},
                           {"dsth", next->height},           {"dst_format", next->format},
                           {"sws_flags", SWS_FAST_BILINEAR}, {"threads", next->threads}};
            for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
                result = av_opt_set_int(next->scale, options[i].name, options[i].value, 0);
                if (result < 0) {
                    break;
                }
            }
            if (result >= 0) {
                result = sws_init_context(next->scale, NULL, NULL);
            }
        }
    }
    if (result >= 0 && bt709) {
        const int *colors = sws_getCoefficients(SWS_CS_ITU709);
        result = sws_setColorspaceDetails(next->scale, colors, 1, colors, 0, 0, 1 << 16, 1 << 16);
    }
    if (result < 0) {
        media_video_converter_free(&next);
        return result;
    }
    media_video_converter_free(converter);
    *converter = next;
    return 0;
}

static void borrowed_pixels_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

int media_video_convert(MediaVideoConverter **converter, const Frame *frame, AVFrame *destination,
                        bool bt709)
{
    if (!frame || !frame->data || frame->width < 1 || frame->width > INT_MAX / 4 ||
        frame->height < 1 || frame->stride < frame->width * 4 ||
        (size_t)frame->height > SIZE_MAX / (size_t)frame->stride || !destination ||
        !destination->data[0]) {
        return AVERROR(EINVAL);
    }
    int result =
        media_video_converter_prepare(converter, frame->width, frame->height, destination, bt709);
    if (result < 0) {
        return result;
    }
    MediaVideoConverter *current = *converter;
    if (current->threads == 1) {
        const uint8_t *source[4] = {frame->data, NULL, NULL, NULL};
        int strides[4] = {frame->stride, 0, 0, 0};
        result = sws_scale(current->scale, source, strides, 0, frame->height, destination->data,
                           destination->linesize);
        return result == destination->height ? 0 : (result < 0 ? result : AVERROR(EIO));
    }
    AVFrame *source = current->source;
    source->width = frame->width;
    source->height = frame->height;
    source->format = AV_PIX_FMT_RGBA;
    source->data[0] = frame->data;
    source->linesize[0] = frame->stride;
    /* Frame API slicing requires refcounted buffers. Borrow the caller's pixels
     * only for this synchronous call; never copy or retain a queue job's frame. */
    source->buf[0] = av_buffer_create(frame->data, (size_t)frame->stride * frame->height,
                                      borrowed_pixels_free, NULL, AV_BUFFER_FLAG_READONLY);
    if (!source->buf[0]) {
        av_frame_unref(source);
        return AVERROR(ENOMEM);
    }
    result = sws_scale_frame(current->scale, destination, source);
    av_frame_unref(source);
    return result < 0 ? result : 0;
}

static int encoder_open(const Config *cfg, bool stream, bool header, const char *name,
                        AVCodecContext **result, MediaCodecSelection *selection, char *error,
                        size_t size)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec || codec->type != AVMEDIA_TYPE_VIDEO || (stream && codec->id != AV_CODEC_ID_H264)) {
        snprintf(error, size, "video encoder '%s' unavailable%s", name,
                 stream ? " or not H.264 (FLV streaming requires H.264)" : "");
        return -1;
    }
    bool x264 = !strcmp(name, "libx264"), x265 = !strcmp(name, "libx265");
    bool lossless = codec->id == AV_CODEC_ID_FFV1 || codec->id == AV_CODEC_ID_RAWVIDEO ||
                    codec->id == AV_CODEC_ID_PNG || codec->id == AV_CODEC_ID_HUFFYUV ||
                    codec->id == AV_CODEC_ID_UTVIDEO;
    if (lossless && !stream && !strcmp(cfg->record_rate_control, "bitrate")) {
        snprintf(error, size,
                 "lossless encoder '%s' has no bitrate target; use record.rate_control=auto", name);
        return -1;
    }
    bool crf = !stream && (!strcmp(cfg->record_rate_control, "crf") ||
                           (!strcmp(cfg->record_rate_control, "auto") && (x264 || x265)));
    if (crf && !x264 && !x265) {
        snprintf(error, size,
                 "encoder '%s' does not support record.rate_control=crf; use bitrate or auto "
                 "(record.crf is not translated to OpenH264 quality)",
                 name);
        return -1;
    }
    AVCodecContext *v = avcodec_alloc_context3(codec);
    if (!v) {
        snprintf(error, size, "video encoder allocation failed");
        return -1;
    }
    v->width = cfg->width;
    v->height = cfg->height;
    v->pix_fmt = AV_PIX_FMT_YUV420P;
    const enum AVPixelFormat *formats = NULL;
    int count = 0;
    if (avcodec_get_supported_config(v, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     (const void **)&formats, &count) == 0 &&
        count > 0) {
        bool supported = false;
        for (int i = 0; i < count; i++) {
            supported |= formats[i] == v->pix_fmt;
        }
        if (!supported) {
            snprintf(error, size,
                     "encoder '%s' cannot accept CPU YUV420P frames; hardware frames require "
                     "an implemented device integration",
                     name);
            avcodec_free_context(&v);
            return -1;
        }
    }
    v->time_base = stream ? (AVRational){1, cfg->fps} : (AVRational){1, 1000000};
    v->framerate = (AVRational){cfg->fps, 1};
    v->gop_size = cfg->fps * 2;
    v->max_b_frames = 0;
    v->thread_count = 2;
    if (stream) {
        v->color_primaries = AVCOL_PRI_BT709;
        v->color_trc = AVCOL_TRC_BT709;
        v->colorspace = AVCOL_SPC_BT709;
        v->color_range = AVCOL_RANGE_MPEG;
    }
    if (header) {
        v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (!crf && !lossless) {
        v->bit_rate =
            (int64_t)(stream ? cfg->stream.video_bitrate_kbps : cfg->record_bitrate_kbps) * 1000;
        v->rc_max_rate = v->bit_rate;
        v->rc_buffer_size = (int)(v->bit_rate * 2);
        if (stream && x264) {
            v->rc_min_rate = v->bit_rate;
        }
    }
    AVDictionary *options = NULL;
    if (x264 || x265) {
        av_dict_set(&options, "preset", stream ? cfg->stream.encoder_preset : cfg->record_preset,
                    0);
        if (crf) {
            char value[16];
            snprintf(value, sizeof(value), "%d", cfg->record_crf);
            av_dict_set(&options, "crf", value, 0);
        }
    }
    if (x264) {
        av_dict_set(&options, "tune", "zerolatency", 0);
        if (stream) {
            av_dict_set(&options, "profile", "high", 0);
            av_dict_set(&options, "forced-idr", "1", 0);
            av_dict_set(&options, "x264-params", "nal-hrd=cbr:force-cfr=1:scenecut=0", 0);
        }
    } else if (!strcmp(name, "libopenh264")) {
        /* OpenH264 requires multiple slices to use encoder threads effectively.
         * Bound CPU parallelism while preserving a headless software baseline. */
        if ((int64_t)v->width * v->height >= 1280 * 720) {
            v->thread_count = 4;
            v->slices = 4;
        }
        av_dict_set(&options, "rc_mode", "bitrate", 0);
        av_dict_set(&options, "allow_skip_frames", "0", 0);
    }
    int code = avcodec_open2(v, codec, &options);
    AVDictionaryEntry *unused = av_dict_get(options, "", NULL, AV_DICT_IGNORE_SUFFIX);
    if (code < 0 || unused) {
        if (unused) {
            snprintf(error, size, "encoder '%s' rejected option '%s'; settings were not applied",
                     name, unused->key);
        } else {
            char detail[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(code, detail, sizeof(detail));
            snprintf(error, size, "cannot initialize encoder '%s': %s", name, detail);
        }
        av_dict_free(&options);
        avcodec_free_context(&v);
        return -1;
    }
    av_dict_free(&options);
    snprintf(selection->encoder, sizeof(selection->encoder), "%s", name);
    snprintf(
        selection->detail, sizeof(selection->detail), "%s",
        x264 || x265
            ? (crf ? "legacy CRF/preset active" : "bitrate; codec preset active")
            : (stream ? "target bitrate; stream.encoder_preset inactive for this encoder"
                      : "target bitrate; record.crf and record.preset inactive for this encoder"));
    if (lossless) {
        snprintf(selection->detail, sizeof(selection->detail),
                 "lossless; record.bitrate_kbps, record.crf and record.preset inactive");
    }
    if (!strcmp(name, "libopenh264")) {
        size_t used = strlen(selection->detail);
        snprintf(selection->detail + used, sizeof(selection->detail) - used,
                 "; OpenH264 bitrate may vary; codec frame skipping disabled");
    }
    *result = v;
    return 0;
}
int media_video_open(const Config *cfg, bool stream, bool header, AVCodecContext **result,
                     MediaCodecSelection *selection, char *error, size_t size)
{
    *result = NULL;
    memset(selection, 0, sizeof(*selection));
    const char *requested = stream ? cfg->stream.video_encoder : cfg->video_codec;
    if (strcmp(requested, "auto")) {
        return encoder_open(cfg, stream, header, requested, result, selection, error, size);
    }
#ifdef CAST_MEDIA_PROFILE_LGPL
    const char *candidates[] = {"libopenh264"};
#else
    const char *candidates[] = {"libx264", "libopenh264"};
#endif
    char first_failure[CAST_ERR] = "";
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!encoder_open(cfg, stream, header, candidates[i], result, selection, error, size)) {
            if (first_failure[0]) {
                size_t used = strlen(selection->detail);
                snprintf(selection->detail + used, sizeof(selection->detail) - used,
                         "; auto fallback: %.600s", first_failure);
            }
            return 0;
        }
        if (!first_failure[0]) {
            snprintf(first_failure, sizeof(first_failure), "%s", error);
        }
    }
    snprintf(error, size, "no usable reviewed CPU H.264 encoder: %.800s", first_failure);
    return -1;
}
int media_codec_check(const Config *cfg, bool stream, char *output, size_t size)
{
    AVCodecContext *context = NULL;
    MediaCodecSelection selection;
    if (media_video_open(cfg, stream, true, &context, &selection, output, size)) {
        return -1;
    }
    snprintf(output, size, "selected %s; %s", selection.encoder, selection.detail);
    avcodec_free_context(&context);
    return 0;
}
struct MediaLibrary {
    const char *name, *license, *configuration;
    const void *address;
};
static size_t libraries(struct MediaLibrary list[5])
{
    list[0] = (struct MediaLibrary){"avcodec", avcodec_license(), avcodec_configuration(),
                                    (const void *)(uintptr_t)avcodec_version};
    list[1] = (struct MediaLibrary){"avformat", avformat_license(), avformat_configuration(),
                                    (const void *)(uintptr_t)avformat_version};
    list[2] = (struct MediaLibrary){"avutil", avutil_license(), avutil_configuration(),
                                    (const void *)(uintptr_t)avutil_version};
    list[3] = (struct MediaLibrary){"swscale", swscale_license(), swscale_configuration(),
                                    (const void *)(uintptr_t)swscale_version};
    list[4] = (struct MediaLibrary){"swresample", swresample_license(), swresample_configuration(),
                                    (const void *)(uintptr_t)swresample_version};
    return 5;
}
int media_profile_validate(bool require_lgpl, char *error, size_t size)
{
    if (!require_lgpl) {
        return 0;
    }
    struct MediaLibrary list[5];
    size_t count = libraries(list);
    for (size_t i = 0; i < count; i++) {
        const struct MediaLibrary *lib = &list[i];
        if (strncmp(lib->license, "LGPL", 4) || strstr(lib->configuration, "--enable-gpl") ||
            strstr(lib->configuration, "--enable-nonfree")) {
            Dl_info info = {0};
            dladdr(lib->address, &info);
            snprintf(error, size,
                     "Pro requires LGPL shared media: loaded %s (%s) from %.600s; "
                     "restore the controlled media tree before starting capture",
                     lib->name, lib->license, info.dli_fname ? info.dli_fname : "unknown path");
            return -1;
        }
    }
    return 0;
}
void media_profile_report(char *output, size_t size)
{
    struct MediaLibrary list[5];
    size_t count = libraries(list);
    for (size_t i = 0; i < count; i++) {
        Dl_info info = {0};
        dladdr(list[i].address, &info);
        size_t used = strlen(output);
        if (used >= size) {
            return;
        }
        snprintf(output + used, size - used, "FFmpeg %s: %s; loaded %s\n", list[i].name,
                 list[i].license, info.dli_fname ? info.dli_fname : "unknown path");
    }
    size_t used = strlen(output);
    if (used < size) {
        snprintf(output + used, size - used, "FFmpeg configuration: %s\n", avcodec_configuration());
    }
}
