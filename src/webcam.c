#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define CAMERA_BUFFERS 4
#define CAMERA_POLL_MS 50
struct CastCamera {
    int fd, w, h, stride;
    uint32_t format;
    bool synthetic, streaming, failed, stop, worker_started;
    pthread_t worker;
    pthread_mutex_t lock;
    uint64_t epoch, accept_after_ns;
    char failure[CAST_ERR];
    struct {
        void *ptr;
        size_t len;
        uint64_t epoch;
    } bufs[CAMERA_BUFFERS];
    unsigned count;
    AVCodecContext *decoder;
    AVFrame *decoded;
    struct SwsContext *scale;
    Frame cached, working;
    char path[PATH_MAX];
#ifdef CAST_TEST
    const struct CameraTest *test;
#endif
};
struct CastOutput {
    int fd, w, h, stride, size;
    uint32_t format;
    uint8_t *pixels;
    struct SwsContext *scale;
};
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int error(char *e, size_t n, const char *action, const char *path)
{
    snprintf(e, n, "%s %s: %s", action, path, strerror(errno));
    return -1;
}
static uint32_t caps(const struct v4l2_capability *c)
{
    return c->capabilities & V4L2_CAP_DEVICE_CAPS ? c->device_caps : c->capabilities;
}
static enum AVPixelFormat raw_format(uint32_t f)
{
    switch (f) {
    case V4L2_PIX_FMT_YUYV:
        return AV_PIX_FMT_YUYV422;
    case V4L2_PIX_FMT_UYVY:
        return AV_PIX_FMT_UYVY422;
    case V4L2_PIX_FMT_NV12:
        return AV_PIX_FMT_NV12;
    case V4L2_PIX_FMT_YUV420:
        return AV_PIX_FMT_YUV420P;
    case V4L2_PIX_FMT_RGB24:
        return AV_PIX_FMT_RGB24;
    case V4L2_PIX_FMT_BGR24:
        return AV_PIX_FMT_BGR24;
    case V4L2_PIX_FMT_BGR32:
        return AV_PIX_FMT_BGRA;
    default:
        return AV_PIX_FMT_NONE;
    }
}
static int camera_ioctl(CastCamera *c, unsigned long request, void *argument)
{
#ifdef CAST_TEST
    if (c->test) {
        return c->test->ioctl(c->test->data, request, argument);
    }
#endif
    return xioctl(c->fd, request, argument);
}
static CastCamera *camera_alloc(char *e, size_t n)
{
    CastCamera *c = calloc(1, sizeof(*c));
    if (!c) {
        snprintf(e, n, "camera allocation failed");
        return NULL;
    }
    c->fd = -1;
    int r = pthread_mutex_init(&c->lock, NULL);
    if (r) {
        snprintf(e, n, "camera mutex: %s", strerror(r));
        free(c);
        return NULL;
    }
    return c;
}
static void *camera_worker(void *);
static int camera_start(CastCamera *c, char *e, size_t n)
{
    int r = pthread_create(&c->worker, NULL, camera_worker, c);
    if (r) {
        snprintf(e, n, "camera worker: %s", strerror(r));
        return -1;
    }
    c->worker_started = true;
    return 0;
}
void camera_close(CastCamera *c)
{
    if (!c) {
        return;
    }
    if (c->worker_started) {
        pthread_mutex_lock(&c->lock);
        c->stop = true;
        pthread_mutex_unlock(&c->lock);
        /* The worker owns device ioctls; its poll wakes within 50 ms. */
        pthread_join(c->worker, NULL);
    }
    if (c->streaming) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        camera_ioctl(c, VIDIOC_STREAMOFF, &t);
    }
    for (unsigned i = 0; i < c->count; i++) {
#ifdef CAST_TEST
        if (c->test) {
            break;
        }
#endif
        if (c->bufs[i].ptr && c->bufs[i].ptr != MAP_FAILED) {
            munmap(c->bufs[i].ptr, c->bufs[i].len);
        }
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    avcodec_free_context(&c->decoder);
    av_frame_free(&c->decoded);
    sws_freeContext(c->scale);
    frame_free(&c->cached);
    frame_free(&c->working);
    pthread_mutex_destroy(&c->lock);
    free(c);
}
CastCamera *camera_open(const Config *cfg, char *e, size_t n)
{
    CastCamera *c = camera_alloc(e, n);
    if (!c) {
        return NULL;
    }
    snprintf(c->path, sizeof(c->path), "%s", cfg->camera_device);
    if (!strcmp(cfg->camera_device, "synthetic")) {
        c->synthetic = true;
        c->w = 640;
        c->h = 360;
        return c;
    }
    c->fd = open(cfg->camera_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->fd < 0) {
        error(e, n, "open webcam", c->path);
        goto fail;
    }
    struct v4l2_capability cap = {0};
    if (xioctl(c->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        error(e, n, "query webcam", c->path);
        goto fail;
    }
    if (!(caps(&cap) & V4L2_CAP_VIDEO_CAPTURE) || !(caps(&cap) & V4L2_CAP_STREAMING)) {
        snprintf(e, n, "%s is not a streaming single-plane V4L2 capture device", c->path);
        goto fail;
    }
    const uint32_t preferred[] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV,   V4L2_PIX_FMT_UYVY,
                                  V4L2_PIX_FMT_NV12,  V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_RGB24,
                                  V4L2_PIX_FMT_BGR24};
    for (size_t k = 0; k < sizeof(preferred) / sizeof(*preferred) && !c->format; k++) {
        if (preferred[k] == V4L2_PIX_FMT_MJPEG && !avcodec_find_decoder(AV_CODEC_ID_MJPEG)) {
            continue;
        }
        for (unsigned j = 0;; j++) {
            struct v4l2_fmtdesc d = {.index = j, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
            if (xioctl(c->fd, VIDIOC_ENUM_FMT, &d) < 0) {
                break;
            }
            if (d.pixelformat != preferred[k]) {
                continue;
            }
            struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
            f.fmt.pix.width = (unsigned)(cfg->width > 1920 ? 1920 : cfg->width);
            f.fmt.pix.height = (unsigned)(cfg->height > 1080 ? 1080 : cfg->height);
            f.fmt.pix.pixelformat = preferred[k];
            f.fmt.pix.field = V4L2_FIELD_ANY;
            if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.pixelformat != preferred[k] ||
                !f.fmt.pix.width || !f.fmt.pix.height || f.fmt.pix.width > 8192 ||
                f.fmt.pix.height > 8192) {
                continue;
            }
            c->format = f.fmt.pix.pixelformat;
            c->w = (int)f.fmt.pix.width;
            c->h = (int)f.fmt.pix.height;
            c->stride = (int)f.fmt.pix.bytesperline;
            break;
        }
    }
    if (!c->format) {
        snprintf(e, n, "%s has no compatible MJPEG/YUYV/UYVY/NV12/YUV420/RGB24 webcam format",
                 c->path);
        goto fail;
    }
    struct v4l2_streamparm p = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    p.parm.capture.timeperframe.numerator = 1;
    p.parm.capture.timeperframe.denominator = (unsigned)cfg->fps;
    xioctl(c->fd, VIDIOC_S_PARM, &p);
    if (c->format == V4L2_PIX_FMT_MJPEG) {
        c->decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_MJPEG));
        c->decoded = av_frame_alloc();
        if (!c->decoder || !c->decoded || avcodec_open2(c->decoder, c->decoder->codec, NULL) < 0) {
            snprintf(e, n, "cannot initialize FFmpeg MJPEG decoder");
            goto fail;
        }
    }
    struct v4l2_requestbuffers req = {
        .count = CAMERA_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || !req.count) {
        error(e, n, "allocate mmap webcam buffers", c->path);
        goto fail;
    }
    c->count = req.count > CAMERA_BUFFERS ? CAMERA_BUFFERS : req.count;
    for (unsigned i = 0; i < c->count; i++) {
        struct v4l2_buffer b = {
            .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
            error(e, n, "query mmap webcam buffer", c->path);
            goto fail;
        }
        c->bufs[i].len = b.length;
        c->bufs[i].ptr =
            mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
        if (c->bufs[i].ptr == MAP_FAILED || xioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
            error(e, n, "queue mmap webcam buffer", c->path);
            goto fail;
        }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
        error(e, n, "start webcam", c->path);
        goto fail;
    }
    c->streaming = true;
    if (camera_start(c, e, n) < 0) {
        goto fail;
    }
    return c;
