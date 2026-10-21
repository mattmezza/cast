#ifndef CAST_LOCAL_IMAGE_INPUT_H
#define CAST_LOCAL_IMAGE_INPUT_H
#include <libavformat/avformat.h>
#include <stddef.h>
#define CAST_LOCAL_IMAGE_PIXELS_MAX (7680 * 4320)
#define CAST_LOCAL_IMAGE_BYTES_MAX (64u * 1024u * 1024u)
/* Worker-only, pread on an already opened regular descriptor. Does not change
 * descriptor position. 1 selects magic-verified PNG/JPEG image2pipe and sets
 * video_codec_id; pass a NULL URL to avformat_open_input in this case. 0 leaves
 * ordinary container probing unchanged. -1 returns a bounded validation error.
 * Caller retains its custom AVIO and regular-FD ownership; no path is reopened. */
int local_image_input(int fd, AVFormatContext *, const AVInputFormat **, char *, size_t);
/* For the forced single-stream image2pipe context only: bounds decoder pixels
 * and threads while probing, before the renderer creates its own decoder. */
int local_image_stream_info(AVFormatContext *);
#endif
