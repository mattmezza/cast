#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stream.h"
#include "media_internal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STREAM_MAGIC 0x43535432U
#define STREAM_RATE 48000
#define STREAM_AUDIO_RING 48000
#define STREAM_AUDIO_CHUNK 480
#define STREAM_KEY_MAX 1024
#define STREAM_WORKER_FD 3
#define STREAM_TICK_NS 10000000ULL
/* One latest-frame mailbox deliberately uses less than queue_frames' upper bound.
 * No encoded reference packets are dropped to recover from network backpressure. */
struct StreamShared {
    unsigned magic;
    size_t size, frame_size;
    Config cfg;
    pthread_mutex_t media_mutex;
    atomic_uint_fast64_t epoch, session, heartbeat, phase_started_ns;
    atomic_int phase, failure, failure_code, finished, connected;
    atomic_uint_fast64_t bytes_written, video_frames, audio_samples, dropped_frames;
    uint64_t frame_epoch, frame_ns, frame_sequence, consumed_sequence;
    bool have_frame, silent;
    uint64_t audio_begin, audio_end;
    float audio[STREAM_AUDIO_RING * 2];
#ifdef CAST_TEST
    atomic_int hold_ms, hold_stage;
#endif
    uint8_t frame[];
};
enum WorkerPhase {
    WORKER_IDLE,
    WORKER_CONNECT,
    WORKER_WRITE,
    WORKER_ENCODING
};
enum WorkerFailure {
    FAILURE_NONE,
    FAILURE_KEY,
    FAILURE_ENCODER,
    FAILURE_CONNECT,
    FAILURE_WRITE,
    FAILURE_LAG,
    FAILURE_INTERNAL
};
struct CastStream {
    pthread_t supervisor;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool started, shutdown, active, child_connected;
    int fd;
    pid_t child;
    CastAudio *audio;
    struct StreamShared *shared;
    size_t shared_size;
    Config cfg;
    StreamSnapshot status;
    uint64_t audio_next_ns, audio_epoch, connect_started_ns;
    uint64_t bitrate_sample_ns, bitrate_sample_bytes;
};
struct StreamEncoder {
    struct StreamShared *shared;
    AVFormatContext *format;
    AVCodecContext *video, *sound;
    AVStream *video_stream, *audio_stream;
    AVFrame *video_frame, *audio_frame;
    struct SwsContext *scale;
    uint64_t epoch, started_ns;
    int64_t video_pts, audio_pts;
    bool force_keyframe;
    uint64_t written_bytes;
};
extern char **environ;
static uint64_t samples_at(uint64_t ns)
{
    return ns / 1000000000ULL * STREAM_RATE + ns % 1000000000ULL * STREAM_RATE / 1000000000ULL;
}
static void wipe(void *memory, size_t size)
{
    explicit_bzero(memory, size);
}
static void lock_media(struct StreamShared *shared)
{
    int result = pthread_mutex_lock(&shared->media_mutex);
    if (result == EOWNERDEAD) {
        /* A deadline can kill an encoder during its short memory copy. No stale
         * partially copied frame may survive that process's retirement. */
        shared->have_frame = false;
        shared->audio_begin = shared->audio_end = 0;
        wipe(shared->frame, shared->frame_size);
        wipe(shared->audio, sizeof(shared->audio));
        pthread_mutex_consistent(&shared->media_mutex);
    }
}
static void clear_media_locked(struct StreamShared *shared)
{
    shared->have_frame = false;
    shared->consumed_sequence = shared->frame_sequence;
    shared->frame_ns = 0;
    shared->audio_begin = shared->audio_end = 0;
    wipe(shared->frame, shared->frame_size);
    wipe(shared->audio, sizeof(shared->audio));
}
const char *stream_lifecycle_name(StreamLifecycle state)
{
    static const char *names[] = {"stopped",      "connecting", "streaming",
                                  "reconnecting", "stopping",   "failed"};
    return state >= STREAM_STOPPED && state <= STREAM_FAILED ? names[state] : "failed";
}
bool stream_settings_changed(const StreamConfig *a, const StreamConfig *b)
{
    return strcmp(a->service, b->service) || strcmp(a->server_url, b->server_url) ||
           strcmp(a->key_file, b->key_file) || strcmp(a->tls_ca_file, b->tls_ca_file) ||
           strcmp(a->encoder_preset, b->encoder_preset) ||
           a->video_bitrate_kbps != b->video_bitrate_kbps ||
           a->audio_bitrate_kbps != b->audio_bitrate_kbps || a->queue_frames != b->queue_frames ||
           a->lag_ms != b->lag_ms || a->connect_timeout_ms != b->connect_timeout_ms ||
           a->write_timeout_ms != b->write_timeout_ms ||
           a->reconnect_attempts != b->reconnect_attempts ||
           a->reconnect_initial_ms != b->reconnect_initial_ms ||
           a->reconnect_max_ms != b->reconnect_max_ms;
}
static int read_key(const char *path, char *key, char *error, size_t size)
{
    if (!path || path[0] != '/') {
        snprintf(error, size,
                 "stream-key file must use an absolute path; set stream.key_file /absolute/path");
        return -1;
    }
    /* O_NONBLOCK ensures even a malicious replacement FIFO cannot block start.
     * O_NOFOLLOW and fstat inspect the opened inode rather than a racy stat path. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        snprintf(error, size,
                 "cannot open stream-key file; use an existing regular file (no symlink), owned by "
                 "your user, chmod 600, and an absolute stream.key_file path");
        return -1;
    }
    struct stat before, after;
    int result = -1;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != getuid() ||
        (before.st_mode & 0777) != 0600 || before.st_nlink != 1) {
        snprintf(error, size,
                 "stream-key file must be a regular file owned by your user, with one link and "
                 "mode 600; remove symlinks/hard links and run chmod 600 on the stream-key file");
        goto done;
    }
    if (before.st_size <= 0 || before.st_size > STREAM_KEY_MAX + 1) {
        snprintf(error, size,
                 "stream-key file must contain 1..1024 bytes on one line, optionally followed by "
                 "one newline");
        goto done;
    }
    size_t used = 0;
    while (used < STREAM_KEY_MAX + 2) {
        ssize_t count = read(fd, key + used, STREAM_KEY_MAX + 2 - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            snprintf(error, size,
                     "cannot read stream-key file; check its permissions and recreate it as a "
                     "regular mode-600 file");
            goto done;
        }
        if (!count) {
            break;
        }
        used += (size_t)count;
    }
    if (fstat(fd, &after) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec || after.st_uid != getuid() ||
        (after.st_mode & 0777) != 0600 || after.st_nlink != 1) {
        snprintf(error, size,
                 "stream-key file changed while being read; finish editing it, ensure mode 600, "
                 "then start again");
        goto done;
    }
    if (used && key[used - 1] == '\n') {
        used--;
    }
    if (!used || used > STREAM_KEY_MAX) {
        snprintf(error, size,
                 "stream-key file must contain 1..1024 bytes on one line, optionally followed by "
                 "one newline");
        goto done;
    }
    for (size_t i = 0; i < used; i++) {
        unsigned char byte = (unsigned char)key[i];
        /* Keys are URL path components. Query punctuation remains valid for
         * service-provided publish tokens; no whitespace/control/NUL is valid. */
        if (byte <= 32 || byte >= 127 || byte == '#' || byte == '\\') {
            snprintf(error, size,
                     "stream-key file must contain one nonempty ASCII line with no spaces, control "
                     "characters, fragments, or backslashes");
            goto done;
        }
    }
    key[used] = 0;
    result = 0;
