#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define RATE 48000
#define AUDIO_CHUNK 480
struct VideoJob {
    Frame frame;
    uint64_t pts_ns, generation;
};
struct CastRecorder {
    pthread_t worker;
    pthread_mutex_t mutex, encoder_mutex;
    pthread_cond_t changed;
    bool worker_started, shutdown, header_written;
    atomic_bool active, paused, finalizing;
    atomic_uint_fast64_t drops;
    CastAudio *audio;
    Config cfg;
    char path[PATH_MAX], file_path[PATH_MAX], error[CAST_ERR], codec_error[CAST_ERR];
    uint64_t segment_ns, elapsed_ns, generation, accept_after_ns, audio_segment_sample;
    int64_t audio_produced, audio_encoded, last_video_pts;
    struct VideoJob *queue;
    unsigned head, count, capacity;
    AVFormatContext *format;
    AVCodecContext *video, *sound;
    AVStream *vs, *as;
    AVFrame *vframe, *aframe;
    AVAudioFifo *fifo;
    struct SwsContext *scale;
    SwrContext *resample;
    int fd;
    AVIOContext *io;
#ifdef CAST_TEST
    atomic_int slow_ms, hold_ms, fail_after;
    atomic_bool codec_busy;
    atomic_int_fast64_t write_limit;
    int64_t written;
    int video_count;
#endif
};
static int fferror(char *e, size_t n, const char *action, int code)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, detail, sizeof(detail));
    snprintf(e, n, "%s: %s", action, detail);
    return -1;
}
static uint64_t ns_to_samples(uint64_t ns)
{
    return (ns / 1000000000) * RATE + (ns % 1000000000) * RATE / 1000000000;
}
static uint64_t elapsed_locked(CastRecorder *r, uint64_t now)
{
    return r->elapsed_ns +
           (atomic_load(&r->active) && !atomic_load(&r->paused) && now > r->segment_ns
                ? now - r->segment_ns
                : 0);
}
static void clear_queue(CastRecorder *r)
{
    while (r->count) {
        struct VideoJob *j = &r->queue[r->head];
        frame_free(&j->frame);
        r->head = (r->head + 1) % r->capacity;
        r->count--;
        atomic_fetch_add(&r->drops, 1);
    }
}
static int write_file(void *opaque, const uint8_t *data, int size)
{
    CastRecorder *r = opaque;
    int done = 0;
    while (done < size) {
#ifdef CAST_TEST
        int64_t limit = atomic_load(&r->write_limit);
        if (limit >= 0 && r->written + size - done > limit) {
            return AVERROR(ENOSPC);
        }
#endif
        ssize_t z = write(r->fd, data + done, (size_t)(size - done));
        if (z < 0 && errno == EINTR) {
            continue;
        }
        if (z < 0) {
            return AVERROR(errno);
        }
        if (!z) {
            return AVERROR(EIO);
        }
        done += (int)z;
#ifdef CAST_TEST
        r->written += z;
#endif
    }
    return size;
}
static int64_t seek_file(void *opaque, int64_t offset, int whence)
{
    CastRecorder *r = opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        if (fstat(r->fd, &st) < 0) {
            return AVERROR(errno);
        }
        return st.st_size;
    }
    off_t z = lseek(r->fd, (off_t)offset, whence & ~AVSEEK_FORCE);
    return z < 0 ? AVERROR(errno) : z;
}
static void free_encoder(CastRecorder *r)
{
    sws_freeContext(r->scale);
    r->scale = NULL;
    swr_free(&r->resample);
    av_audio_fifo_free(r->fifo);
    r->fifo = NULL;
    av_frame_free(&r->vframe);
    av_frame_free(&r->aframe);
    avcodec_free_context(&r->video);
    avcodec_free_context(&r->sound);
    if (r->format) {
        r->format->pb = NULL;
        avformat_free_context(r->format);
        r->format = NULL;
    }
    if (r->io) {
        av_freep(&r->io->buffer);
        avio_context_free(&r->io);
    }
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    r->vs = r->as = NULL;
    r->header_written = false;
}
static int packets(CastRecorder *r, AVCodecContext *c, AVStream *s)
{
    AVPacket *p = av_packet_alloc();
    if (!p) {
        return fferror(r->codec_error, sizeof(r->codec_error), "packet allocation",
                       AVERROR(ENOMEM));
    }
    int z;
    while ((z = avcodec_receive_packet(c, p)) >= 0) {
        av_packet_rescale_ts(p, c->time_base, s->time_base);
        p->stream_index = s->index;
        if (!p->duration && c == r->video) {
            p->duration = av_rescale_q(1, (AVRational){1, r->cfg.fps}, s->time_base);
        }
        int w = av_interleaved_write_frame(r->format, p);
        av_packet_unref(p);
        if (w < 0) {
            av_packet_free(&p);
            return fferror(r->codec_error, sizeof(r->codec_error),
                           "recording write failed (partial file retained)", w);
        }
    }
    av_packet_free(&p);
    if (z != AVERROR(EAGAIN) && z != AVERROR_EOF) {
        return fferror(r->codec_error, sizeof(r->codec_error), "encoder packet", z);
    }
    if (r->io && r->io->error < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error),
                       "recording I/O failed (partial file retained)", r->io->error);
    }
    return 0;
}
static int send_frame(CastRecorder *r, AVCodecContext *c, AVStream *s, AVFrame *f)
{
    int z = avcodec_send_frame(c, f);
    if (z < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "encode recording", z);
    }
    return packets(r, c, s);
}
static int setup_video(CastRecorder *r)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(r->cfg.video_codec);
    if (!codec) {
        snprintf(r->codec_error, sizeof(r->codec_error),
                 "video encoder '%s' unavailable; use software libx264", r->cfg.video_codec);
        return -1;
    }
    r->vs = avformat_new_stream(r->format, NULL);
    r->video = avcodec_alloc_context3(codec);
    r->vframe = av_frame_alloc();
    if (!r->vs || !r->video || !r->vframe) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video allocation", AVERROR(ENOMEM));
    }
    AVCodecContext *v = r->video;
    v->width = r->cfg.width;
    v->height = r->cfg.height;
    v->pix_fmt = AV_PIX_FMT_YUV420P;
    v->time_base = (AVRational){1, 1000000};
    v->framerate = (AVRational){r->cfg.fps, 1};
    v->gop_size = r->cfg.fps * 2;
    v->max_b_frames = 0;
    v->thread_count = 2;
    if (r->format->oformat->flags & AVFMT_GLOBALHEADER) {
        v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary *opts = NULL;
    char crf[32];
    snprintf(crf, sizeof(crf), "%d", r->cfg.record_crf);
    if (!strncmp(r->cfg.video_codec, "libx264", 7) || !strcmp(r->cfg.video_codec, "libx265")) {
        av_dict_set(&opts, "preset", r->cfg.record_preset, 0);
        av_dict_set(&opts, "crf", crf, 0);
        if (!strncmp(r->cfg.video_codec, "libx264", 7)) {
            av_dict_set(&opts, "tune", "zerolatency", 0);
        }
    }
    int z = avcodec_open2(v, codec, &opts);
    av_dict_free(&opts);
    if (z < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "open software video encoder", z);
    }
    r->vs->time_base = v->time_base;
    r->vs->avg_frame_rate = v->framerate;
    if ((z = avcodec_parameters_from_context(r->vs->codecpar, v)) < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video parameters", z);
    }
    r->vframe->format = v->pix_fmt;
    r->vframe->width = v->width;
    r->vframe->height = v->height;
    if ((z = av_frame_get_buffer(r->vframe, 32)) < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video buffers", z);
    }
    return 0;
}
static int setup_audio(CastRecorder *r)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(r->cfg.audio_codec);
    if (!codec) {
        snprintf(r->codec_error, sizeof(r->codec_error), "audio encoder '%s' unavailable",
                 r->cfg.audio_codec);
        return -1;
    }
    r->as = avformat_new_stream(r->format, NULL);
    r->sound = avcodec_alloc_context3(codec);
    r->aframe = av_frame_alloc();
    if (!r->as || !r->sound || !r->aframe) {
        return fferror(r->codec_error, sizeof(r->codec_error), "audio allocation", AVERROR(ENOMEM));
    }
    AVCodecContext *s = r->sound;
    s->sample_rate = RATE;
    s->time_base = (AVRational){1, RATE};
    s->bit_rate = 128000;
    s->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&s->ch_layout, 2);
    const enum AVSampleFormat *formats = NULL;
    int count = 0;
    if (avcodec_get_supported_config(s, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     (const void **)&formats, &count) == 0 &&
        count > 0) {
        s->sample_fmt = formats[0];
    }
    if (r->format->oformat->flags & AVFMT_GLOBALHEADER) {
        s->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    int z = avcodec_open2(s, codec, NULL);
    if (z < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "open audio encoder", z);
    }
    r->as->time_base = s->time_base;
    if ((z = avcodec_parameters_from_context(r->as->codecpar, s)) < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "audio parameters", z);
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    z = swr_alloc_set_opts2(&r->resample, &s->ch_layout, s->sample_fmt, RATE, &stereo,
                            AV_SAMPLE_FMT_FLT, RATE, 0, NULL);
    if (z < 0 || (z = swr_init(r->resample)) < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "audio sample conversion", z);
    }
    r->fifo = av_audio_fifo_alloc(s->sample_fmt, 2, 4096);
    if (!r->fifo) {
        return fferror(r->codec_error, sizeof(r->codec_error), "audio FIFO allocation",
                       AVERROR(ENOMEM));
    }
    r->aframe->format = s->sample_fmt;
    r->aframe->sample_rate = RATE;
    av_channel_layout_copy(&r->aframe->ch_layout, &s->ch_layout);
    r->aframe->nb_samples = s->frame_size ? s->frame_size : 1024;
    if ((z = av_frame_get_buffer(r->aframe, 0)) < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "audio buffers", z);
    }
    return 0;
}
static int fill_audio(CastRecorder *r, int64_t goal, uint64_t segment_ns, uint64_t segment_sample,
                      bool silence)
{
    /* Audio uses the same active monotonic timeline as video. Read each active
     * segment from its original capture time; silence replaces absent data. */
    while (r->audio_produced < goal) {
        int n = (int)(goal - r->audio_produced);
        if (n > AUDIO_CHUNK) {
            n = AUDIO_CHUNK;
        }
        float mix[AUDIO_CHUNK * 2];
        int64_t local = r->audio_produced - (int64_t)segment_sample;
        if (local < 0) {
            local = 0;
        }
        uint64_t ns = segment_ns + (uint64_t)local * 1000000000 / RATE;
        if (silence) {
            memset(mix, 0, (size_t)n * 2 * sizeof(float));
        } else {
            audio_read(r->audio, ns, mix, n);
        }
        uint8_t **converted = NULL;
        int line = 0;
        int z =
            av_samples_alloc_array_and_samples(&converted, &line, 2, n, r->sound->sample_fmt, 0);
        if (z < 0) {
            return fferror(r->codec_error, sizeof(r->codec_error), "audio conversion allocation",
                           z);
        }
        const uint8_t *in[1] = {(const uint8_t *)mix};
        z = swr_convert(r->resample, converted, n, in, n);
        if (z >= 0 && av_audio_fifo_write(r->fifo, (void **)converted, z) != z) {
            z = AVERROR(ENOMEM);
        }
        av_freep(&converted[0]);
        av_freep(&converted);
        if (z < 0) {
            return fferror(r->codec_error, sizeof(r->codec_error), "audio queue", z);
        }
        r->audio_produced += n;
        int size = r->sound->frame_size ? r->sound->frame_size : 1024;
        while (av_audio_fifo_size(r->fifo) >= size) {
            if ((z = av_frame_make_writable(r->aframe)) < 0) {
                return fferror(r->codec_error, sizeof(r->codec_error), "audio writable buffer", z);
            }
            r->aframe->nb_samples = size;
            if (av_audio_fifo_read(r->fifo, (void **)r->aframe->data, size) != size) {
                return fferror(r->codec_error, sizeof(r->codec_error), "audio FIFO read",
                               AVERROR(EIO));
            }
            r->aframe->pts = r->audio_encoded;
            r->audio_encoded += size;
            if (send_frame(r, r->sound, r->as, r->aframe) < 0) {
                return -1;
            }
        }
    }
    return 0;
}
static void zero_pending_audio(CastRecorder *r)
{
    if (!r->fifo) {
        return;
    }
    int n = av_audio_fifo_size(r->fifo);
    av_audio_fifo_reset(r->fifo);
    if (!n) {
        return;
    }
    uint8_t **silence = NULL;
    int line = 0;
    if (av_samples_alloc_array_and_samples(&silence, &line, 2, n, r->sound->sample_fmt, 0) < 0) {
        return;
    }
    av_samples_set_silence(silence, 0, n, 2, r->sound->sample_fmt);
    av_audio_fifo_write(r->fifo, (void **)silence, n);
    av_freep(&silence[0]);
    av_freep(&silence);
}
static int encode_video(CastRecorder *r, const struct VideoJob *job)
{
#ifdef CAST_TEST
    int hold = atomic_load(&r->hold_ms);
    if (hold > 0) {
        atomic_store(&r->codec_busy, true);
        struct timespec delay = {.tv_sec = hold / 1000, .tv_nsec = (hold % 1000) * 1000000L};
        nanosleep(&delay, NULL);
        atomic_store(&r->codec_busy, false);
    }
    if (atomic_load(&r->fail_after) >= 0 && r->video_count++ >= atomic_load(&r->fail_after)) {
        snprintf(r->codec_error, sizeof(r->codec_error),
                 "injected encoder failure; partial file retained");
        return -1;
    }
#endif
    int z = av_frame_make_writable(r->vframe);
    if (z < 0) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video writable buffer", z);
    }
    const Frame *f = &job->frame;
    r->scale =
        sws_getCachedContext(r->scale, f->width, f->height, AV_PIX_FMT_RGBA, r->cfg.width,
                             r->cfg.height, r->video->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!r->scale) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video conversion", AVERROR(ENOMEM));
    }
    const uint8_t *src[4] = {f->data, NULL, NULL, NULL};
    int stride[4] = {f->stride, 0, 0, 0};
    if (sws_scale(r->scale, src, stride, 0, f->height, r->vframe->data, r->vframe->linesize) !=
        r->cfg.height) {
        return fferror(r->codec_error, sizeof(r->codec_error), "video scaling", AVERROR(EIO));
    }
    int64_t pts = (int64_t)(job->pts_ns / 1000);
    int64_t tick = av_rescale_q(1, r->vs->time_base, r->video->time_base);
    if (tick < 1) {
        tick = 1;
    }
    if (pts < r->last_video_pts + tick) {
        pts = r->last_video_pts + tick;
    }
    r->last_video_pts = pts;
    r->vframe->pts = pts;
    return send_frame(r, r->video, r->vs, r->vframe);
}
/* Only the recording worker owns codec/muxer resources while a recording exists.
 * encoder_mutex also gives privacy barriers a point after all in-flight codec
 * calls, without holding the independent queue/status mutex. */
