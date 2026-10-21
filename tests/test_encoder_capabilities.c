#include "encoder_capabilities.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool all(void *unused, const char *name, bool video)
{
    (void)unused; (void)name; (void)video; return true;
}
static bool x264_only(void *unused, const char *name, bool video)
{
    (void)unused; return video && !strcmp(name, "libx264");
}
static bool videotoolbox_only(void *unused, const char *name, bool video)
{
    (void)unused; return video && !strcmp(name, "h264_videotoolbox");
}
static bool absent(void *unused, const char *name, bool video)
{
    (void)unused; (void)name; (void)video; return false;
}
int main(void)
{
    CastEncoderCapabilities c;
    encoder_capabilities_query(&c, true, all, NULL);
    assert(c.video_available == (1u << CAST_VIDEO_ENCODER_COUNT) - 1);
    assert(!(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_X264)));
    assert(!(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_X265)));
    assert(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_OPENH264));
    assert(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_AUTO));
    assert(c.audio_allowed == (1u << CAST_AUDIO_ENCODER_COUNT) - 1);
    encoder_capabilities_query(&c, false, all, NULL);
    assert(c.video_allowed == c.video_available);
    encoder_capabilities_query(&c, true, x264_only, NULL);
    assert(c.video_available & CAST_ENCODER_BIT(CAST_VIDEO_AUTO));
    assert(!c.video_allowed && !c.audio_available);
    encoder_capabilities_query(&c, false, x264_only, NULL);
    assert(c.video_allowed == (CAST_ENCODER_BIT(CAST_VIDEO_AUTO) | CAST_ENCODER_BIT(CAST_VIDEO_X264)));
    encoder_capabilities_query(&c, true, videotoolbox_only, NULL);
    assert(c.video_allowed == CAST_ENCODER_BIT(CAST_VIDEO_VIDEOTOOLBOX));
    assert(!(c.video_available & CAST_ENCODER_BIT(CAST_VIDEO_AUTO)));
    encoder_capabilities_query(&c, true, absent, NULL);
    assert(!c.video_available && !c.video_allowed && !c.audio_available && !c.audio_allowed);
    encoder_capabilities_query(&c, true, NULL, NULL);
    assert(!c.video_available);
    encoder_capabilities_snapshot(&c);
    char error[256];
    assert(encoder_capabilities_container(NULL, "auto", "aac", error, sizeof error));
    assert(encoder_capabilities_container("matroska", "not-an-encoder", "aac", error, sizeof error));
    assert(encoder_capabilities_container("not-a-muxer", "auto", "aac", error, sizeof error));
    assert(encoder_capabilities_container("webm", "auto", "aac", error, sizeof error));
#ifdef CAST_MEDIA_PROFILE_LGPL
    assert(!(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_X264)));
    assert(!(c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_X265)));
#endif
    if ((c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_AUTO)) &&
        (c.audio_allowed & CAST_ENCODER_BIT(CAST_AUDIO_AAC))) {
        assert(!encoder_capabilities_container("matroska", "auto", "aac", error, sizeof error));
        assert(!encoder_capabilities_container("mp4", "auto", "aac", error, sizeof error));
    }
    if ((c.video_allowed & CAST_ENCODER_BIT(CAST_VIDEO_VP9)) &&
        (c.audio_allowed & CAST_ENCODER_BIT(CAST_AUDIO_OPUS))) {
        assert(!encoder_capabilities_container("webm", "libvpx-vp9", "libopus", error, sizeof error));
    }
    printf("encoder capabilities: video available=0x%x allowed=0x%x; audio available=0x%x allowed=0x%x; policy/mux checks passed\n",
           c.video_available, c.video_allowed, c.audio_available, c.audio_allowed);
    return 0;
}