done:
    close(fd);
    if (result) {
        wipe(key, STREAM_KEY_MAX + 3);
    }
    return result;
}
int stream_key_validate(const char *path, char *error, size_t size)
{
    char key[STREAM_KEY_MAX + 3];
    int result = read_key(path, key, error, size);
    wipe(key, sizeof(key));
    return result;
}
static bool protocol_available(const char *name)
{
    void *iterator = NULL;
    const char *protocol;
    while ((protocol = avio_enum_protocols(&iterator, 1))) {
        if (!strcmp(protocol, name)) {
            return true;
        }
    }
    return false;
}
static int local_support(const Config *cfg, char *error, size_t size)
{
    if (!avcodec_find_encoder_by_name("libx264") || !avcodec_find_encoder_by_name("aac")) {
        snprintf(error, size,
                 "streaming requires FFmpeg software libx264 and AAC encoders; install an FFmpeg "
                 "build containing both");
        return -1;
    }
    if (!av_guess_format("flv", NULL, NULL) || !protocol_available("rtmp")) {
        snprintf(error, size,
                 "streaming requires FFmpeg FLV muxer and RTMP protocol support; install a "
                 "compatible FFmpeg build");
        return -1;
    }
    if (!strncmp(cfg->stream.server_url, "rtmps://", 8) &&
        (!protocol_available("rtmps") || !protocol_available("tls"))) {
        snprintf(error, size,
                 "RTMPS requires FFmpeg RTMPS and TLS support; install a TLS-enabled FFmpeg build");
        return -1;
    }
    return 0;
}
static void child_phase(struct StreamShared *shared, int phase)
{
    atomic_store(&shared->phase_started_ns, cast_now_ns());
    atomic_store(&shared->phase, phase);
    atomic_store(&shared->heartbeat, cast_now_ns());
}
static int interrupted(void *opaque)
{
    struct StreamShared *shared = opaque;
    int phase = atomic_load(&shared->phase);
    uint64_t limit = (uint64_t)(phase == WORKER_CONNECT ? shared->cfg.stream.connect_timeout_ms
                                                        : shared->cfg.stream.write_timeout_ms) *
                     1000000ULL;
    uint64_t started = atomic_load(&shared->phase_started_ns);
    return atomic_load(&shared->session) == 0 || (started && cast_now_ns() - started > limit);
}
static void discard_ffmpeg_log(void *opaque, int level, const char *format, va_list arguments)
{
    (void)opaque;
    (void)level;
    (void)format;
    (void)arguments;
    /* This callback exists only in the exec-isolated child. FFmpeg protocol
     * diagnostics can contain tcUrl/playpath (the key), so none are forwarded.
     * Parent diagnostics use fixed action strings and numeric AVERROR values. */
}
static int worker_fail(struct StreamShared *shared, int failure, int code)
{
    atomic_store(&shared->failure_code, code);
    atomic_store(&shared->failure, failure);
    return -1;
}
static int setup_audio_encoder(struct StreamEncoder *encoder, bool initial)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    avcodec_free_context(&encoder->sound);
    av_frame_free(&encoder->audio_frame);
    encoder->sound = avcodec_alloc_context3(codec);
    encoder->audio_frame = av_frame_alloc();
    if (!encoder->sound || !encoder->audio_frame) {
        return AVERROR(ENOMEM);
    }
    AVCodecContext *sound = encoder->sound;
    sound->sample_rate = STREAM_RATE;
    sound->sample_fmt = AV_SAMPLE_FMT_FLTP;
    sound->time_base = (AVRational){1, STREAM_RATE};
    sound->bit_rate = (int64_t)encoder->shared->cfg.stream.audio_bitrate_kbps * 1000;
    sound->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(&sound->ch_layout, 2);
    int result = avcodec_open2(sound, codec, NULL);
    if (result < 0) {
        return result;
    }
    if (initial) {
        encoder->audio_stream = avformat_new_stream(encoder->format, NULL);
        if (!encoder->audio_stream) {
            return AVERROR(ENOMEM);
        }
        encoder->audio_stream->time_base = sound->time_base;
        result = avcodec_parameters_from_context(encoder->audio_stream->codecpar, sound);
        if (result < 0) {
            return result;
        }
    }
    AVFrame *frame = encoder->audio_frame;
    frame->format = sound->sample_fmt;
    frame->sample_rate = sound->sample_rate;
    frame->nb_samples = sound->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &sound->ch_layout);
    return av_frame_get_buffer(frame, 0);
}
static int setup_encoder(struct StreamEncoder *encoder)
{
    struct StreamShared *shared = encoder->shared;
    const Config *cfg = &shared->cfg;
    int result = avformat_alloc_output_context2(&encoder->format, NULL, "flv", NULL);
    if (result < 0 || !encoder->format) {
        return result < 0 ? result : AVERROR(ENOMEM);
    }
    encoder->format->interrupt_callback = (AVIOInterruptCB){interrupted, shared};
    /* Avoid the interleaver retaining audio/video across a privacy epoch. */
    encoder->format->max_interleave_delta = 0;
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    encoder->video = avcodec_alloc_context3(codec);
    encoder->video_frame = av_frame_alloc();
    encoder->video_stream = avformat_new_stream(encoder->format, NULL);
    if (!encoder->video || !encoder->video_frame || !encoder->video_stream) {
        return AVERROR(ENOMEM);
    }
    AVCodecContext *video = encoder->video;
    video->width = cfg->width;
    video->height = cfg->height;
    video->pix_fmt = AV_PIX_FMT_YUV420P;
    video->time_base = (AVRational){1, cfg->fps};
    video->framerate = (AVRational){cfg->fps, 1};
    video->bit_rate = (int64_t)cfg->stream.video_bitrate_kbps * 1000;
    video->rc_max_rate = video->rc_min_rate = video->bit_rate;
    video->rc_buffer_size = (int)(video->bit_rate * 2);
    video->gop_size = cfg->fps * 2;
    video->max_b_frames = 0;
    video->thread_count = 2;
    video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    video->color_primaries = AVCOL_PRI_BT709;
    video->color_trc = AVCOL_TRC_BT709;
    video->colorspace = AVCOL_SPC_BT709;
    video->color_range = AVCOL_RANGE_MPEG;
    AVDictionary *options = NULL;
    av_dict_set(&options, "preset", cfg->stream.encoder_preset, 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
    av_dict_set(&options, "profile", "high", 0);
    av_dict_set(&options, "forced-idr", "1", 0);
    av_dict_set(&options, "x264-params", "nal-hrd=cbr:force-cfr=1:scenecut=0", 0);
    result = avcodec_open2(video, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
        return result;
    }
    encoder->video_stream->time_base = video->time_base;
    encoder->video_stream->avg_frame_rate = video->framerate;
    result = avcodec_parameters_from_context(encoder->video_stream->codecpar, video);
    if (result < 0) {
        return result;
    }
    encoder->video_frame->format = video->pix_fmt;
    encoder->video_frame->width = video->width;
    encoder->video_frame->height = video->height;
    result = av_frame_get_buffer(encoder->video_frame, 32);
    if (result < 0 || (result = setup_audio_encoder(encoder, true)) < 0) {
        return result;
    }
    encoder->scale =
        sws_getContext(cfg->width, cfg->height, AV_PIX_FMT_RGBA, cfg->width, cfg->height,
                       video->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!encoder->scale) {
        return AVERROR(ENOMEM);
    }
    const int *colors = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(encoder->scale, colors, 1, colors, 0, 0, 1 << 16, 1 << 16);
    return 0;
}
static void free_encoder(struct StreamEncoder *encoder)
{
    sws_freeContext(encoder->scale);
    av_frame_free(&encoder->video_frame);
    av_frame_free(&encoder->audio_frame);
    avcodec_free_context(&encoder->video);
    avcodec_free_context(&encoder->sound);
    if (encoder->format) {
        if (encoder->format->pb) {
            child_phase(encoder->shared, WORKER_WRITE);
            avio_closep(&encoder->format->pb);
        }
        avformat_free_context(encoder->format);
    }
}
static int write_packets(struct StreamEncoder *encoder, AVCodecContext *codec, AVStream *stream)
{
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return AVERROR(ENOMEM);
    }
    int result;
    while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
        if (encoder->epoch != atomic_load(&encoder->shared->epoch)) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        /* AAC's initial encoder delay can yield a negative first timestamp.
         * Make all streams begin at 0 while retaining the sample clock. */
        if (codec == encoder->sound) {
            packet->pts +=
                av_rescale_q(codec->initial_padding, codec->time_base, stream->time_base);
            packet->dts +=
                av_rescale_q(codec->initial_padding, codec->time_base, stream->time_base);
        }
        child_phase(encoder->shared, WORKER_WRITE);
        /* No interleaved muxer queue: each accepted packet is submitted now.
         * Network-accepted packets cannot be retracted by a privacy command. */
        result = av_write_frame(encoder->format, packet);
        if (result >= 0) {
            avio_flush(encoder->format->pb);
            result = encoder->format->pb->error;
        }
        child_phase(encoder->shared, WORKER_ENCODING);
        if (result < 0) {
            av_packet_free(&packet);
            return result;
        }
        uint64_t written = (uint64_t)encoder->format->pb->bytes_written;
        atomic_fetch_add(&encoder->shared->bytes_written, written - encoder->written_bytes);
        encoder->written_bytes = written;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}