static int finish_encoder(CastRecorder *r, bool failed, uint64_t elapsed, uint64_t segment,
                          uint64_t segment_sample)
{
    int rc = failed ? -1 : 0;
    char original[CAST_ERR];
    snprintf(original, sizeof(original), "%s", r->codec_error);
    if (!failed && r->sound) {
        if (fill_audio(r, (int64_t)ns_to_samples(elapsed), segment, segment_sample, false) < 0) {
            rc = -1;
        }
        int pending = av_audio_fifo_size(r->fifo);
        if (pending > 0 && rc == 0) {
            int z = av_frame_make_writable(r->aframe);
            if (z >= 0) {
                r->aframe->nb_samples = pending;
                av_audio_fifo_read(r->fifo, (void **)r->aframe->data, pending);
                r->aframe->pts = r->audio_encoded;
                r->audio_encoded += pending;
                if (send_frame(r, r->sound, r->as, r->aframe) < 0) {
                    rc = -1;
                }
            } else {
                rc = fferror(r->codec_error, sizeof(r->codec_error), "final audio buffer", z);
            }
        }
    }
    if (r->header_written) {
        if (r->video && send_frame(r, r->video, r->vs, NULL) < 0) {
            rc = -1;
        }
        if (r->sound && send_frame(r, r->sound, r->as, NULL) < 0) {
            rc = -1;
        }
        int z = av_write_trailer(r->format);
        if (z < 0) {
            rc = fferror(r->codec_error, sizeof(r->codec_error),
                         "recording finalization failed; partial file retained", z);
        }
        avio_flush(r->io);
        if (r->io->error < 0) {
            rc = fferror(r->codec_error, sizeof(r->codec_error),
                         "recording final I/O; partial file retained", r->io->error);
        }
    }
    if (failed && original[0]) {
        snprintf(r->codec_error, sizeof(r->codec_error), "%s", original);
    }
    if (r->fd >= 0 && fsync(r->fd) < 0 && rc == 0) {
        rc = fferror(r->codec_error, sizeof(r->codec_error),
                     "recording fsync; partial file retained", AVERROR(errno));
    }
    free_encoder(r);
    pthread_mutex_lock(&r->mutex);
    clear_queue(r);
    snprintf(r->error, sizeof(r->error), "%s", rc < 0 ? r->codec_error : "");
    atomic_store(&r->active, false);
    atomic_store(&r->paused, false);
    atomic_store(&r->finalizing, false);
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return rc;
}
static void fail_recording(CastRecorder *r)
{
    pthread_mutex_lock(&r->mutex);
    r->elapsed_ns = elapsed_locked(r, cast_now_ns());
    uint64_t elapsed = r->elapsed_ns, segment = r->segment_ns;
    uint64_t segment_sample = r->audio_segment_sample;
    atomic_store(&r->active, false);
    atomic_store(&r->paused, false);
    atomic_store(&r->finalizing, true);
    clear_queue(r);
    pthread_mutex_unlock(&r->mutex);
    finish_encoder(r, true, elapsed, segment, segment_sample);
}
static void *worker(void *data)
{
    CastRecorder *r = data;
    for (;;) {
        pthread_mutex_lock(&r->mutex);
        while ((!atomic_load(&r->active) || atomic_load(&r->paused)) &&
               !atomic_load(&r->finalizing) && !r->shutdown) {
            pthread_cond_wait(&r->changed, &r->mutex);
        }
        if (r->shutdown && !atomic_load(&r->active) && !atomic_load(&r->finalizing)) {
            pthread_mutex_unlock(&r->mutex);
            return NULL;
        }
        struct VideoJob job = {0};
        if (r->count) {
            job = r->queue[r->head];
            memset(&r->queue[r->head], 0, sizeof(job));
            r->head = (r->head + 1) % r->capacity;
            r->count--;
        }
        pthread_mutex_unlock(&r->mutex);
#ifdef CAST_TEST
        /* Test slowdown precedes codec acceptance: a barrier can discard this
         * job while live/status/stop stay responsive. */
        int slow = atomic_load(&r->slow_ms);
        if (job.frame.data && slow > 0) {
            struct timespec delay = {.tv_sec = slow / 1000, .tv_nsec = (slow % 1000) * 1000000L};
            nanosleep(&delay, NULL);
        }
#endif
        pthread_mutex_lock(&r->encoder_mutex);
        pthread_mutex_lock(&r->mutex);
        bool finalizing = atomic_load(&r->finalizing);
        bool running = atomic_load(&r->active) && !atomic_load(&r->paused);
        bool valid = (running || finalizing) && job.generation == r->generation;
        uint64_t elapsed = elapsed_locked(r, cast_now_ns());
        uint64_t segment = r->segment_ns, segment_sample = r->audio_segment_sample;
        bool empty = !r->count;
        pthread_mutex_unlock(&r->mutex);
        int rc = 0;
        if (job.frame.data) {
            if (valid) {
                rc = encode_video(r, &job);
            } else {
                atomic_fetch_add(&r->drops, 1);
            }
            frame_free(&job.frame);
        }
        if (rc == 0 && finalizing && empty) {
            finish_encoder(r, false, elapsed, segment, segment_sample);
        } else if (rc == 0 && running) {
            /* A 30 ms horizon absorbs graph scheduling jitter. Limit each
             * batch to 100 ms so a privacy barrier can acquire encoder_mutex
             * between batches even when encoding has fallen behind. */
            uint64_t safe = elapsed > 30000000 ? elapsed - 30000000 : 0;
            int64_t goal = (int64_t)ns_to_samples(safe);
            if (goal > r->audio_produced + RATE / 10) {
                goal = r->audio_produced + RATE / 10;
            }
            rc = fill_audio(r, goal, segment, segment_sample, false);
        }
        if (rc < 0) {
            fail_recording(r);
        }
        pthread_mutex_unlock(&r->encoder_mutex);
        pthread_mutex_lock(&r->mutex);
        if (!r->count && !atomic_load(&r->finalizing) && atomic_load(&r->active) &&
            !atomic_load(&r->paused)) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += 10000000;
            if (until.tv_nsec >= 1000000000) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&r->changed, &r->mutex, &until);
        }
        pthread_mutex_unlock(&r->mutex);
    }
}
CastRecorder *recorder_open(CastAudio *audio)
{
    CastRecorder *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->audio = audio;
    r->fd = -1;
    r->last_video_pts = -1;
#ifdef CAST_TEST
    atomic_store(&r->fail_after, -1);
    atomic_store(&r->write_limit, -1);
#endif
    pthread_mutex_init(&r->mutex, NULL);
    pthread_mutex_init(&r->encoder_mutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&r->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&r->worker, NULL, worker, r) != 0) {
        pthread_cond_destroy(&r->changed);
        pthread_mutex_destroy(&r->mutex);
        pthread_mutex_destroy(&r->encoder_mutex);
        free(r);
        return NULL;
    }
    r->worker_started = true;
    return r;
}
static int create_file(CastRecorder *r, const char *path)
{
    if (path && *path) {
        if (strlen(path) >= sizeof(r->file_path)) {
            snprintf(r->codec_error, sizeof(r->codec_error), "record path is too long");
            return -1;
        }
        snprintf(r->file_path, sizeof(r->file_path), "%s", path);
        r->fd = open(r->file_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    } else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char stamp[64];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        const char *ext = !strcmp(r->cfg.record_container, "matroska") ? "mkv"
                          : !strcmp(r->cfg.record_container, "mp4")    ? "mp4"
                          : !strcmp(r->cfg.record_container, "webm")   ? "webm"
                                                                       : r->cfg.record_container;
        for (unsigned i = 0; i < 1000; i++) {
            int z = snprintf(r->file_path, sizeof(r->file_path), "%s/cast-%s-%ld-%u.%s",
                             r->cfg.record_dir, stamp, (long)getpid(), i, ext);
            if (z < 0 || (size_t)z >= sizeof(r->file_path)) {
                snprintf(r->codec_error, sizeof(r->codec_error),
                         "record directory path is too long");
                return -1;
            }
            r->fd = open(r->file_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (r->fd >= 0 || errno != EEXIST) {
                break;
            }
        }
    }
    if (r->fd < 0) {
        snprintf(r->codec_error, sizeof(r->codec_error),
                 "cannot create recording %.700s: %s (existing files are never overwritten)",
                 r->file_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(r->fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(r->codec_error, sizeof(r->codec_error), "recording target is not a regular file");
        close(r->fd);
        r->fd = -1;
        return -1;
    }
    return 0;
}
int recorder_start(CastRecorder *r, const Config *cfg, const char *path, char *reply, size_t n)
{
    pthread_mutex_lock(&r->mutex);
    if (atomic_load(&r->active) || atomic_load(&r->finalizing)) {
        snprintf(reply, n,
                 atomic_load(&r->finalizing)
                     ? "recording is finalizing; wait for status record.state=stopped"
                     : "a recording already exists; use record pause/resume/stop");
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }
    pthread_mutex_unlock(&r->mutex);
    pthread_mutex_lock(&r->encoder_mutex);
    r->cfg = *cfg;
    r->codec_error[0] = 0;
    r->audio_produced = r->audio_encoded = 0;
    r->last_video_pts = -1;
#ifdef CAST_TEST
    r->video_count = 0;
    r->written = 0;
#endif
    free(r->queue);
    r->queue = NULL;
    r->capacity = (unsigned)cfg->record_queue;
    r->head = r->count = 0;
    if (!r->capacity || r->capacity > 120) {
        snprintf(r->codec_error, sizeof(r->codec_error), "record queue must be 1..120");
        goto fail;
    }
    r->queue = calloc(r->capacity, sizeof(*r->queue));
    if (!r->queue) {
        snprintf(r->codec_error, sizeof(r->codec_error), "record queue allocation failed");
        goto fail;
    }
    if (create_file(r, path) < 0) {
        goto fail;
    }
    int z = avformat_alloc_output_context2(&r->format, NULL, cfg->record_container, r->file_path);
    if (z < 0 || !r->format) {
        fferror(r->codec_error, sizeof(r->codec_error), "recording container",
                z < 0 ? z : AVERROR(EINVAL));
        goto fail_file;
    }
    uint8_t *buf = av_malloc(65536);
    if (!buf) {
        fferror(r->codec_error, sizeof(r->codec_error), "recording I/O allocation",
                AVERROR(ENOMEM));
        goto fail_file;
    }
    r->io = avio_alloc_context(buf, 65536, 1, r, NULL, write_file, seek_file);
    if (!r->io) {
        av_free(buf);
        fferror(r->codec_error, sizeof(r->codec_error), "recording I/O context", AVERROR(ENOMEM));
        goto fail_file;
    }
    r->format->pb = r->io;
    r->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    r->format->max_interleave_delta = 100000;
    if (setup_video(r) < 0 || setup_audio(r) < 0) {
        goto fail_file;
    }
    AVDictionary *options = NULL;
    if (!strcmp(cfg->record_container, "matroska")) {
        av_dict_set(&options, "cluster_time_limit", "1000", 0);
        av_dict_set(&options, "cluster_size_limit", "1048576", 0);
    }
    z = avformat_write_header(r->format, &options);
    av_dict_free(&options);
    if (z < 0) {
        fferror(r->codec_error, sizeof(r->codec_error), "recording header", z);
        goto fail_file;
    }
    r->header_written = true;
    avio_flush(r->io);
    if (r->io->error < 0) {
        fferror(r->codec_error, sizeof(r->codec_error), "recording header I/O", r->io->error);
        goto fail_file;
    }
    pthread_mutex_lock(&r->mutex);
    r->error[0] = 0;
    snprintf(r->path, sizeof(r->path), "%s", r->file_path);
    r->elapsed_ns = 0;
    r->audio_segment_sample = 0;
    r->generation++;
    r->segment_ns = r->accept_after_ns = cast_now_ns();
    atomic_store(&r->paused, false);
    atomic_store(&r->active, true);
    pthread_cond_broadcast(&r->changed);
    snprintf(reply, n, "recording started: %s", r->path);
    pthread_mutex_unlock(&r->mutex);
    pthread_mutex_unlock(&r->encoder_mutex);
    return 0;
fail_file:
    free_encoder(r);
    unlink(r->file_path);
fail:
    pthread_mutex_lock(&r->mutex);
    snprintf(r->error, sizeof(r->error), "%s", r->codec_error);
    snprintf(reply, n, "%s", r->error);
    pthread_mutex_unlock(&r->mutex);
    pthread_mutex_unlock(&r->encoder_mutex);
    return -1;
}
int recorder_stop(CastRecorder *r, char *reply, size_t n)
{
    pthread_mutex_lock(&r->mutex);
    if (!atomic_load(&r->active)) {
        if (atomic_load(&r->finalizing)) {
            snprintf(reply, n, "recording finalizing: %s", r->path);
            pthread_mutex_unlock(&r->mutex);
            return 0;
        }
        snprintf(reply, n, "%s", r->error[0] ? r->error : "no recording is active");
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }
    r->elapsed_ns = elapsed_locked(r, cast_now_ns());
    atomic_store(&r->active, false);
    atomic_store(&r->paused, false);
    atomic_store(&r->finalizing, true);
    /* The worker drains accepted active frames, then muxes/fsyncs the same file.
     * This control path never enters the encoder or performs file I/O. */
    pthread_cond_broadcast(&r->changed);
    snprintf(reply, n, "recording finalizing: %s", r->path);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}
static bool lock_active_encoder(CastRecorder *r)
{
    while (pthread_mutex_trylock(&r->encoder_mutex) != 0) {
        /* A failed/stopped recording may be fsyncing for an unbounded disk
         * interval. Live privacy controls need no recording barrier then. */
        if (!atomic_load(&r->active)) {
            return false;
        }
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    return true;
}
int recorder_pause(CastRecorder *r, bool paused, char *reply, size_t n)
{
    pthread_mutex_lock(&r->mutex);
    if (!atomic_load(&r->active)) {
        snprintf(reply, n, "no recording is active");
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }
    if (atomic_load(&r->paused) == paused) {
        snprintf(reply, n, "recording %s: %s", paused ? "paused" : "resumed", r->path);
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }
    if (paused) {
        r->elapsed_ns = elapsed_locked(r, cast_now_ns());
        atomic_store(&r->paused, true);
        r->generation++;
        clear_queue(r);
    }
    uint64_t elapsed = r->elapsed_ns;
    uint64_t segment = r->segment_ns, segment_sample = r->audio_segment_sample;
    pthread_mutex_unlock(&r->mutex);
    /* The pause gate is already published. Codec calls accepted before the gate
     * may finish; queued/unsubmitted frames and pending audio are discarded. */
    if (!lock_active_encoder(r)) {
        snprintf(reply, n, "recording stopped during pause");
        return -1;
    }
    pthread_mutex_lock(&r->mutex);
    if (!atomic_load(&r->active)) {
        snprintf(reply, n, "%s", r->error[0] ? r->error : "recording stopped during pause");
        pthread_mutex_unlock(&r->mutex);
        pthread_mutex_unlock(&r->encoder_mutex);
        return -1;
    }
    pthread_mutex_unlock(&r->mutex);
    int rc = 0;
    if (paused) {
        zero_pending_audio(r);
        rc = fill_audio(r, (int64_t)ns_to_samples(elapsed), segment, segment_sample, true);
    }
    pthread_mutex_lock(&r->mutex);
    if (rc < 0) {
        snprintf(r->error, sizeof(r->error), "%s", r->codec_error);
    } else if (!paused && atomic_load(&r->active)) {
        r->segment_ns = r->accept_after_ns = cast_now_ns();
        r->audio_segment_sample = ns_to_samples(r->elapsed_ns);
        r->generation++;
        atomic_store(&r->paused, false);
    }
    snprintf(reply, n, rc < 0 ? "%s" : "recording %s: %s",
             rc < 0   ? r->error
             : paused ? "paused"
                      : "resumed",
             r->path);
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    if (rc < 0) {
        fail_recording(r);
    }
    pthread_mutex_unlock(&r->encoder_mutex);
    return rc;
}
int recorder_frame(CastRecorder *r, const Frame *f, char *e, size_t n)
{
    if (!atomic_load(&r->active) || atomic_load(&r->paused)) {
        return 0;
    }
    if (pthread_mutex_trylock(&r->mutex) != 0) {
        atomic_fetch_add(&r->drops, 1);
        return 1;
    }
    if (!atomic_load(&r->active) || atomic_load(&r->paused)) {
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }
    if (!f || !f->data || f->width != r->cfg.width || f->height != r->cfg.height ||
        f->stride < f->width * 4) {
        snprintf(e, n, "recording frame format/dimensions mismatch");
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }
    if (f->ts_ns < r->accept_after_ns) {
        atomic_fetch_add(&r->drops, 1);
        pthread_mutex_unlock(&r->mutex);
        return 1;
    }
    if (r->count == r->capacity) {
        struct VideoJob *old = &r->queue[r->head];
        frame_free(&old->frame);
        r->head = (r->head + 1) % r->capacity;
        r->count--;
        atomic_fetch_add(&r->drops, 1);
    }
    struct VideoJob *j = &r->queue[(r->head + r->count) % r->capacity];
    if (frame_copy(&j->frame, f) < 0) {
        snprintf(e, n, "recording queue frame allocation failed");
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }
    j->pts_ns = r->elapsed_ns + f->ts_ns - r->segment_ns;
    j->generation = r->generation;
    r->count++;
    pthread_cond_signal(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}
void recorder_barrier(CastRecorder *r)
{
    pthread_mutex_lock(&r->mutex);
    if (!atomic_load(&r->active)) {
        pthread_mutex_unlock(&r->mutex);
        return;
    }
    r->generation++;
    r->accept_after_ns = cast_now_ns();
    clear_queue(r);
    pthread_mutex_unlock(&r->mutex);
    if (lock_active_encoder(r)) {
        zero_pending_audio(r);
        pthread_mutex_unlock(&r->encoder_mutex);
    }
}
void recorder_status(CastRecorder *r, bool *active, bool *paused, uint64_t *drops, char *error,
                     size_t n)
{
    pthread_mutex_lock(&r->mutex);
    *active = atomic_load(&r->active);
    *paused = atomic_load(&r->paused);
    *drops = atomic_load(&r->drops);
    snprintf(error, n, "%s", r->error);
    pthread_mutex_unlock(&r->mutex);
}
bool recorder_finalizing(CastRecorder *r)
{
    return atomic_load(&r->finalizing);
}
uint64_t recorder_duration(CastRecorder *r)
{
    pthread_mutex_lock(&r->mutex);
    uint64_t z = elapsed_locked(r, cast_now_ns());
    pthread_mutex_unlock(&r->mutex);
    return z;
}
void recorder_path(CastRecorder *r, char *out, size_t n)
{
    pthread_mutex_lock(&r->mutex);
    snprintf(out, n, "%s", r->path);
    pthread_mutex_unlock(&r->mutex);
}
void recorder_close(CastRecorder *r)
{
    if (!r) {
        return;
    }
    char reply[CAST_ERR];
    if (atomic_load(&r->active)) {
        recorder_stop(r, reply, sizeof(reply));
    }
    pthread_mutex_lock(&r->mutex);
    r->shutdown = true;
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    if (r->worker_started) {
        pthread_join(r->worker, NULL);
    }
    free_encoder(r);
    clear_queue(r);
    free(r->queue);
    pthread_cond_destroy(&r->changed);
    pthread_mutex_destroy(&r->mutex);
    pthread_mutex_destroy(&r->encoder_mutex);
    free(r);
}
#ifdef CAST_TEST
void recorder_test_slow(CastRecorder *r, int ms)
{
    atomic_store(&r->slow_ms, ms);
}
void recorder_test_hold(CastRecorder *r, int ms)
{
    atomic_store(&r->hold_ms, ms);
}
bool recorder_test_codec_busy(CastRecorder *r)
{
    return atomic_load(&r->codec_busy);
}
void recorder_test_failure(CastRecorder *r, int frames)
{
    atomic_store(&r->fail_after, frames);
}
void recorder_test_write_limit(CastRecorder *r, int64_t bytes)
{
    atomic_store(&r->write_limit, bytes);
}
#endif
