#include "timeline_render.h"
#include "media_codec.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RATE 48000
#define READER_COUNT 8
#define OPERATION_NS UINT64_C(10000000000)
#define PIXEL_LIMIT (7680 * 4320)
#define INPUT_FORMATS "mov,matroska,webm,wav,mp3,flac,ogg,aac,png_pipe,jpeg_pipe"
typedef struct TimelineReader TimelineReader;
struct CastTimelineRenderer {
    CastTimeline timeline;
    Config config;
    CastTimelineCancel cancel;
    void *cancel_user;
    uint64_t deadline, sequence;
    TimelineReader *readers;
    Frame layer;
    float *mix;
    unsigned mix_capacity;
};
struct TimelineReader {
    CastTimelineRenderer *owner;
    uint64_t media_id, used;
    bool audio, empty, flushing, ended, current_valid, pending_valid;
    int fd, stream;
    int64_t last_target, fallback_ns;
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *decoded, *current, *pending;
    int64_t current_ns, pending_ns;
    struct SwsContext *scale;
    SwrContext *resample;
    float *samples;
    unsigned sample_capacity, sample_count;
    int64_t sample_start;
};
static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (size) {
        vsnprintf(error, size, format, args);
    }
    va_end(args);
    return -1;
}
static int fferror(char *error, size_t size, const char *action, int code)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, detail, sizeof detail);
    return fail(error, size, "%s: %s", action, detail);
}
static uint64_t samples(uint64_t ns)
{
    return ns / UINT64_C(1000000000) * RATE +
           ns % UINT64_C(1000000000) * RATE / UINT64_C(1000000000);
}
static uint64_t sample_ns(uint64_t sample)
{
    return sample / RATE * UINT64_C(1000000000) + sample % RATE * UINT64_C(1000000000) / RATE;
}
static bool cancelled(CastTimelineRenderer *r)
{
    return (r->cancel && r->cancel(r->cancel_user)) || cast_now_ns() > r->deadline;
}
static int interrupt(void *opaque)
{
    return cancelled(((TimelineReader *)opaque)->owner);
}
static int input_read(void *opaque, uint8_t *buffer, int count)
{
    TimelineReader *reader = opaque;
    if (interrupt(reader)) {
        return AVERROR_EXIT;
    }
    ssize_t got;
    do {
        got = read(reader->fd, buffer, (size_t)count);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? AVERROR(errno) : got ? (int)got : AVERROR_EOF;
}
static int64_t input_seek(void *opaque, int64_t offset, int whence)
{
    TimelineReader *reader = opaque;
    if (interrupt(reader)) {
        return AVERROR_EXIT;
    }
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        return fstat(reader->fd, &st) ? AVERROR(errno) : st.st_size;
    }
    off_t result = lseek(reader->fd, offset, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
}
static void reader_close(TimelineReader *reader)
{
    if (!reader) {
        return;
    }
    avcodec_free_context(&reader->codec);
    avformat_close_input(&reader->format);
    if (reader->io) {
        av_freep(&reader->io->buffer);
        avio_context_free(&reader->io);
    }
    if (reader->fd >= 0) {
        close(reader->fd);
    }
    av_packet_free(&reader->packet);
    av_frame_free(&reader->decoded);
    av_frame_free(&reader->current);
    av_frame_free(&reader->pending);
    sws_freeContext(reader->scale);
    swr_free(&reader->resample);
    free(reader->samples);
    memset(reader, 0, sizeof *reader);
    reader->fd = -1;
}
static int local_open(const CastTimelineMedia *media, char *error, size_t size)
{
    int fd = open(media->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return fail(error, size, "media %" PRIu64 " cannot open local source: %s", media->id,
                    strerror(errno));
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return fail(error, size, "media %" PRIu64 " requires a nonempty local regular file",
                    media->id);
    }
    return fd;
}
static int reader_open(TimelineReader *reader, CastTimelineRenderer *owner,
                       const CastTimelineMedia *media, bool audio, char *error, size_t size)
{
    memset(reader, 0, sizeof *reader);
    reader->fd = -1;
    reader->owner = owner;
    reader->audio = audio;
    reader->media_id = media->id;
    reader->last_target = -1;
    reader->fd = local_open(media, error, size);
    if (reader->fd < 0) {
        return -1;
    }
    reader->format = avformat_alloc_context();
    unsigned char *buffer = av_malloc(32768);
    reader->io =
        buffer ? avio_alloc_context(buffer, 32768, 0, reader, input_read, NULL, input_seek) : NULL;
    if (!reader->io) {
        av_free(buffer);
    }
    if (!reader->format || !reader->io) {
        return fail(error, size, "media %" PRIu64 " input allocation failed", media->id);
    }
    reader->format->pb = reader->io;
    reader->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    reader->format->max_streams = 16;
    reader->format->max_index_size = 1024 * 1024;
    reader->format->interrupt_callback = (AVIOInterruptCB){interrupt, reader};
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", INPUT_FORMATS, 0);
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
    int result = avformat_open_input(&reader->format, media->path, NULL, &options);
    av_dict_free(&options);
    if (result < 0 || (result = avformat_find_stream_info(reader->format, NULL)) < 0) {
        return fail(error, size, "media %" PRIu64 " unsupported or unreadable local source",
                    media->id);
    }
    reader->stream = av_find_best_stream(
        reader->format, audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (reader->stream < 0) {
        if (!audio || media->kind == CAST_TIMELINE_AUDIO) {
            return fail(error, size, "media %" PRIu64 " lacks its required stream", media->id);
        }
        reader->empty = true;
        return 0;
    }
    AVCodecParameters *parameters = reader->format->streams[reader->stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        return fail(error, size, "media %" PRIu64 " decoder unavailable", media->id);
    }
    reader->codec = avcodec_alloc_context3(codec);
    reader->decoded = av_frame_alloc();
    reader->current = av_frame_alloc();
    reader->pending = av_frame_alloc();
    reader->packet = av_packet_alloc();
    if (!reader->codec || !reader->decoded || !reader->current || !reader->pending ||
        !reader->packet) {
        return fail(error, size, "media %" PRIu64 " decoder allocation failed", media->id);
    }
    if ((result = avcodec_parameters_to_context(reader->codec, parameters)) < 0) {
        return fferror(error, size, "copy decoder parameters", result);
    }
    reader->codec->thread_count = 2;
    reader->codec->max_pixels = PIXEL_LIMIT;
    if ((result = avcodec_open2(reader->codec, codec, NULL)) < 0) {
        return fferror(error, size, "open media decoder", result);
    }
    return 0;
}
static TimelineReader *get_reader(CastTimelineRenderer *r, uint64_t media_id, bool audio,
                                  char *error, size_t size)
{
    TimelineReader *oldest = &r->readers[0];
    for (unsigned i = 0; i < READER_COUNT; ++i) {
        TimelineReader *reader = &r->readers[i];
        if (reader->media_id == media_id && reader->audio == audio) {
            reader->used = ++r->sequence;
            return reader;
        }
        if (!reader->media_id || (oldest->media_id && reader->used < oldest->used)) {
            oldest = reader;
        }
    }
    reader_close(oldest);
    const CastTimelineMedia *media = timeline_media(&r->timeline, media_id);
    if (!media || reader_open(oldest, r, media, audio, error, size)) {
        reader_close(oldest);
        return NULL;
    }
    oldest->used = ++r->sequence;
    return oldest;
}
static int64_t origin(const TimelineReader *reader)
{
    const AVStream *stream = reader->format->streams[reader->stream];
    if (stream->start_time != AV_NOPTS_VALUE) {
        return stream->start_time;
    }
    return reader->format->start_time == AV_NOPTS_VALUE
               ? 0
               : av_rescale_q(reader->format->start_time, AV_TIME_BASE_Q, stream->time_base);
}
static int64_t frame_time(TimelineReader *reader, const AVFrame *frame)
{
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    int64_t ns =
        pts == AV_NOPTS_VALUE
            ? reader->fallback_ns
            : av_rescale_q(pts - origin(reader), reader->format->streams[reader->stream]->time_base,
                           (AVRational){1, 1000000000});
    if (reader->audio && frame->sample_rate > 0) {
        reader->fallback_ns = ns + av_rescale(frame->nb_samples, 1000000000, frame->sample_rate);
    } else {
        AVRational rate = reader->format->streams[reader->stream]->avg_frame_rate;
        reader->fallback_ns =
            ns +
            (rate.num > 0 && rate.den > 0 ? av_rescale(rate.den, 1000000000, rate.num) : 40000000);
    }
    return ns;
}
static int seek_reader(TimelineReader *reader, int64_t ns, char *error, size_t size)
{
    const AVStream *stream = reader->format->streams[reader->stream];
    int64_t timestamp =
        av_rescale_q(ns, (AVRational){1, 1000000000}, stream->time_base) + origin(reader);
    int result = av_seek_frame(reader->format, reader->stream, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        return fferror(error, size, "seek local timeline source", result);
    }
    avcodec_flush_buffers(reader->codec);
    av_packet_unref(reader->packet);
    av_frame_unref(reader->current);
    av_frame_unref(reader->pending);
    av_frame_unref(reader->decoded);
    reader->flushing = reader->ended = reader->current_valid = reader->pending_valid = false;
    reader->sample_count = 0;
    reader->fallback_ns = ns;
    swr_free(&reader->resample);
    return 0;
}
/* Returns 0 for a frame, 1 for clean EOF, -1 for cancellation/decode errors. */
static int next_frame(TimelineReader *reader, char *error, size_t size)
{
    av_frame_unref(reader->decoded);
    for (;;) {
        if (interrupt(reader)) {
            return fail(error, size, "timeline decoding cancelled or timed out");
        }
        int result = avcodec_receive_frame(reader->codec, reader->decoded);
        if (result >= 0) {
            return 0;
        }
        if (result == AVERROR_EOF) {
            return 1;
        }
        if (result != AVERROR(EAGAIN)) {
            return fferror(error, size, "decode timeline source", result);
        }
        if (reader->flushing) {
            return 1;
        }
        do {
            av_packet_unref(reader->packet);
            result = av_read_frame(reader->format, reader->packet);
            if (result < 0) {
                break;
            }
        } while (reader->packet->stream_index != reader->stream);
        if (result == AVERROR_EOF) {
            reader->flushing = true;
            result = avcodec_send_packet(reader->codec, NULL);
        } else if (result < 0) {
            return fferror(error, size, "read timeline source", result);
        } else {
            result = avcodec_send_packet(reader->codec, reader->packet);
        }
        av_packet_unref(reader->packet);
        if (result < 0 && result != AVERROR_EOF) {
            return fferror(error, size, "submit timeline packet", result);
        }
    }
}
static AVFrame *video_frame(TimelineReader *reader, uint64_t ns, char *error, size_t size)
{
    if (reader->empty) {
        return NULL;
    }
    if ((reader->last_target >= 0 && ns < (uint64_t)reader->last_target) ||
        (reader->last_target<0 ? ns > UINT64_C(2000000000) : ns>(uint64_t) reader->last_target +
         UINT64_C(2000000000))) {
        if (seek_reader(reader, (int64_t)ns, error, size)) {
            return NULL;
        }
    }
    reader->last_target = (int64_t)ns;
    for (;;) {
        if (reader->pending_valid) {
            if (reader->pending_ns > (int64_t)ns) {
                break;
            }
            av_frame_unref(reader->current);
            av_frame_move_ref(reader->current, reader->pending);
            reader->current_ns = reader->pending_ns;
            reader->current_valid = true;
            reader->pending_valid = false;
        }
        if (reader->ended) {
            break;
        }
        int result = next_frame(reader, error, size);
        if (result < 0) {
            return NULL;
        }
        if (result) {
            reader->ended = true;
            break;
        }
        reader->pending_ns = frame_time(reader, reader->decoded);
        av_frame_move_ref(reader->pending, reader->decoded);
        reader->pending_valid = true;
    }
    return reader->current_valid ? reader->current : NULL;
}
CastTimelineRenderer *timeline_renderer_open(const CastTimeline *timeline, const Config *config,
                                             CastTimelineCancel cancel, void *user, char *error,
                                             size_t size)
{
    if (error && size) {
        *error = 0;
    }
    if (!timeline || !config || timeline_validate(timeline, error, size) ||
        config_validate(config, error, size)) {
        return NULL;
    }
    CastTimelineRenderer *r = calloc(1, sizeof *r);
    if (!r) {
        fail(error, size, "timeline renderer allocation failed");
        return NULL;
    }
    r->timeline = *timeline;
    r->config = *config;
    r->cancel = cancel;
    r->cancel_user = user;
    r->deadline = cast_now_ns() + OPERATION_NS;
    r->readers = calloc(READER_COUNT, sizeof *r->readers);
    if (!r->readers) {
        fail(error, size, "timeline reader allocation failed");
        timeline_renderer_close(r);
        return NULL;
    }
    for (unsigned i = 0; i < READER_COUNT; ++i) {
        r->readers[i].fd = -1;
    }
    for (unsigned i = 0; i < timeline->clip_count; ++i) {
        const CastTimelineClip *clip = &timeline->clips[i];
        if (clip->capture_locked) {
            fail(error, size, "clip %" PRIu64 " is still recording/finalizing", clip->id);
            timeline_renderer_close(r);
            return NULL;
        }
        const CastTimelineMedia *media = timeline_media(timeline, clip->media_id);
        int fd = local_open(media, error, size);
        if (fd < 0) {
            timeline_renderer_close(r);
            return NULL;
        }
        close(fd);
        if (cancelled(r)) {
            fail(error, size, "timeline open cancelled or timed out");
            timeline_renderer_close(r);
            return NULL;
        }
    }
    return r;
}
void timeline_renderer_close(CastTimelineRenderer *r)
{
    if (!r) {
        return;
    }
    if (r->readers) {
        for (unsigned i = 0; i < READER_COUNT; ++i) {
            reader_close(&r->readers[i]);
        }
    }
    free(r->readers);
    frame_free(&r->layer);
    free(r->mix);
    free(r);
}
int timeline_renderer_frame(CastTimelineRenderer *r, uint64_t time, Frame *out, char *error,
                            size_t size)
{
    if (error && size) {
        *error = 0;
    }
    if (!r || !out || time > CAST_TIMELINE_LIMIT_NS) {
        return fail(error, size, "invalid timeline frame request");
    }
    r->deadline = cast_now_ns() + OPERATION_NS;
    if (cancelled(r)) {
        return fail(error, size, "timeline rendering cancelled");
    }
    if (frame_alloc(out, r->config.width, r->config.height)) {
        return fail(error, size, "timeline frame allocation failed");
    }
    for (int y = 0; y < out->height; ++y) {
        for (int x = 0; x < out->width; ++x) {
            unsigned char *p = out->data + (size_t)y * out->stride + x * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    }
    for (unsigned track = 0; track < CAST_TIMELINE_TRACK_MAX; ++track) {
        for (unsigned i = 0; i < r->timeline.clip_count; ++i) {
            const CastTimelineClip *clip = &r->timeline.clips[i];
            const CastTimelineMedia *media = timeline_media(&r->timeline, clip->media_id);
            if (clip->track != track || media->kind == CAST_TIMELINE_AUDIO ||
                time < clip->start_ns || time - clip->start_ns >= clip->duration_ns) {
                continue;
            }
            TimelineReader *reader = get_reader(r, media->id, false, error, size);
            if (!reader) {
                return -1;
            }
            AVFrame *frame =
                video_frame(reader, clip->source_in_ns + time - clip->start_ns, error, size);
            if (!frame) {
                if (error && size && *error) {
                    return -1;
                }
                continue;
            }
            if (frame->width < 1 || frame->height < 1 ||
                (int64_t)frame->width * frame->height > PIXEL_LIMIT) {
                return fail(error, size, "media %" PRIu64 " exceeds decoder image bounds",
                            media->id);
            }
            double aspect = (double)frame->width / frame->height;
            if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
                aspect *= av_q2d(frame->sample_aspect_ratio);
            }
            int width = out->width, height = (int)llround(width / aspect);
            if (height > out->height) {
                height = out->height;
                width = (int)llround(height * aspect);
            }
            if (width < 1) {
                width = 1;
            }
            if (height < 1) {
                height = 1;
            }
            if (frame_alloc(&r->layer, width, height)) {
                return fail(error, size, "timeline layer allocation failed");
            }
            reader->scale = sws_getCachedContext(reader->scale, frame->width, frame->height,
                                                 frame->format, width, height, AV_PIX_FMT_RGBA,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
            if (!reader->scale) {
                return fail(error, size, "media scaler allocation failed");
            }
            const int *coefficients = sws_getCoefficients(
                frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
            if (sws_setColorspaceDetails(reader->scale, coefficients,
                                         frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0,
                                         1 << 16, 1 << 16) < 0) {
                return fail(error, size, "timeline colorspace configuration failed");
            }
            uint8_t *destination[] = {r->layer.data, NULL, NULL, NULL};
            int strides[] = {r->layer.stride, 0, 0, 0};
            if (sws_scale(reader->scale, (const uint8_t *const *)frame->data, frame->linesize, 0,
                          frame->height, destination, strides) < 0) {
                return fail(error, size, "timeline video conversion failed");
            }
            int left = (out->width - width) / 2, top = (out->height - height) / 2;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const uint8_t *src = r->layer.data + (size_t)y * r->layer.stride + 4 * x;
                    uint8_t *dst = out->data + (size_t)(y + top) * out->stride + 4 * (x + left);
                    unsigned alpha = src[3];
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        dst[channel] =
                            (uint8_t)((src[channel] * alpha + dst[channel] * (255 - alpha) + 127) /
                                      255);
                    }
                }
            }
        }
    }
    out->ts_ns = time;
    return 0;
}

static int audio_chunk(TimelineReader *reader, char *error, size_t size)
{
    int result = next_frame(reader, error, size);
    if (result) {
        if (result == 1 && reader->resample && swr_get_delay(reader->resample, RATE) > 0 &&
            reader->sample_capacity) {
            int64_t next = reader->sample_start + reader->sample_count;
            uint8_t *destination = (uint8_t *)reader->samples;
            int count =
                swr_convert(reader->resample, &destination, (int)reader->sample_capacity, NULL, 0);
            if (count < 0) {
                return fferror(error, size, "drain audio conversion", count);
            }
            if (count) {
                reader->sample_start = next;
                reader->sample_count = (unsigned)count;
                return 0;
            }
        }
        return result;
    }
    AVFrame *frame = reader->decoded;
    if (frame->sample_rate < 1 || frame->nb_samples < 1 || frame->nb_samples > 1048576) {
        return fail(error, size, "invalid decoded audio bounds");
    }
    if (!reader->resample) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        int code =
            swr_alloc_set_opts2(&reader->resample, &stereo, AV_SAMPLE_FMT_FLT, RATE,
                                &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
        if (code < 0 || swr_init(reader->resample) < 0) {
            return fail(error, size, "audio resampler initialization failed");
        }
    }
    int capacity = swr_get_out_samples(reader->resample, frame->nb_samples);
    if (capacity < 1 || capacity > 2097152) {
        return fail(error, size, "audio resampler bounds exceeded");
    }
    if ((unsigned)capacity > reader->sample_capacity) {
        float *buffer = realloc(reader->samples, (size_t)capacity * 2 * sizeof(float));
        if (!buffer) {
            return fail(error, size, "audio allocation failed");
        }
        reader->samples = buffer;
        reader->sample_capacity = (unsigned)capacity;
    }
    int64_t time = frame_time(reader, frame);
    reader->sample_start = av_rescale_q(time, (AVRational){1, 1000000000}, (AVRational){1, RATE}) -
                           swr_get_delay(reader->resample, RATE);
    uint8_t *destination = (uint8_t *)reader->samples;
    int count = swr_convert(reader->resample, &destination, capacity,
                            (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (count < 0) {
        return fferror(error, size, "audio conversion", count);
    }
    reader->sample_count = (unsigned)count;
    return 0;
}
static int audio_window(TimelineReader *reader, uint64_t start, unsigned count, float *out,
                        char *error, size_t size)
{
    memset(out, 0, (size_t)count * 2 * sizeof(float));
    if (reader->empty) {
        return 0;
    }
    uint64_t ns = sample_ns(start);
    if (reader->last_target < 0 ? ns > UINT64_C(2000000000)
                                : ns < (uint64_t)reader->last_target ||
                                      ns - (uint64_t)reader->last_target > UINT64_C(2000000000)) {
        if (seek_reader(reader, ns, error, size)) {
            return -1;
        }
    }
    reader->last_target = (int64_t)ns;
    int64_t end = (int64_t)start + count;
    for (;;) {
        int64_t chunk_end = reader->sample_start + reader->sample_count;
        if (reader->sample_count && chunk_end > (int64_t)start) {
            int64_t left =
                reader->sample_start > (int64_t)start ? reader->sample_start : (int64_t)start;
            int64_t right = chunk_end < end ? chunk_end : end;
            if (right > left) {
                memcpy(out + (left - (int64_t)start) * 2,
                       reader->samples + (left - reader->sample_start) * 2,
                       (size_t)(right - left) * 2 * sizeof(float));
            }
            if (chunk_end >= end) {
                return 0;
            }
        }
        int result = audio_chunk(reader, error, size);
        if (result) {
            return result < 0 ? -1 : 0;
        }
        if (reader->sample_start >= end) {
            return 0;
        }
    }
}
int timeline_renderer_audio(CastTimelineRenderer *r, uint64_t position, unsigned count, float *out,
                            char *error, size_t size)
{
    if (error && size) {
        *error = 0;
    }
    if (!r || !out || !count || count > RATE || position > samples(CAST_TIMELINE_LIMIT_NS) ||
        count > samples(CAST_TIMELINE_LIMIT_NS) - position) {
        return fail(error, size, "invalid timeline audio request");
    }
    r->deadline = cast_now_ns() + OPERATION_NS;
    if (cancelled(r)) {
        return fail(error, size, "timeline rendering cancelled");
    }
    memset(out, 0, (size_t)count * 2 * sizeof(float));
    if (count > r->mix_capacity) {
        float *buffer = realloc(r->mix, (size_t)count * 2 * sizeof(float));
        if (!buffer) {
            return fail(error, size, "timeline audio allocation failed");
        }
        r->mix = buffer;
        r->mix_capacity = count;
    }
    for (unsigned i = 0; i < r->timeline.clip_count; ++i) {
        const CastTimelineClip *clip = &r->timeline.clips[i];
        const CastTimelineMedia *media = timeline_media(&r->timeline, clip->media_id);
        if (clip->muted || clip->gain == 0 || media->kind == CAST_TIMELINE_IMAGE) {
            continue;
        }
        uint64_t begin = samples(clip->start_ns), end = samples(clip->start_ns + clip->duration_ns);
        uint64_t left = position > begin ? position : begin,
                 right = position + count < end ? position + count : end;
        if (right <= left) {
            continue;
        }
        TimelineReader *reader = get_reader(r, media->id, true, error, size);
        if (!reader) {
            return -1;
        }
        unsigned n = (unsigned)(right - left);
        if (audio_window(reader, samples(clip->source_in_ns) + left - begin, n, r->mix, error,
                         size)) {
            return -1;
        }
        for (unsigned j = 0; j < n * 2; ++j) {
            out[(left - position) * 2 + j] += r->mix[j] * clip->gain;
        }
    }
    for (unsigned i = 0; i < count * 2; ++i) {
        out[i] = isfinite(out[i]) ? fmaxf(-1, fminf(1, out[i])) : 0;
    }
    return 0;
}

struct CastTimelineExport {
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_bool cancelled;
    CastTimeline timeline;
    Config config;
    CastTimelineExportSnapshot snapshot;
    CastTimelineFrameFilter filter;
    void *filter_user;
};
static bool export_cancelled(void *opaque)
{
    return atomic_load(&((CastTimelineExport *)opaque)->cancelled);
}
static int encode_packets(AVFormatContext *format, AVCodecContext *codec, AVStream *stream,
                          AVFrame *frame, char *error, size_t size)
{
    int code = avcodec_send_frame(codec, frame);
    if (code < 0) {
        return fferror(error, size, "encoding timeline", code);
    }
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return fail(error, size, "packet allocation failed");
    }
    while ((code = avcodec_receive_packet(codec, packet)) >= 0) {
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        code = av_interleaved_write_frame(format, packet);
        av_packet_unref(packet);
        if (code < 0) {
            break;
        }
    }
    av_packet_free(&packet);
    return code == AVERROR(EAGAIN) || code == AVERROR_EOF
               ? 0
               : fferror(error, size, "writing timeline packets", code);
}
static int output_interrupt(void *opaque)
{
    return export_cancelled(opaque);
}
typedef struct {
    int fd;
    CastTimelineExport *job;
} ExportOutput;
static int output_write(void *opaque, const uint8_t *data, int count)
{
    ExportOutput *out = opaque;
    int done = 0;
    while (done < count) {
        if (export_cancelled(out->job)) {
            return AVERROR_EXIT;
        }
        ssize_t n = write(out->fd, data + done, (size_t)(count - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return AVERROR(errno ? errno : EIO);
        }
        done += (int)n;
    }
    return done;
}
static int64_t output_seek(void *opaque, int64_t offset, int whence)
{
    ExportOutput *out = opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        return fstat(out->fd, &st) ? AVERROR(errno) : st.st_size;
    }
    off_t n = lseek(out->fd, offset, whence & ~AVSEEK_FORCE);
    return n < 0 ? AVERROR(errno) : n;
}
static void *export_worker(void *opaque)
{
    CastTimelineExport *job = opaque;
    char error[CAST_ERR] = "", temporary[PATH_MAX] = "";
    CastTimelineRenderer *renderer = NULL;
    AVFormatContext *format = NULL;
    AVCodecContext *video = NULL, *audio = NULL;
    AVFrame *vf = NULL, *af = NULL;
    MediaVideoConverter *converter = NULL;
    SwrContext *resample = NULL;
    Frame rgba = {0};
    float *pcm = NULL;
    int fd = -1, result = -1;
    AVIOContext *output_io = NULL;
    ExportOutput output = {.fd = -1, .job = job};
    bool published = false;
    pthread_mutex_lock(&job->mutex);
    job->snapshot.state = CAST_TIMELINE_EXPORT_RUNNING;
    pthread_mutex_unlock(&job->mutex);
    renderer = timeline_renderer_open(&job->timeline, &job->config, export_cancelled, job, error,
                                      sizeof error);
    if (!renderer) {
        goto done;
    }
    if (!job->snapshot.duration_ns) {
        fail(error, sizeof error, "timeline is empty");
        goto done;
    }
    struct stat existing;
    if (lstat(job->snapshot.path, &existing) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_uid != geteuid() || existing.st_nlink != 1) {
            fail(error, sizeof error, "export destination must be an owned regular file");
            goto done;
        }
        for (unsigned i = 0; i < job->timeline.media_count; ++i) {
            struct stat source;
            if (stat(job->timeline.media[i].path, &source) == 0 &&
                source.st_dev == existing.st_dev && source.st_ino == existing.st_ino) {
                fail(error, sizeof error, "export would replace source media");
                goto done;
            }
        }
    } else if (errno != ENOENT) {
        fail(error, sizeof error, "cannot inspect export destination: %s", strerror(errno));
        goto done;
    }
    if (snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", job->snapshot.path) >=
        (int)sizeof temporary) {
        fail(error, sizeof error, "export path too long");
        goto done;
    }
    fd = mkstemp(temporary);
    if (fd < 0) {
        fail(error, sizeof error, "cannot create export temporary: %s", strerror(errno));
        goto done;
    }
    output.fd = fd;
    const char *container = job->config.record_container;
    if (strcmp(container, "matroska") && strcmp(container, "mp4") && strcmp(container, "mov")) {
        fail(error, sizeof error, "unsupported timeline export container");
        goto done;
    }
    if (avformat_alloc_output_context2(&format, NULL, container, temporary) < 0 || !format) {
        fail(error, sizeof error, "cannot allocate export muxer");
        goto done;
    }
    format->interrupt_callback = (AVIOInterruptCB){output_interrupt, job};
    MediaCodecSelection selection;
    if (media_video_open(&job->config, false, !!(format->oformat->flags & AVFMT_GLOBALHEADER),
                         &video, &selection, error, sizeof error)) {
        goto done;
    }
    video->color_primaries = AVCOL_PRI_BT709;
    video->color_trc = AVCOL_TRC_BT709;
    video->colorspace = AVCOL_SPC_BT709;
    video->color_range = AVCOL_RANGE_MPEG;
    AVStream *vs = avformat_new_stream(format, NULL);
    if (!vs || avcodec_parameters_from_context(vs->codecpar, video) < 0) {
        fail(error, sizeof error, "cannot initialize video stream");
        goto done;
    }
    vs->time_base = video->time_base;
    const AVCodec *encoder = avcodec_find_encoder_by_name(job->config.audio_codec);
    if (!encoder) {
        fail(error, sizeof error, "audio encoder unavailable: %s", job->config.audio_codec);
        goto done;
    }
    audio = avcodec_alloc_context3(encoder);
    if (!audio) {
        fail(error, sizeof error, "audio encoder allocation failed");
        goto done;
    }
    audio->sample_rate = RATE;
    audio->time_base = (AVRational){1, RATE};
    audio->bit_rate = 128000;
    audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const enum AVSampleFormat *formats = NULL;
    int format_count = 0;
    if (avcodec_get_supported_config(audio, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     (const void **)&formats, &format_count) == 0 &&
        format_count > 0) {
        audio->sample_fmt = formats[0];
    }
    av_channel_layout_default(&audio->ch_layout, 2);
    if (format->oformat->flags & AVFMT_GLOBALHEADER) {
        audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(audio, encoder, NULL) < 0) {
        fail(error, sizeof error, "cannot open audio encoder");
        goto done;
    }
    AVStream *as = avformat_new_stream(format, NULL);
    if (!as || avcodec_parameters_from_context(as->codecpar, audio) < 0) {
        fail(error, sizeof error, "cannot initialize audio stream");
        goto done;
    }
    as->time_base = audio->time_base;
    vf = av_frame_alloc();
    af = av_frame_alloc();
    if (!vf || !af) {
        fail(error, sizeof error, "export frame allocation failed");
        goto done;
    }
    vf->color_primaries = video->color_primaries;
    vf->color_trc = video->color_trc;
    vf->colorspace = video->colorspace;
    vf->color_range = video->color_range;
    vf->format = video->pix_fmt;
    vf->width = video->width;
    vf->height = video->height;
    if (av_frame_get_buffer(vf, 32) < 0) {
        fail(error, sizeof error, "video buffer allocation failed");
        goto done;
    }
    unsigned block = audio->frame_size > 0 ? (unsigned)audio->frame_size : 1024;
    af->format = audio->sample_fmt;
    af->sample_rate = RATE;
    af->nb_samples = (int)block;
    av_channel_layout_copy(&af->ch_layout, &audio->ch_layout);
    if (av_frame_get_buffer(af, 0) < 0) {
        fail(error, sizeof error, "audio buffer allocation failed");
        goto done;
    }
    pcm = calloc((size_t)block * 2, sizeof(float));
    if (!pcm) {
        fail(error, sizeof error, "audio mixing allocation failed");
        goto done;
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&resample, &audio->ch_layout, audio->sample_fmt, RATE, &stereo,
                            AV_SAMPLE_FMT_FLT, RATE, 0, NULL) < 0 ||
        swr_init(resample) < 0) {
        fail(error, sizeof error, "export audio conversion initialization failed");
        goto done;
    }
    uint8_t *output_buffer = av_malloc(32768);
    output_io = output_buffer ? avio_alloc_context(output_buffer, 32768, 1, &output, NULL,
                                                   output_write, output_seek)
                              : NULL;
    if (!output_io) {
        av_free(output_buffer);
        fail(error, sizeof error, "export IO allocation failed");
        goto done;
    }
    format->pb = output_io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_write_header(format, NULL) < 0) {
        fail(error, sizeof error, "cannot write export header");
        goto done;
    }
    uint64_t duration = job->snapshot.duration_ns, vi = 0, ai = 0, total = samples(duration);
    uint64_t video_time = 0, audio_time = 0;
    while (video_time < duration || ai < total) {
        if (export_cancelled(job)) {
            fail(error, sizeof error, "timeline export cancelled");
            goto done;
        }
        if (video_time < duration && (ai >= total || video_time <= audio_time)) {
            if (timeline_renderer_frame(renderer, video_time, &rgba, error, sizeof error)) {
                goto done;
            }
            if (job->filter &&
                job->filter(job->filter_user, video_time, &rgba, error, sizeof error)) {
                if (!*error) {
                    fail(error, sizeof error, "timeline frame filter failed");
                }
                goto done;
            }
            if (av_frame_make_writable(vf) < 0 ||
                media_video_convert(&converter, &rgba, vf, true)) {
                fail(error, sizeof error, "export video conversion failed");
                goto done;
            }
            vf->pts =
                av_rescale_q((int64_t)video_time, (AVRational){1, 1000000000}, video->time_base);
            if (encode_packets(format, video, vs, vf, error, sizeof error)) {
                goto done;
            }
            video_time = ++vi * UINT64_C(1000000000) / (unsigned)job->config.fps;
        } else {
            unsigned count = total - ai < block ? (unsigned)(total - ai) : block;
            memset(pcm, 0, (size_t)block * 2 * sizeof(float));
            if (timeline_renderer_audio(renderer, ai, count, pcm, error, sizeof error)) {
                goto done;
            }
            unsigned encoded = count;
            if (count < block && !(encoder->capabilities & (AV_CODEC_CAP_VARIABLE_FRAME_SIZE |
                                                            AV_CODEC_CAP_SMALL_LAST_FRAME))) {
                encoded = block;
            }
            if (av_frame_make_writable(af) < 0) {
                fail(error, sizeof error, "audio buffer is not writable");
                goto done;
            }
            const uint8_t *input = (const uint8_t *)pcm;
            af->nb_samples = swr_convert(resample, af->data, (int)encoded, &input, (int)encoded);
            af->pts = (int64_t)ai;
            if (af->nb_samples < 0 || encode_packets(format, audio, as, af, error, sizeof error)) {
                goto done;
            }
            ai += count;
            audio_time = sample_ns(ai);
        }
        pthread_mutex_lock(&job->mutex);
        job->snapshot.completed_ns = video_time < audio_time ? video_time : audio_time;
        pthread_mutex_unlock(&job->mutex);
    }
    if (encode_packets(format, video, vs, NULL, error, sizeof error) ||
        encode_packets(format, audio, as, NULL, error, sizeof error) ||
        av_write_trailer(format) < 0) {
        if (!*error) {
            fail(error, sizeof error, "export finalization failed");
        }
        goto done;
    }
    avio_flush(format->pb);
    if (format->pb->error < 0) {
        fail(error, sizeof error, "export write failed");
        goto done;
    }
    if (fsync(fd)) {
        fail(error, sizeof error, "export synchronization failed");
        goto done;
    }
    if (export_cancelled(job)) {
        fail(error, sizeof error, "timeline export cancelled");
        goto done;
    }
    struct stat final_stat;
    if (lstat(job->snapshot.path, &final_stat) == 0 &&
        (!S_ISREG(final_stat.st_mode) || final_stat.st_uid != geteuid() ||
         final_stat.st_nlink != 1)) {
        fail(error, sizeof error, "export destination changed to unsafe file");
        goto done;
    }
    if (rename(temporary, job->snapshot.path)) {
        fail(error, sizeof error, "export publish failed: %s", strerror(errno));
        goto done;
    }
    published = true;
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", job->snapshot.path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        if (slash == parent) {
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    } else {
        snprintf(parent, sizeof parent, ".");
    }
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd >= 0) {
        (void)fsync(parent_fd);
        close(parent_fd);
    }
    result = 0;