static int epoch_refresh(struct StreamEncoder *encoder)
{
    uint64_t current = atomic_load(&encoder->shared->epoch);
    if (encoder->epoch == current) {
        return 0;
    }
    /* x264 zerolatency has no delayed reference frames. The next submitted
     * video frame is an IDR; delayed AAC samples are retired by replacement. */
    encoder->epoch = current;
    encoder->force_keyframe = true;
    return setup_audio_encoder(encoder, false);
}
static int encode_video(struct StreamEncoder *encoder)
{
    struct StreamShared *shared = encoder->shared;
    uint8_t *raw = malloc(shared->frame_size);
    if (!raw) {
        return AVERROR(ENOMEM);
    }
    uint64_t epoch = atomic_load(&shared->epoch);
    bool have_frame;
    lock_media(shared);
    have_frame = shared->have_frame && shared->frame_epoch == epoch;
    if (have_frame) {
        memcpy(raw, shared->frame, shared->frame_size);
        shared->consumed_sequence = shared->frame_sequence;
    }
    pthread_mutex_unlock(&shared->media_mutex);
    if (!have_frame) {
        /* Waiting for a freshly composed privacy frame never reveals a raw
         * source. The configured background is the conservative fallback. */
        uint32_t color = shared->cfg.pause_color;
        for (size_t i = 0; i < shared->frame_size; i += 4) {
            raw[i] = (uint8_t)(color >> 16);
            raw[i + 1] = (uint8_t)(color >> 8);
            raw[i + 2] = (uint8_t)color;
            raw[i + 3] = 255;
        }
    }
    int result = av_frame_make_writable(encoder->video_frame);
    if (result >= 0) {
        const uint8_t *source[] = {raw};
        int stride[] = {shared->cfg.width * 4};
        sws_scale(encoder->scale, source, stride, 0, shared->cfg.height, encoder->video_frame->data,
                  encoder->video_frame->linesize);
    }
    wipe(raw, shared->frame_size);
    free(raw);
    if (result < 0) {
        return result;
    }
    if (epoch != atomic_load(&shared->epoch)) {
        return 0;
    }
    encoder->video_frame->pts = encoder->video_pts;
    encoder->video_frame->pict_type =
        encoder->force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    encoder->force_keyframe = false;
    result = avcodec_send_frame(encoder->video, encoder->video_frame);
    /* Remove raw converted pixels before any potentially stalled network write.
     * avcodec_send_frame has accepted the media; codec-owned copies are scoped
     * to that epoch and its packets are checked before network acceptance. */
    if (av_frame_make_writable(encoder->video_frame) >= 0) {
        for (int plane = 0; plane < 3; plane++) {
            int height = plane ? shared->cfg.height / 2 : shared->cfg.height;
            wipe(encoder->video_frame->data[plane],
                 (size_t)encoder->video_frame->linesize[plane] * height);
        }
    }
    if (result < 0) {
        return result;
    }
    atomic_fetch_add(&shared->video_frames, 1);
    return write_packets(encoder, encoder->video, encoder->video_stream);
}
static int encode_audio(struct StreamEncoder *encoder)
{
    struct StreamShared *shared = encoder->shared;
    int count = encoder->sound->frame_size;
    int result = av_frame_make_writable(encoder->audio_frame);
    if (result < 0) {
        return result;
    }
    uint64_t epoch = atomic_load(&shared->epoch);
    /* Capture latency is a fixed 60ms; the continuous video/audio output clocks
     * share one origin and are never reset by pause/freeze/blur. */
    uint64_t begin = samples_at(encoder->started_ns - 60000000ULL) + (uint64_t)encoder->audio_pts;
    lock_media(shared);
    for (int i = 0; i < count; i++) {
        uint64_t sample = begin + (uint64_t)i;
        bool available =
            !shared->silent && sample >= shared->audio_begin && sample < shared->audio_end;
        for (int channel = 0; channel < 2; channel++) {
            ((float *)encoder->audio_frame->data[channel])[i] =
                available ? shared->audio[(sample % STREAM_AUDIO_RING) * 2 + (unsigned)channel] : 0;
        }
    }
    pthread_mutex_unlock(&shared->media_mutex);
    if (epoch != atomic_load(&shared->epoch)) {
        for (int channel = 0; channel < 2; channel++) {
            wipe(encoder->audio_frame->data[channel], (size_t)count * sizeof(float));
        }
        return 0;
    }
    encoder->audio_frame->pts = encoder->audio_pts;
    result = avcodec_send_frame(encoder->sound, encoder->audio_frame);
    if (av_frame_make_writable(encoder->audio_frame) >= 0) {
        for (int channel = 0; channel < 2; channel++) {
            wipe(encoder->audio_frame->data[channel], (size_t)count * sizeof(float));
        }
    }
    if (result < 0) {
        return result;
    }
    encoder->audio_pts += count;
    atomic_fetch_add(&shared->audio_samples, (uint64_t)count);
    return write_packets(encoder, encoder->sound, encoder->audio_stream);
}
#ifdef CAST_TEST
static void test_hold(struct StreamShared *shared, int stage)
{
    if (atomic_load(&shared->hold_stage) == stage) {
        int milliseconds = atomic_load(&shared->hold_ms);
        struct timespec pause = {.tv_sec = milliseconds / 1000,
                                 .tv_nsec = (milliseconds % 1000) * 1000000L};
        nanosleep(&pause, NULL);
    }
}
#endif
int stream_worker_main(int fd)
{
    struct stat metadata;
    if (fd != STREAM_WORKER_FD || fstat(fd, &metadata) ||
        metadata.st_size < (off_t)sizeof(struct StreamShared)) {
        return 2;
    }
    struct StreamShared *shared =
        mmap(NULL, (size_t)metadata.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        return 2;
    }
    if (shared->magic != STREAM_MAGIC || shared->size != (size_t)metadata.st_size ||
        shared->frame_size != (size_t)shared->cfg.width * shared->cfg.height * 4 ||
        shared->size != sizeof(*shared) + shared->frame_size) {
        munmap(shared, (size_t)metadata.st_size);
        return 2;
    }
    /* A detached daemon must not leave an orphan credential-bearing worker. */
    pid_t parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    if (getppid() != parent || parent == 1) {
        munmap(shared, shared->size);
        return 2;
    }
    av_log_set_callback(discard_ffmpeg_log);
    signal(SIGPIPE, SIG_IGN);
    struct StreamEncoder encoder = {.shared = shared, .force_keyframe = true};
    char key[STREAM_KEY_MAX + 3], ignored[CAST_ERR];
    int result = read_key(shared->cfg.stream.key_file, key, ignored, sizeof(ignored));
    wipe(ignored, sizeof(ignored));
    if (result) {
        worker_fail(shared, FAILURE_KEY, EACCES);
        goto finished;
    }
    child_phase(shared, WORKER_ENCODING);
    result = setup_encoder(&encoder);
    if (result < 0) {
        worker_fail(shared, FAILURE_ENCODER, result);
        goto finished;
    }
    char url[sizeof(shared->cfg.stream.server_url) + STREAM_KEY_MAX + 4];
    const char *server = shared->cfg.stream.server_url;
    /* Service application query parameters belong before the appended key.
     * They are retained in tcUrl; key-specific query parameters remain playpath. */
    const char *query = strchr(server, '?');
    size_t base = query ? (size_t)(query - server) : strlen(server);
    while (base && server[base - 1] == '/') {
        base--;
    }
    snprintf(url, sizeof(url), "%.*s/%s%s", (int)base, server, key, query ? query : "");
    wipe(key, sizeof(key));
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "rtmp,rtmps,tcp,tls", 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    if (shared->cfg.stream.tls_ca_file[0]) {
        av_dict_set(&options, "ca_file", shared->cfg.stream.tls_ca_file, 0);
    }
    av_dict_set(&options, "rtmp_live", "live", 0);
    av_dict_set(&options, "tcp_nodelay", "1", 0);
    av_dict_set_int(&options, "rw_timeout", (int64_t)shared->cfg.stream.write_timeout_ms * 1000, 0);
    child_phase(shared, WORKER_CONNECT);
#ifdef CAST_TEST
    test_hold(shared, WORKER_CONNECT);
#endif
    result = avio_open2(&encoder.format->pb, url, AVIO_FLAG_WRITE,
                        &encoder.format->interrupt_callback, &options);
    wipe(url, sizeof(url));
    av_dict_free(&options);
    if (result < 0) {
        worker_fail(shared, FAILURE_CONNECT, result);
        goto finished;
    }
    encoder.format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
    result = avformat_write_header(encoder.format, &options);
    av_dict_free(&options);
    if (result < 0) {
        worker_fail(shared, FAILURE_CONNECT, result);
        goto finished;
    }
    atomic_store(&shared->connected, 1);
    encoder.started_ns = cast_now_ns();
    encoder.epoch = atomic_load(&shared->epoch);
    child_phase(shared, WORKER_IDLE);
    while (atomic_load(&shared->session)) {
        uint64_t now = cast_now_ns();
        uint64_t elapsed = now - encoder.started_ns;
        uint64_t video_due = (uint64_t)encoder.video_pts * 1000000000ULL / shared->cfg.fps;
        uint64_t audio_due = (uint64_t)encoder.audio_pts * 1000000000ULL / STREAM_RATE;
        uint64_t due = video_due < audio_due ? video_due : audio_due;
        if (elapsed > due + (uint64_t)shared->cfg.stream.lag_ms * 1000000ULL) {
            /* Do not catch up by emitting seconds-old packets. Reopen fresh
             * encoders/headers through the finite supervisor retry policy. */
            worker_fail(shared, FAILURE_LAG, ETIMEDOUT);
            break;
        }
        if (due > elapsed) {
            uint64_t wait = due - elapsed;
            if (wait > 5000000ULL) {
                wait = 5000000ULL;
            }
            struct timespec pause = {.tv_nsec = (long)wait};
            nanosleep(&pause, NULL);
            continue;
        }
        child_phase(shared, WORKER_ENCODING);
        result = epoch_refresh(&encoder);
        if (result >= 0) {
            if (video_due <= audio_due) {
                result = encode_video(&encoder);
                encoder.video_pts++;
            } else {
                result = encode_audio(&encoder);
            }
        }
        if (result < 0) {
            worker_fail(shared, FAILURE_WRITE, result);
            break;
        }
        child_phase(shared, WORKER_IDLE);
    }
finished:
    wipe(key, sizeof(key));
    free_encoder(&encoder);
    atomic_store(&shared->finished, 1);
    int failed = atomic_load(&shared->failure);
    munmap(shared, (size_t)metadata.st_size);
    return failed ? 1 : 0;
}
static void worker_error(struct CastStream *stream, int failure, int code)
{
    const char *action;
    switch (failure) {
    case FAILURE_KEY:
        action =
            "stream-key file changed or became inaccessible; finish editing it and start again";
        break;
    case FAILURE_ENCODER:
        action = "stream encoder initialization failed; check FFmpeg libx264/AAC support and "
                 "encoding settings";
        break;
    case FAILURE_CONNECT:
        action = "stream connection failed; check server address, stream-key file, network and TLS "
                 "trust";
        break;
    case FAILURE_WRITE:
        action = "stream network write failed; check ingest availability and upload capacity";
        break;
    case FAILURE_LAG:
        action = "stream exceeded its lag budget; reduce video bitrate/resolution or choose a "
                 "faster encoder preset";
        break;
    default:
        action = "stream worker failed; check local resources and start again";
        break;
    }
    /* av_strerror is keyed only by a numeric code, never FFmpeg's URL-bearing
     * log text. Authentication rejection is treated as permanent below. */
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code < 0 ? code : AVERROR(code), detail, sizeof(detail));
    snprintf(stream->status.error, sizeof(stream->status.error), "%s (%s)", action, detail);
}
static int spawn_worker(struct CastStream *stream)
{
    struct StreamShared *shared = stream->shared;
    atomic_store(&shared->failure, FAILURE_NONE);
    atomic_store(&shared->failure_code, 0);
    atomic_store(&shared->finished, 0);
    atomic_store(&shared->connected, 0);
    atomic_store(&shared->phase, WORKER_CONNECT);
    atomic_store(&shared->phase_started_ns, cast_now_ns());
    atomic_store(&shared->heartbeat, cast_now_ns());
    /* Retire all raw media at the reconnect boundary. The daemon composes a
     * fresh frame using its current pause/freeze/blur state on its next tick. */
    lock_media(shared);
    atomic_fetch_add(&shared->epoch, 1);
    clear_media_locked(shared);
    pthread_mutex_unlock(&shared->media_mutex);
    audio_stream_privacy(stream->audio);
    stream->audio_epoch = atomic_load(&shared->epoch);
    stream->audio_next_ns = cast_now_ns() - 30000000ULL;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    /* Every descriptor except the intentional shared mailbox is closed in the
     * exec child. No capture device, IPC socket, recording, or PipeWire fd leaks. */
    int duplicate = fcntl(stream->fd, F_DUPFD_CLOEXEC, STREAM_WORKER_FD + 1);
    if (duplicate < 0) {
        posix_spawn_file_actions_destroy(&actions);
        return errno;
    }
    posix_spawn_file_actions_adddup2(&actions, duplicate, STREAM_WORKER_FD);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, STREAM_WORKER_FD + 1);
    char *arguments[] = {"cast", "--internal-stream-worker", "3", NULL};
    int result = posix_spawn(&stream->child, "/proc/self/exe", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(duplicate);
    if (!result) {
        stream->status.generation++;
        stream->connect_started_ns = cast_now_ns();
        stream->child_connected = false;
    }
    return result;
}
static void publish_audio(struct CastStream *stream, uint64_t now)
{
    struct StreamShared *shared = stream->shared;
    uint64_t epoch = atomic_load(&shared->epoch);
    if (epoch != stream->audio_epoch || now > stream->audio_next_ns + 100000000ULL) {
        stream->audio_next_ns = now - 30000000ULL;
        stream->audio_epoch = epoch;
    }
    while (stream->audio_next_ns + 40000000ULL <= now) {
        float mix[STREAM_AUDIO_CHUNK * 2];
        uint64_t start = samples_at(stream->audio_next_ns);
        /* No reader consumes the ring: recording, virtual microphone, and this
         * lane read by timestamps with independent privacy acceptance windows. */
        audio_stream_read(stream->audio, stream->audio_next_ns, mix, STREAM_AUDIO_CHUNK);
        lock_media(shared);
        if (epoch == atomic_load(&shared->epoch) && !shared->silent) {
            if (!shared->audio_end || start > shared->audio_end) {
                shared->audio_begin = start;
            }
            for (int i = 0; i < STREAM_AUDIO_CHUNK; i++) {
                size_t target = ((start + (uint64_t)i) % STREAM_AUDIO_RING) * 2;
                shared->audio[target] = mix[i * 2];
                shared->audio[target + 1] = mix[i * 2 + 1];
            }
            shared->audio_end = start + STREAM_AUDIO_CHUNK;
            if (shared->audio_end - shared->audio_begin > STREAM_AUDIO_RING) {
                shared->audio_begin = shared->audio_end - STREAM_AUDIO_RING;
            }
        }
        pthread_mutex_unlock(&shared->media_mutex);
        wipe(mix, sizeof(mix));
        stream->audio_next_ns += STREAM_TICK_NS;
    }
}
static void retry_or_fail(struct CastStream *stream, int failure, int code, uint64_t now)
{
    worker_error(stream, failure, code);
    bool permanent = failure == FAILURE_KEY || failure == FAILURE_ENCODER ||
                     code == AVERROR(EACCES) || code == AVERROR(EPERM) ||
                     code == AVERROR_HTTP_UNAUTHORIZED || code == AVERROR_HTTP_FORBIDDEN;
    if (permanent ||
        stream->status.retry_attempt >= (unsigned)stream->cfg.stream.reconnect_attempts) {
        stream->active = false;
        stream->status.active = false;
        stream->status.state = STREAM_FAILED;
        atomic_store(&stream->shared->session, 0);
        lock_media(stream->shared);
        atomic_fetch_add(&stream->shared->epoch, 1);
        clear_media_locked(stream->shared);
        pthread_mutex_unlock(&stream->shared->media_mutex);
        audio_stream_privacy(stream->audio);
        return;
    }
    unsigned attempt = ++stream->status.retry_attempt;
    uint64_t delay = (uint64_t)stream->cfg.stream.reconnect_initial_ms;
    for (unsigned i = 1; i < attempt && delay < (uint64_t)stream->cfg.stream.reconnect_max_ms;
         i++) {
        delay *= 2;
    }
    if (delay > (uint64_t)stream->cfg.stream.reconnect_max_ms) {
        delay = (uint64_t)stream->cfg.stream.reconnect_max_ms;
    }
    stream->status.state = STREAM_RECONNECTING;
    stream->status.connected_started_ns = 0;
    stream->status.next_retry_ns = now + delay * 1000000ULL;
}
static void *supervise(void *opaque)
{
    struct CastStream *stream = opaque;
    pthread_mutex_lock(&stream->mutex);
    while (!stream->shutdown || stream->child > 0) {
        uint64_t now = cast_now_ns();
        if (stream->child > 0) {
            if (!stream->active || stream->shutdown) {
                /* SIGKILL is the cancellation boundary, including libc DNS.
                 * Reaping is asynchronous and never done by a control caller. */
                kill(stream->child, SIGKILL);
            } else {
                int phase = atomic_load(&stream->shared->phase);
                uint64_t began = atomic_load(&stream->shared->phase_started_ns);
                uint64_t budget =
                    (uint64_t)(phase == WORKER_CONNECT ? stream->cfg.stream.connect_timeout_ms
                                                       : stream->cfg.stream.write_timeout_ms) *
                    1000000ULL;
                if ((phase == WORKER_CONNECT || phase == WORKER_WRITE ||
                     phase == WORKER_ENCODING) &&
                    began && now - began > budget) {
                    atomic_store(&stream->shared->failure,
                                 phase == WORKER_CONNECT ? FAILURE_CONNECT : FAILURE_WRITE);
                    atomic_store(&stream->shared->failure_code, AVERROR(ETIMEDOUT));
                    kill(stream->child, SIGKILL);
                }
                if (!stream->child_connected && atomic_load(&stream->shared->connected)) {
                    stream->child_connected = true;
                    stream->status.state = STREAM_STREAMING;
                    stream->status.connected_started_ns = now;
                    stream->status.next_retry_ns = 0;
                    stream->status.error[0] = 0;
                }
            }
            int child_status;
            pid_t reaped = waitpid(stream->child, &child_status, WNOHANG);
            if (reaped == stream->child || (reaped < 0 && errno == ECHILD)) {
                stream->child = 0;
                stream->child_connected = false;
                if (stream->active && !stream->shutdown) {
                    retry_or_fail(stream, atomic_load(&stream->shared->failure),
                                  atomic_load(&stream->shared->failure_code), now);
                } else {
                    stream->status.state = STREAM_STOPPED;
                    stream->status.connected_started_ns = 0;
                }
                pthread_cond_broadcast(&stream->changed);
            }
        }
        if (stream->active && !stream->child && now >= stream->status.next_retry_ns) {
            int result = spawn_worker(stream);
            if (result) {
                retry_or_fail(stream, FAILURE_INTERNAL, result, now);
            }
        }
        if (stream->shared && now >= stream->bitrate_sample_ns + 1000000000ULL) {
            uint64_t bytes = atomic_load(&stream->shared->bytes_written);
            if (stream->bitrate_sample_ns && bytes >= stream->bitrate_sample_bytes) {
                stream->status.outgoing_bitrate_kbps =
                    stream->active ? (double)(bytes - stream->bitrate_sample_bytes) * 8.0 *
                                         1000000.0 / (double)(now - stream->bitrate_sample_ns)
                                   : 0;
            }
            stream->bitrate_sample_ns = now;
            stream->bitrate_sample_bytes = bytes;
        }
        if (stream->active && stream->shared) {
            /* This lock covers only a bounded in-memory PipeWire mix, never
             * encoder/network work, and pins the mapping across start/stop. */
            publish_audio(stream, now);
        }
        if (stream->shutdown && !stream->child) {
            break;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += (long)STREAM_TICK_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&stream->changed, &stream->mutex, &deadline);
    }
    pthread_mutex_unlock(&stream->mutex);
    return NULL;
}
CastStream *stream_open(CastAudio *audio)
{
    struct CastStream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        return NULL;
    }
    stream->fd = -1;
    stream->audio = audio;
    pthread_mutex_init(&stream->mutex, NULL);
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(&stream->changed, &attributes);
    pthread_condattr_destroy(&attributes);
    if (pthread_create(&stream->supervisor, NULL, supervise, stream)) {
        pthread_cond_destroy(&stream->changed);
        pthread_mutex_destroy(&stream->mutex);
        free(stream);
        return NULL;
    }
    stream->started = true;
    return stream;
}
static void free_shared(struct CastStream *stream)
{
    if (stream->shared) {
        pthread_mutex_destroy(&stream->shared->media_mutex);
        wipe(stream->shared, stream->shared_size);
        munmap(stream->shared, stream->shared_size);
        stream->shared = NULL;
    }
    if (stream->fd >= 0) {
        close(stream->fd);
        stream->fd = -1;
    }
}
void stream_close(CastStream *stream)
{
    if (!stream) {
        return;
    }
    pthread_mutex_lock(&stream->mutex);
    stream->active = false;
    stream->shutdown = true;
    if (stream->shared) {
        atomic_store(&stream->shared->session, 0);
    }
    if (stream->child > 0) {
        kill(stream->child, SIGKILL);
    }
    pthread_cond_broadcast(&stream->changed);
    pthread_mutex_unlock(&stream->mutex);
    if (stream->started) {
        pthread_join(stream->supervisor, NULL);
    }
    free_shared(stream);
    pthread_cond_destroy(&stream->changed);
    pthread_mutex_destroy(&stream->mutex);
    free(stream);
}
int stream_start(CastStream *stream, const Config *cfg, char *error, size_t size)
{
    if (config_validate(cfg, error, size)) {
        return -1;
    }
    if (!cfg->stream.server_url[0] || !cfg->stream.key_file[0]) {
        snprintf(error, size,
                 "configure stream.server_url and an absolute stream.key_file path before cast "
                 "stream start");
        return -1;
    }
    if (local_support(cfg, error, size) || stream_key_validate(cfg->stream.key_file, error, size)) {
        return -1;
    }
    pthread_mutex_lock(&stream->mutex);
    if (stream->active || stream->child) {
        snprintf(error, size,
                 "stream is already active or stopping; inspect cast stream status or stop it "
                 "before starting again");
        pthread_mutex_unlock(&stream->mutex);
        return -1;
    }
    free_shared(stream);
    size_t frame_size = (size_t)cfg->width * cfg->height * 4;
    size_t shared_size = sizeof(struct StreamShared) + frame_size;
    int fd = memfd_create("cast-stream-mailbox", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)shared_size)) {
        if (fd >= 0) {
            close(fd);
        }
        snprintf(error, size, "cannot allocate bounded streaming mailbox; check available memory");
        pthread_mutex_unlock(&stream->mutex);
        return -1;
    }
    struct StreamShared *shared =
        mmap(NULL, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) {
        close(fd);
        snprintf(error, size, "cannot map bounded streaming mailbox; check available memory");
        pthread_mutex_unlock(&stream->mutex);
        return -1;
    }
    memset(shared, 0, shared_size);
    shared->magic = STREAM_MAGIC;
    shared->size = shared_size;
    shared->frame_size = frame_size;
    shared->cfg = *cfg;
    shared->silent = true;
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&shared->media_mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    atomic_init(&shared->epoch, 1);
    atomic_init(&shared->session, stream->status.generation + 1);
    fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    stream->fd = fd;
    stream->shared = shared;
    stream->shared_size = shared_size;
    stream->cfg = *cfg;
    uint64_t generation = stream->status.generation + 1;
    memset(&stream->status, 0, sizeof(stream->status));
    stream->status.generation = generation;
    stream->status.state = STREAM_CONNECTING;
    stream->status.active = true;
    stream->status.session_started_ns = cast_now_ns();
    stream->active = true;
    stream->bitrate_sample_ns = cast_now_ns();
    stream->bitrate_sample_bytes = 0;
    pthread_cond_broadcast(&stream->changed);
    pthread_mutex_unlock(&stream->mutex);
    return 0;
}
int stream_stop(CastStream *stream, char *error, size_t size)
{
    (void)error;
    (void)size;
    pthread_mutex_lock(&stream->mutex);
    stream->active = false;
    stream->status.active = false;
    stream->status.generation++;
    stream->status.next_retry_ns = 0;
    stream->status.outgoing_bitrate_kbps = 0;
    stream->status.state = stream->child ? STREAM_STOPPING : STREAM_STOPPED;
    if (stream->shared) {
        lock_media(stream->shared);
        atomic_store(&stream->shared->session, 0);
        atomic_fetch_add(&stream->shared->epoch, 1);
        clear_media_locked(stream->shared);
        pthread_mutex_unlock(&stream->shared->media_mutex);
    }
    if (stream->child > 0) {
        kill(stream->child, SIGKILL);
    }
    pthread_cond_broadcast(&stream->changed);
    pthread_mutex_unlock(&stream->mutex);
    audio_stream_privacy(stream->audio);
    return 0;
}
int stream_frame(CastStream *stream, const Frame *frame, bool silent, char *error, size_t size)
{
    pthread_mutex_lock(&stream->mutex);
    if (!stream->active) {
        pthread_mutex_unlock(&stream->mutex);
        return 0;
    }
    struct StreamShared *shared = stream->shared;
    if (frame->width != stream->cfg.width || frame->height != stream->cfg.height || !frame->data) {
        snprintf(
            error, size,
            "streaming frame dimensions changed; stop streaming before changing output resolution");
        pthread_mutex_unlock(&stream->mutex);
        return -1;
    }
    lock_media(shared);
    if (shared->frame_sequence != shared->consumed_sequence) {
        atomic_fetch_add(&shared->dropped_frames, 1);
    }
    if (silent != shared->silent) {
        atomic_fetch_add(&shared->epoch, 1);
        clear_media_locked(shared);
    }
    shared->silent = silent;
    for (int y = 0; y < frame->height; y++) {
        memcpy(shared->frame + (size_t)y * frame->width * 4,
               frame->data + (size_t)y * frame->stride, (size_t)frame->width * 4);
    }
    shared->frame_epoch = atomic_load(&shared->epoch);
    shared->frame_ns = cast_now_ns();
    shared->frame_sequence++;
    shared->have_frame = true;
    pthread_mutex_unlock(&shared->media_mutex);
    pthread_mutex_unlock(&stream->mutex);
    return 0;
}
void stream_privacy(CastStream *stream, bool silent)
{
    pthread_mutex_lock(&stream->mutex);
    if (stream->shared) {
        lock_media(stream->shared);
        stream->shared->silent = silent;
        atomic_fetch_add(&stream->shared->epoch, 1);
        clear_media_locked(stream->shared);
        pthread_mutex_unlock(&stream->shared->media_mutex);
    }
    pthread_mutex_unlock(&stream->mutex);
    audio_stream_privacy(stream->audio);
}
void stream_barrier(CastStream *stream)
{
    pthread_mutex_lock(&stream->mutex);
    if (stream->shared) {
        lock_media(stream->shared);
        atomic_fetch_add(&stream->shared->epoch, 1);
        clear_media_locked(stream->shared);
        pthread_mutex_unlock(&stream->shared->media_mutex);
    }
    pthread_mutex_unlock(&stream->mutex);
    audio_stream_privacy(stream->audio);
}
void stream_status(CastStream *stream, StreamSnapshot *status)
{
    pthread_mutex_lock(&stream->mutex);
    *status = stream->status;
    if (stream->shared) {
        status->bytes_written = atomic_load(&stream->shared->bytes_written);
        status->video_frames = atomic_load(&stream->shared->video_frames);
        status->audio_samples = atomic_load(&stream->shared->audio_samples);
        status->dropped_frames = atomic_load(&stream->shared->dropped_frames);
        lock_media(stream->shared);
        status->queue_depth =
            stream->shared->frame_sequence != stream->shared->consumed_sequence ? 1 : 0;
        pthread_mutex_unlock(&stream->shared->media_mutex);
    }
    pthread_mutex_unlock(&stream->mutex);
}
void stream_doctor(const Config *cfg, char *output, size_t size)
{
    char error[CAST_ERR] = "";
    int support = local_support(cfg, error, sizeof(error));
    size_t used = strlen(output);
    if (used >= size) {
        return;
    }
    snprintf(output + used, size - used,
             "Streaming: %s%s; software H.264/AAC, FLV RTMP/RTMPS, TLS verification enabled; no "
             "network probe.\n",
             support ? "unavailable: " : "local codec/muxer/protocol support available",
             support ? error : "");
    used = strlen(output);
    if (used >= size) {
        return;
    }
    int valid = stream_key_validate(cfg->stream.key_file, error, sizeof(error));
    snprintf(output + used, size - used, "Stream-key file: %s%s (contents never displayed).\n",
             valid ? "invalid: " : "accessible, owned regular mode-600 file", valid ? error : "");
}
#ifdef CAST_TEST
void stream_test_hold(CastStream *stream, int milliseconds)
{
    pthread_mutex_lock(&stream->mutex);
    if (stream->shared) {
        atomic_store(&stream->shared->hold_ms, milliseconds);
    }
    pthread_mutex_unlock(&stream->mutex);
}
void stream_test_stage(CastStream *stream, int stage)
{
    pthread_mutex_lock(&stream->mutex);
    if (stream->shared) {
        atomic_store(&stream->shared->hold_stage, stage);
    }
    pthread_mutex_unlock(&stream->mutex);
}
#endif
