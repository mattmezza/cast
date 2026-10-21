#include "encoder_capabilities.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <string.h>
const char *const cast_video_encoder_names[CAST_VIDEO_ENCODER_COUNT] = {
    "auto", "libopenh264", "libx264", "libx265", "libvpx-vp9", "libaom-av1", "h264_videotoolbox"};
const char *const cast_audio_encoder_names[CAST_AUDIO_ENCODER_COUNT] = {
    "aac", "libopus", "pcm_s16le", "flac"};
static bool registered(void *unused, const char *name, bool video)
{
    (void)unused;
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    return codec && codec->type == (video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO);
}
void encoder_capabilities_query(CastEncoderCapabilities *out, bool lgpl,
                                CastEncoderLookup lookup, void *user)
{
    memset(out, 0, sizeof *out);
    if (!lookup) return;
    for (unsigned i = 1; i < CAST_VIDEO_ENCODER_COUNT; ++i) {
        if (lookup(user, cast_video_encoder_names[i], true)) {
            out->video_available |= CAST_ENCODER_BIT(i);
            if (!lgpl || (i != CAST_VIDEO_X264 && i != CAST_VIDEO_X265))
                out->video_allowed |= CAST_ENCODER_BIT(i);
        }
    }
    uint32_t h264 = CAST_ENCODER_BIT(CAST_VIDEO_OPENH264) | CAST_ENCODER_BIT(CAST_VIDEO_X264);
    if (out->video_available & h264) out->video_available |= CAST_ENCODER_BIT(CAST_VIDEO_AUTO);
    if (out->video_allowed & h264) out->video_allowed |= CAST_ENCODER_BIT(CAST_VIDEO_AUTO);
    for (unsigned i = 0; i < CAST_AUDIO_ENCODER_COUNT; ++i) {
        if (lookup(user, cast_audio_encoder_names[i], false)) {
            out->audio_available |= CAST_ENCODER_BIT(i);
            out->audio_allowed |= CAST_ENCODER_BIT(i);
        }
    }
}
void encoder_capabilities_snapshot(CastEncoderCapabilities *out)
{
#ifdef CAST_MEDIA_PROFILE_LGPL
    const bool lgpl = true;
#else
    const bool lgpl = false;
#endif
    encoder_capabilities_query(out, lgpl, registered, NULL);
}
static int named(const char *name, const char *const *names, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) if (!strcmp(name, names[i])) return (int)i;
    return -1;
}
int encoder_capabilities_container(const char *container, const char *video,
                                   const char *audio, char *error, size_t size)
{
    CastEncoderCapabilities caps;
    encoder_capabilities_snapshot(&caps);
    if (!container || !video || !audio) {
        snprintf(error, size, "missing output container or encoder"); return -1;
    }
    int vi = named(video, cast_video_encoder_names, CAST_VIDEO_ENCODER_COUNT);
    int ai = named(audio, cast_audio_encoder_names, CAST_AUDIO_ENCODER_COUNT);
    if (vi < 0 || !(caps.video_allowed & CAST_ENCODER_BIT(vi))) {
        snprintf(error, size, "video encoder '%s' unavailable or prohibited by media profile", video);
        return -1;
    }
    if (ai < 0 || !(caps.audio_allowed & CAST_ENCODER_BIT(ai))) {
        snprintf(error, size, "audio encoder '%s' unavailable or prohibited by media profile", audio);
        return -1;
    }
    if (vi == CAST_VIDEO_AUTO) {
#ifdef CAST_MEDIA_PROFILE_LGPL
        vi = CAST_VIDEO_OPENH264;
#else
        vi = caps.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_X264) ? CAST_VIDEO_X264 : CAST_VIDEO_OPENH264;
#endif
    }
    const AVOutputFormat *format = av_guess_format(container, NULL, NULL);
    const AVCodec *v = avcodec_find_encoder_by_name(cast_video_encoder_names[vi]);
    const AVCodec *a = avcodec_find_encoder_by_name(cast_audio_encoder_names[ai]);
    if (!format || !v || !a ||
        avformat_query_codec(format, v->id, FF_COMPLIANCE_NORMAL) <= 0 ||
        avformat_query_codec(format, a->id, FF_COMPLIANCE_NORMAL) <= 0) {
        snprintf(error, size, "container '%s' does not support selected video/audio encoders", container);
        return -1;
    }
    if (size) error[0] = '\0';
    return 0;
}
