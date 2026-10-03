#include "composition_assets.h"
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOGO_BYTES (8 * 1024 * 1024)
#define LOGO_PIXELS (4 * 1024 * 1024)
static int logo_error(char *error, size_t n, const char *path, const char *reason)
{
    if (error && n) {
        snprintf(error, n, "logo '%.256s': %s", path, reason);
    }
    return -1;
}
int composition_logo_load(const char *path, Frame *out, char *error, size_t n)
{
    /* O_PATH does not activate device drivers. Verify the held inode, then
     * obtain its readable descriptor without resolving the user's path again. */
    int handle = open(path, O_PATH | O_CLOEXEC);
    if (handle < 0) {
        return logo_error(error, n, path, strerror(errno));
    }
    struct stat info;
    if (fstat(handle, &info) || !S_ISREG(info.st_mode) || info.st_size < 1 ||
        info.st_size > LOGO_BYTES) {
        close(handle);
        return logo_error(error, n, path, "use a local regular PNG/JPEG/WebP/BMP file up to 8 MiB");
    }
    char descriptor[64];
    snprintf(descriptor, sizeof descriptor, "/proc/self/fd/%d", handle);
    int fd = open(descriptor, O_RDONLY | O_CLOEXEC);
    close(handle);
    if (fd < 0) {
        return logo_error(error, n, path, strerror(errno));
    }
    uint8_t *bytes = av_mallocz((size_t)info.st_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!bytes) {
        close(fd);
        return logo_error(error, n, path, "cannot allocate image input");
    }
    size_t used = 0;
    while (used < (size_t)info.st_size) {
        ssize_t count = read(fd, bytes + used, (size_t)info.st_size - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            av_free(bytes);
            return logo_error(error, n, path, "cannot read complete image file");
        }
        used += (size_t)count;
    }
    close(fd);
    enum AVCodecID id = AV_CODEC_ID_NONE;
    if (used >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        id = AV_CODEC_ID_PNG;
    } else if (used >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
        id = AV_CODEC_ID_MJPEG;
    } else if (used >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4)) {
        id = AV_CODEC_ID_WEBP;
    } else if (used >= 2 && !memcmp(bytes, "BM", 2)) {
        id = AV_CODEC_ID_BMP;
    }
    const AVCodec *decoder = avcodec_find_decoder(id);
    AVCodecContext *context = decoder ? avcodec_alloc_context3(decoder) : NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *image = av_frame_alloc();
    struct SwsContext *scale = NULL;
    Frame candidate = {0};
    int rc = -1;
    const char *reason = "unsupported image; use PNG, JPEG, WebP or BMP";
    if (!context || !packet || !image) {
        goto done;
    }
    context->max_pixels = LOGO_PIXELS;
    context->thread_count = 1;
    reason = "cannot decode image; check that the file is complete and dimensions are bounded";
    if (avcodec_open2(context, decoder, NULL) < 0 ||
        av_packet_from_data(packet, bytes, (int)used) < 0) {
        goto done;
    }
    bytes = NULL; /* Packet now owns the padded input allocation. */
    if (avcodec_send_packet(context, packet) < 0 || avcodec_receive_frame(context, image) < 0) {
        goto done;
    }
    reason = "image exceeds 4096 pixels per edge or 4 million pixels; resize the logo";
    if (image->width < 1 || image->height < 1 || image->width > 4096 || image->height > 4096 ||
        (size_t)image->width * image->height > LOGO_PIXELS) {
        goto done;
    }
    reason = "cannot allocate or convert logo pixels";
    if (frame_alloc(&candidate, image->width, image->height)) {
        goto done;
    }
    scale = sws_getContext(image->width, image->height, image->format, image->width, image->height,
                           AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *dest[] = {candidate.data, NULL, NULL, NULL};
    int strides[] = {candidate.stride, 0, 0, 0};
    if (!scale || sws_scale(scale, (const uint8_t *const *)image->data, image->linesize, 0,
                            image->height, dest, strides) != image->height) {
        goto done;
    }
    frame_free(out);
    *out = candidate;
    memset(&candidate, 0, sizeof candidate);
    rc = 0;
done:
    frame_free(&candidate);
    sws_freeContext(scale);
    av_frame_free(&image);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    av_free(bytes);
    return rc ? logo_error(error, n, path, reason) : 0;
}
int composition_image_scale(const Frame *source, Frame *out, int w, int h, char *error, size_t n)
{
    if (frame_alloc(out, w, h)) {
        return logo_error(error, n, "prepared image", "cannot allocate scaled image");
    }
    struct SwsContext *scale = sws_getContext(source->width, source->height, AV_PIX_FMT_RGBA, w, h,
                                              AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    const uint8_t *input[] = {source->data, NULL, NULL, NULL};
    int input_stride[] = {source->stride, 0, 0, 0};
    uint8_t *output[] = {out->data, NULL, NULL, NULL};
    int output_stride[] = {out->stride, 0, 0, 0};
    int rows =
        scale ? sws_scale(scale, input, input_stride, 0, source->height, output, output_stride) : 0;
    sws_freeContext(scale);
    return rows == h ? 0 : logo_error(error, n, "prepared image", "cannot scale RGBA image");
}