fail:
    camera_close(c);
    return NULL;
}
/* JPEG's legacy YUVJ formats describe the same planes as ordinary YUV,
 * with full-range samples. Give swscale the range separately. */
static struct SwsContext *camera_scaler(struct SwsContext *scale, int w, int h,
                                        enum AVPixelFormat format, enum AVColorRange range,
                                        enum AVColorSpace colorspace)
{
    int full = range == AVCOL_RANGE_JPEG;
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
        format = AV_PIX_FMT_YUV420P;
        full = 1;
        break;
    case AV_PIX_FMT_YUVJ422P:
        format = AV_PIX_FMT_YUV422P;
        full = 1;
        break;
    case AV_PIX_FMT_YUVJ444P:
        format = AV_PIX_FMT_YUV444P;
        full = 1;
        break;
    case AV_PIX_FMT_YUVJ440P:
        format = AV_PIX_FMT_YUV440P;
        full = 1;
        break;
    default:
        break;
    }
    int matrix = SWS_CS_DEFAULT;
    switch (colorspace) {
    case AVCOL_SPC_BT709:
        matrix = SWS_CS_ITU709;
        break;
    case AVCOL_SPC_FCC:
        matrix = SWS_CS_FCC;
        break;
    case AVCOL_SPC_SMPTE240M:
        matrix = SWS_CS_SMPTE240M;
        break;
    case AVCOL_SPC_BT2020_NCL:
        matrix = SWS_CS_BT2020;
        break;
    default:
        break;
    }
    scale = sws_getCachedContext(scale, w, h, format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL,
                                 NULL, NULL);
    const int *coefficients = sws_getCoefficients(matrix);
    if (scale && sws_setColorspaceDetails(scale, coefficients, full, coefficients, 1, 0, 1 << 16,
                                          1 << 16) < 0) {
        sws_freeContext(scale);
        return NULL;
    }
    return scale;
}
#ifdef CAST_TEST
struct SwsContext *camera_test_scaler(struct SwsContext *scale, const struct AVFrame *frame)
{
    return camera_scaler(scale, frame->width, frame->height, frame->format, frame->color_range,
                         frame->colorspace);
}
#endif
static int decode(CastCamera *c, const void *data, size_t bytes, char *e, size_t n)
{
    const uint8_t *src[4] = {data, NULL, NULL, NULL};
    int strides[4] = {c->stride, 0, 0, 0};
    int w = c->w, h = c->h;
    enum AVPixelFormat f = raw_format(c->format);
    enum AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    enum AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
    if (c->decoder) {
        AVPacket *p = av_packet_alloc();
        if (!p || bytes > INT_MAX || av_new_packet(p, (int)bytes) < 0) {
            av_packet_free(&p);
            snprintf(e, n, "MJPEG packet allocation failed");
            return -1;
        }
        memcpy(p->data, data, bytes);
        int r = avcodec_send_packet(c->decoder, p);
        av_packet_free(&p);
        if (r < 0 || avcodec_receive_frame(c->decoder, c->decoded) < 0) {
            snprintf(e, n, "invalid MJPEG webcam frame");
            return -1;
        }
        w = c->decoded->width;
        h = c->decoded->height;
        f = c->decoded->format;
        range = c->decoded->color_range;
        colorspace = c->decoded->colorspace;
        for (int i = 0; i < 4; i++) {
            src[i] = c->decoded->data[i];
            strides[i] = c->decoded->linesize[i];
        }
    } else {
        int tight = av_image_get_linesize(f, w, 0);
        if (strides[0] < tight) {
            strides[0] = tight;
        }
        size_t needed = (size_t)strides[0] * h;
        if (f == AV_PIX_FMT_NV12) {
            src[1] = (const uint8_t *)data + needed;
            strides[1] = strides[0];
            needed += (size_t)strides[1] * ((h + 1) / 2);
        }
        if (f == AV_PIX_FMT_YUV420P) {
            src[1] = (const uint8_t *)data + needed;
            strides[1] = (strides[0] + 1) / 2;
            src[2] = src[1] + (size_t)strides[1] * ((h + 1) / 2);
            strides[2] = strides[1];
            needed += (size_t)strides[1] * ((h + 1) / 2) * 2;
        }
        if (bytes < needed) {
            snprintf(e, n, "short raw webcam frame");
            return -1;
        }
    }
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || frame_alloc(&c->working, w, h) < 0) {
        snprintf(e, n, "invalid webcam size or frame allocation failed");
        return -1;
    }
    c->scale = camera_scaler(c->scale, w, h, f, range, colorspace);
    if (!c->scale) {
        snprintf(e, n, "webcam conversion unavailable");
        return -1;
    }
    uint8_t *dst[4] = {c->working.data, NULL, NULL, NULL};
    int ds[4] = {c->working.stride, 0, 0, 0};
    if (sws_scale(c->scale, src, strides, 0, h, dst, ds) != h) {
        snprintf(e, n, "webcam conversion failed");
        return -1;
    }
    c->working.ts_ns = cast_now_ns();
    return 0;
}
static void camera_error(CastCamera *c, const char *message, bool fatal, uint64_t epoch)
{
    pthread_mutex_lock(&c->lock);
    if (fatal || epoch == c->epoch) {
        c->failed = fatal;
        snprintf(c->failure, sizeof(c->failure), "%.*s", (int)sizeof(c->failure) - 1, message);
        frame_free(&c->cached);
    }
    pthread_mutex_unlock(&c->lock);
}
static int camera_requeue(CastCamera *c, struct v4l2_buffer *buffer, uint64_t epoch)
{
    if (camera_ioctl(c, VIDIOC_QBUF, buffer) < 0) {
        return -1;
    }
    c->bufs[buffer->index].epoch = epoch;
    return 0;
}
/* Only the capture worker dequeues/requeues buffers after startup. */
static int camera_drain(CastCamera *c, uint64_t epoch, char *e, size_t n)
{
    for (unsigned i = 0; i < c->count; ++i) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (camera_ioctl(c, VIDIOC_DQBUF, &b) < 0) {
            return errno == EAGAIN ? 0 : error(e, n, "drain webcam", c->path);
        }
        if (b.index >= c->count) {
            snprintf(e, n, "invalid V4L2 webcam buffer");
            return -1;
        }
        if (camera_requeue(c, &b, epoch) < 0) {
            return error(e, n, "requeue webcam", c->path);
        }
    }
    return 0;
}
static uint64_t camera_timestamp(const struct v4l2_buffer *b, bool *monotonic)
{
    uint64_t now = cast_now_ns();
    *monotonic = false;
    if ((b->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
        b->timestamp.tv_sec >= 0 && (uint64_t)b->timestamp.tv_sec <= now / 1000000000 &&
        b->timestamp.tv_usec >= 0 && b->timestamp.tv_usec < 1000000) {
        uint64_t stamp =
            (uint64_t)b->timestamp.tv_sec * 1000000000 + (uint64_t)b->timestamp.tv_usec * 1000;
        if (stamp <= now) {
            *monotonic = true;
            return stamp;
        }
    }
    return now;
}
static void *camera_worker(void *data)
{
    CastCamera *c = data;
    uint64_t seen_epoch = 0;
    char message[PATH_MAX + CAST_ERR];
    for (;;) {
        pthread_mutex_lock(&c->lock);
        bool stop = c->stop;
        uint64_t epoch = c->epoch, after = c->accept_after_ns;
        pthread_mutex_unlock(&c->lock);
        if (stop) {
            break;
        }
        if (epoch != seen_epoch) {
            /* Unknown driver timestamps require a queue drain at each boundary.
             * Monotonic timestamps additionally reject late pre-boundary captures. */
            if (camera_drain(c, epoch, message, sizeof(message)) < 0) {
                camera_error(c, message, true, epoch);
                break;
            }
            seen_epoch = epoch;
            continue;
        }
        struct pollfd p = {.fd = c->fd, .events = POLLIN};
        int r = poll(&p, 1, CAMERA_POLL_MS);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            snprintf(message, sizeof(message), "webcam %s disappeared or stopped", c->path);
            camera_error(c, message, true, epoch);
            break;
        }
        if (!r || !(p.revents & POLLIN)) {
            continue;
        }
        pthread_mutex_lock(&c->lock);
        bool current = !c->stop && epoch == c->epoch;
        pthread_mutex_unlock(&c->lock);
        if (!current) {
            continue;
        }
        /* Hold only the newest ready frame, and convert it outside the lock. */
        struct v4l2_buffer newest = {0};
        uint64_t captured = 0;
        bool have = false, fatal = false, current_buffer = false;
        for (unsigned i = 0; i < c->count; ++i) {
            struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    .memory = V4L2_MEMORY_MMAP};
            if (camera_ioctl(c, VIDIOC_DQBUF, &b) < 0) {
                if (errno != EAGAIN) {
                    error(message, sizeof(message), "read webcam", c->path);
                    fatal = true;
                }
                break;
            }
            if (b.index >= c->count || b.bytesused > c->bufs[b.index].len) {
                snprintf(message, sizeof(message), "invalid V4L2 webcam buffer");
                fatal = true;
                break;
            }
            if (have && camera_requeue(c, &newest, epoch) < 0) {
                error(message, sizeof(message), "requeue webcam", c->path);
                fatal = true;
                break;
            }
            newest = b;
            have = true;
            bool monotonic;
            captured = camera_timestamp(&b, &monotonic);
            /* An old exposure can finish after the boundary drain. Without a
             * usable timestamp it is safe only after a current-epoch QBUF. */
            current_buffer = monotonic || c->bufs[b.index].epoch == epoch;
        }
        if (fatal) {
            if (have) {
                camera_requeue(c, &newest, epoch);
            }
            camera_error(c, message, true, epoch);
            break;
        }
        if (!have) {
            continue;
        }
        int ok =
            !current_buffer || captured < after
                ? 1
                : decode(c, c->bufs[newest.index].ptr, newest.bytesused, message, sizeof(message));
        if (camera_requeue(c, &newest, epoch) < 0) {
            error(message, sizeof(message), "requeue webcam", c->path);
            camera_error(c, message, true, epoch);
            break;
        }
        if (ok < 0) {
            camera_error(c, message, false, epoch);
            continue;
        }
        if (ok > 0) {
            continue;
        }
#ifdef CAST_TEST
        if (c->test && c->test->decoded) {
            c->test->decoded(c->test->data);
        }
#endif
        c->working.ts_ns = captured;
        pthread_mutex_lock(&c->lock);
        /* A barrier may arrive while poll, dequeue, or decode is in progress. */
        if (!c->stop && epoch == c->epoch && captured >= c->accept_after_ns) {
            Frame previous = c->cached;
            c->cached = c->working;
            c->working = previous;
            c->failure[0] = 0;
        }
        pthread_mutex_unlock(&c->lock);
    }
    if (c->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        camera_ioctl(c, VIDIOC_STREAMOFF, &type);
        c->streaming = false;
    }
    return NULL;
}
bool camera_failed(CastCamera *c)
{
    if (!c) {
        return false;
    }
    pthread_mutex_lock(&c->lock);
    bool failed = c->failed;
    pthread_mutex_unlock(&c->lock);
    return failed;
}
void camera_barrier(CastCamera *c)
{
    if (!c) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    c->accept_after_ns = cast_now_ns();
    ++c->epoch;
    frame_free(&c->cached);
    if (!c->failed) {
        c->failure[0] = 0;
    }
    pthread_mutex_unlock(&c->lock);
}
int camera_frame(CastCamera *c, Frame *out, char *e, size_t n)
{
    if (!c) {
        snprintf(e, n, "camera unavailable");
        return -1;
    }
    if (c->synthetic) {
        if (frame_alloc(out, c->w, c->h) < 0) {
            snprintf(e, n, "synthetic camera allocation failed");
            return -1;
        }
        uint64_t now = cast_now_ns();
        unsigned phase = (unsigned)(now / 10000000);
        for (int y = 0; y < out->height; y++) {
            for (int x = 0; x < out->width; x++) {
                uint8_t *p = out->data + (size_t)y * out->stride + x * 4;
                p[0] = (uint8_t)(x + phase);
                p[1] = (uint8_t)y;
                p[2] = (uint8_t)(((x / 32) ^ (y / 32)) & 1 ? 200 : 40);
                p[3] = 255;
            }
        }
        out->ts_ns = now;
        return 0;
    }
    pthread_mutex_lock(&c->lock);
    int result = -1;
    if (c->failed || c->failure[0]) {
        snprintf(e, n, "%s", c->failure);
    } else if (!c->cached.data || cast_now_ns() - c->cached.ts_ns > 500000000) {
        snprintf(e, n, "waiting for fresh frames from webcam %s", c->path);
    } else if (frame_copy(out, &c->cached) < 0) {
        snprintf(e, n, "webcam frame allocation failed");
    } else {
        result = 0;
    }
    pthread_mutex_unlock(&c->lock);
    return result;
}
#ifdef CAST_TEST
CastCamera *camera_test_open(const struct CameraTest *test, char *e, size_t n)
{
    CastCamera *c = camera_alloc(e, n);
    if (!c) {
        return NULL;
    }
    c->test = test;
    c->fd = dup(test->fd);
    c->w = test->width;
    c->h = test->height;
    c->stride = test->stride;
    c->format = test->format;
    c->count = test->count;
    snprintf(c->path, sizeof(c->path), "test webcam");
    if (c->fd < 0 || c->count < 1 || c->count > CAMERA_BUFFERS) {
        snprintf(e, n, "invalid test webcam");
        camera_close(c);
        return NULL;
    }
    for (unsigned i = 0; i < c->count; ++i) {
        c->bufs[i].ptr = test->buffers[i];
        c->bufs[i].len = test->lengths[i];
    }
    c->streaming = true;
    if (camera_start(c, e, n) < 0) {
        camera_close(c);
        return NULL;
    }
    return c;
}
#endif
void output_close(CastOutput *o)
{
    if (!o) {
        return;
    }
    if (o->fd >= 0) {
        close(o->fd);
    }
    sws_freeContext(o->scale);
    free(o->pixels);
    free(o);
}
#ifdef CAST_TEST
static int (*test_output_ioctl)(int, unsigned long, void *);
#endif
static int output_ioctl(int fd, unsigned long request, void *argument)
{
#ifdef CAST_TEST
    if (test_output_ioctl) {
        return test_output_ioctl(fd, request, argument);
    }
#endif
    return xioctl(fd, request, argument);
}
CastOutput *output_open(const Config *c, char *e, size_t n)
{
    CastOutput *o = calloc(1, sizeof(*o));
    if (!o) {
        snprintf(e, n, "output allocation failed");
        return NULL;
    }
    o->fd = -1;
    o->w = c->width;
    o->h = c->height;
    if (!c->live_enabled || !strcmp(c->output_device, "none")) {
        return o;
    }
    o->fd = open(c->output_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (o->fd < 0) {
        error(e, n, "open existing v4l2loopback output", c->output_device);
        goto fail;
    }
    struct v4l2_capability cap = {0};
    if (output_ioctl(o->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        error(e, n, "query virtual output", c->output_device);
        goto fail;
    }
    /* exclusive_caps is dynamic. A known loopback driver may advertise
     * CAPTURE until its output format/stream tokens are available again;
     * negotiate OUTPUT to establish whether this opener can be a producer. */
    bool loopback = !strncmp((const char *)cap.driver, "v4l2 loopback", sizeof(cap.driver));
    if (!(caps(&cap) & V4L2_CAP_VIDEO_OUTPUT) && !loopback) {
        snprintf(e, n, "%s is not a V4L2 video output; create v4l2loopback with exclusive_caps=1",
                 c->output_device);
        goto fail;
    }
    const uint32_t formats[] = {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR32};
    for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); i++) {
        struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
        f.fmt.pix.width = (unsigned)c->width;
        f.fmt.pix.height = (unsigned)c->height;
        f.fmt.pix.pixelformat = formats[i];
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (output_ioctl(o->fd, VIDIOC_S_FMT, &f) < 0) {
            if (errno == EBUSY) {
                snprintf(e, n,
                         "%s output is busy; stop the other producer or clients holding its format",
                         c->output_device);
                goto fail;
            }
            if (errno != EINVAL && errno != ENOTTY) {
                error(e, n, "configure virtual output", c->output_device);
                goto fail;
            }
            continue;
        }
        if (f.fmt.pix.pixelformat != formats[i] || f.fmt.pix.width != (unsigned)c->width ||
            f.fmt.pix.height != (unsigned)c->height) {
            continue;
        }
        o->format = formats[i];
        o->stride = (int)f.fmt.pix.bytesperline;
        o->size = (int)f.fmt.pix.sizeimage;
        break;
    }
    if (!o->format) {
        snprintf(e, n, "%s cannot accept configured %dx%d YUYV/RGB24/BGR32 output%s",
                 c->output_device, c->width, c->height,
                 loopback ? "; check for another producer or a locked format" : "");
        goto fail;
    }
    int tight = av_image_get_linesize(raw_format(o->format), o->w, 0);
    if (o->stride < tight) {
        o->stride = tight;
    }
    if (o->size < o->stride * o->h) {
        o->size = o->stride * o->h;
    }
    o->pixels = calloc(1, (size_t)o->size);
    if (!o->pixels) {
        snprintf(e, n, "virtual output allocation failed");
        goto fail;
    }
    struct v4l2_streamparm p = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    p.parm.output.timeperframe.numerator = 1;
    p.parm.output.timeperframe.denominator = (unsigned)c->fps;
    output_ioctl(o->fd, VIDIOC_S_PARM, &p);
    return o;
fail:
    output_close(o);
    return NULL;
}
#ifdef CAST_TEST
CastOutput *output_test_open(const Config *cfg, int (*ioctl_fn)(int, unsigned long, void *),
                             char *error, size_t size)
{
    test_output_ioctl = ioctl_fn;
    CastOutput *output = output_open(cfg, error, size);
    test_output_ioctl = NULL;
    return output;
}
#endif
int output_frame(CastOutput *o, const Frame *f, char *e, size_t n)
{
    if (!o || o->fd < 0) {
        return 0;
    }
    if (!f || !f->data || f->width != o->w || f->height != o->h) {
        snprintf(e, n, "virtual frame dimensions do not match configured output");
        return -1;
    }
    o->scale = sws_getCachedContext(o->scale, f->width, f->height, AV_PIX_FMT_RGBA, o->w, o->h,
                                    raw_format(o->format), SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!o->scale) {
        snprintf(e, n, "virtual camera pixel conversion unavailable");
        return -1;
    }
    const uint8_t *src[4] = {f->data, NULL, NULL, NULL};
    int ss[4] = {f->stride, 0, 0, 0};
    uint8_t *dst[4] = {o->pixels, NULL, NULL, NULL};
    int ds[4] = {o->stride, 0, 0, 0};
    if (sws_scale(o->scale, src, ss, 0, f->height, dst, ds) != o->h) {
        snprintf(e, n, "virtual camera conversion failed");
        return -1;
    }
    ssize_t r;
    do {
        r = write(o->fd, o->pixels, (size_t)o->size);
    } while (r < 0 && errno == EINTR);
    if (r < 0 && errno == EAGAIN) {
        snprintf(e, n, "virtual camera busy; frame dropped");
        return 1;
    }
    if (r != o->size) {
        if (r < 0) {
            return error(e, n, "write virtual camera", "");
        }
        snprintf(e, n, "virtual camera accepted a short frame");
        return -1;
    }
    return 0;
}
void camera_list(char *out, size_t n)
{
    size_t used = 0;
    if (n) {
        out[0] = 0;
    }
    for (int i = 0; i < 256; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct v4l2_capability c = {0};
        if (xioctl(fd, VIDIOC_QUERYCAP, &c) == 0 && (caps(&c) & V4L2_CAP_VIDEO_CAPTURE)) {
            int z = snprintf(out + used, n > used ? n - used : 0, "%s %s formats:", p, c.card);
            if (z > 0) {
                used += (size_t)z;
            }
            for (unsigned j = 0;; j++) {
                struct v4l2_fmtdesc f = {.index = j, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
                if (xioctl(fd, VIDIOC_ENUM_FMT, &f) < 0) {
                    break;
                }
                char four[5] = {(char)f.pixelformat, (char)(f.pixelformat >> 8),
                                (char)(f.pixelformat >> 16), (char)(f.pixelformat >> 24), 0};
                if (used < n) {
                    z = snprintf(out + used, n - used, " %s", four);
                    if (z > 0) {
                        used += (size_t)z;
                    }
                }
            }
            if (used < n) {
                out[used++] = '\n';
                if (used < n) {
                    out[used] = 0;
                }
            }
        }
        close(fd);
        if (used >= n) {
            break;
        }
    }
    if (!used) {
        snprintf(
            out, n,
            "No accessible V4L2 capture devices. Check device ACLs with getfacl /dev/videoN.\n");
    }
}
void camera_doctor(const Config *c, char *out, size_t n)
{
    char devices[2048];
    camera_list(devices, sizeof(devices));
    snprintf(out, n, "Webcam formats:\n%s", devices);
    size_t used = strlen(out);
    int fd = open(c->output_device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct v4l2_capability cap = {0};
    if (!strcmp(c->output_device, "none") || !c->live_enabled) {
        snprintf(out + used, n - used,
                 "Virtual output: disabled (test-only 'none' discards video).\n");
    } else if (fd < 0) {
        snprintf(
            out + used, n - used,
            "Virtual output %s: %s. Arch: install v4l2loopback-dkms + matching kernel headers; "
            "sudo modprobe v4l2loopback video_nr=10 card_label=cast exclusive_caps=1.\n",
            c->output_device, strerror(errno));
    } else if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
               !(caps(&cap) & (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_CAPTURE))) {
        snprintf(out + used, n - used, "Virtual output %s: incompatible V4L2 capabilities.\n",
                 c->output_device);
    } else {
        snprintf(out + used, n - used,
                 "Virtual output %s: %s; producer must claim OUTPUT capability before "
                 "exclusive_caps consumers see CAPTURE.\n",
                 c->output_device, cap.card);
    }
    if (fd >= 0) {
        close(fd);
    }
}