done:
    if (fd >= 0) {
        close(fd);
    }
    if (format) {
        format->pb = NULL;
    }
    if (output_io) {
        av_freep(&output_io->buffer);
        avio_context_free(&output_io);
    }
    avformat_free_context(format);
    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    av_frame_free(&vf);
    av_frame_free(&af);
    swr_free(&resample);
    media_video_converter_free(&converter);
    frame_free(&rgba);
    free(pcm);
    timeline_renderer_close(renderer);
    if (*temporary && !published) {
        unlink(temporary);
    }
    pthread_mutex_lock(&job->mutex);
    job->snapshot.state = result == 0             ? CAST_TIMELINE_EXPORT_COMPLETE
                          : export_cancelled(job) ? CAST_TIMELINE_EXPORT_CANCELLED
                                                  : CAST_TIMELINE_EXPORT_FAILED;
    if (result == 0) {
        job->snapshot.completed_ns = job->snapshot.duration_ns;
    }
    snprintf(job->snapshot.error, sizeof job->snapshot.error, "%s", error);
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}
CastTimelineExport *timeline_export_start_filtered(const CastTimeline *timeline,
                                                   const Config *config, const char *path,
                                                   CastTimelineFrameFilter filter, void *user,
                                                   char *error, size_t size)
{
    if (!timeline || !config || !path || !*path || strlen(path) >= PATH_MAX) {
        fail(error, size, "invalid timeline export request");
        return NULL;
    }
    CastTimeline copy = *timeline;
    Config cfg = *config;
    if (timeline_validate(&copy, error, size) || config_validate(&cfg, error, size)) {
        return NULL;
    }
    CastTimelineExport *job = calloc(1, sizeof *job);
    if (!job) {
        fail(error, size, "export allocation failed");
        return NULL;
    }
    job->timeline = copy;
    job->config = cfg;
    job->filter = filter;
    job->filter_user = user;
    job->snapshot.duration_ns = timeline_duration(&copy);
    snprintf(job->snapshot.path, sizeof job->snapshot.path, "%s", path);
    pthread_mutex_init(&job->mutex, NULL);
    atomic_init(&job->cancelled, false);
    if (pthread_create(&job->thread, NULL, export_worker, job)) {
        pthread_mutex_destroy(&job->mutex);
        free(job);
        fail(error, size, "cannot create export worker");
        return NULL;
    }
    return job;
}
CastTimelineExport *timeline_export_start(const CastTimeline *t, const Config *c, const char *p,
                                          char *e, size_t n)
{
    return timeline_export_start_filtered(t, c, p, NULL, NULL, e, n);
}
void timeline_export_snapshot(CastTimelineExport *job, CastTimelineExportSnapshot *out)
{
    if (job && out) {
        pthread_mutex_lock(&job->mutex);
        *out = job->snapshot;
        pthread_mutex_unlock(&job->mutex);
    }
}
void timeline_export_cancel(CastTimelineExport *job)
{
    if (job) {
        atomic_store(&job->cancelled, true);
    }
}
void timeline_export_close(CastTimelineExport *job)
{
    if (job) {
        timeline_export_cancel(job);
        pthread_join(job->thread, NULL);
        pthread_mutex_destroy(&job->mutex);
        free(job);
    }
}
