/* Default microphone capture through AVAudioEngine. Compile with ARC. */
#import <AVFoundation/AVFoundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include "media_internal.h"
#include "audio_levels.h"
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#define RATE 48000
#define RING 96000
@interface CastMicrophoneToken : NSObject
@property(nonatomic, assign) CastAudio *owner;
@end
@implementation CastMicrophoneToken
@end
@interface CastDesktopAudioSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) CastAudio *owner;
@end
struct CastAudio {
    _Atomic(const CastAudioExtra *) extra_audio;
    SCStream *desktop;
    CastDesktopAudioSink *desktop_sink;
    dispatch_queue_t desktop_queue;
    float desktop_samples[RING * 2];
    uint64_t desktop_begin, desktop_end;
    AVAudioEngine *engine;
    CastMicrophoneToken *microphone_token;
    SwrContext *resampler;
    pthread_mutex_t lock;
    Config cfg;
    AudioLaneLevel levels[2];
    uint64_t level_epoch;
    float samples[RING * 2];
    uint64_t begin, end, accept_after, record_after, stream_after;
    bool tap, silent, desktop_ready, desktop_pending;
    char error[CAST_ERR];
};
static uint64_t sample_time(uint64_t ns)
{ return (ns / 1000000000) * RATE + (ns % 1000000000) * RATE / 1000000000; }
int audio_validate(CastAudio *a, const Config *cfg, char *error, size_t size)
{
    (void)a;
    if (cfg->virtual_audio) { snprintf(error, size, "Virtual audio is not supported on macOS"); return -1; }
    if (cfg->desktop && cfg->desktop_source[0] && strcmp(cfg->desktop_source, "default")) { snprintf(error, size, "macOS desktop audio supports desktop_source=default only"); return -1; }
    if (cfg->mic && cfg->mic_source[0] && strcmp(cfg->mic_source, "default")) {
        snprintf(error, size, "macOS microphone currently supports mic_source=default; choose the input in System Settings > Sound"); return -1;
    }
    return 0;
}
@implementation CastDesktopAudioSink
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    @synchronized(self) {
    (void)stream; CastAudio *a = self.owner; if (!a) return;
    pthread_mutex_lock(&a->lock); a->desktop_ready = a->desktop_pending = false; audio_level_clear(&a->levels[1],++a->level_epoch); snprintf(a->error, sizeof a->error, "Desktop capture stopped: %s", error.localizedDescription.UTF8String); pthread_mutex_unlock(&a->lock);
    }
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    @synchronized(self) {
    (void)stream; CastAudio *a = self.owner;
    if (!a || type != SCStreamOutputTypeAudio || !CMSampleBufferDataIsReady(sample)) return;
    const AudioStreamBasicDescription *format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mSampleRate != RATE || format->mChannelsPerFrame != 2 || !(format->mFormatFlags & kAudioFormatFlagIsFloat) || format->mBitsPerChannel != 32) return;
    AudioBufferList list; size_t required = 0; CMBlockBufferRef retained = NULL;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &required, NULL, 0, NULL, NULL, 0, NULL);
    if (!required || required > 65536) return;
    AudioBufferList *buffers = required <= sizeof list ? &list : malloc(required);
    if (!buffers) return;
    OSStatus rc = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, NULL, buffers, required, NULL, NULL, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained);
    double age = CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), CMSampleBufferGetPresentationTimeStamp(sample)));
    if (rc == noErr && isfinite(age) && age >= -0.05 && age < 1) {
        uint64_t now = cast_now_ns(); uint64_t at = sample_time(age > 0 ? now - (uint64_t)(age * 1e9) : now);
        size_t count = (size_t)CMSampleBufferGetNumSamples(sample);
        if (count > RING) { if (retained) CFRelease(retained); if (buffers != &list) free(buffers); return; }
        bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        if (buffers->mNumberBuffers >= (planar ? 2u : 1u) && buffers->mBuffers[0].mData && (!planar || buffers->mBuffers[1].mData) &&
            buffers->mBuffers[0].mDataByteSize >= count * sizeof(float) * (planar ? 1 : 2) &&
            (!planar || buffers->mBuffers[1].mDataByteSize >= count * sizeof(float))) {
            pthread_mutex_lock(&a->lock);
            if (!a->desktop_end || at > a->desktop_end + RATE/4 || at + count + RATE/4 < a->desktop_end) a->desktop_begin = at;
            else for (uint64_t p = a->desktop_end; p < at; ++p) a->desktop_samples[(p%RING)*2] = a->desktop_samples[(p%RING)*2+1] = 0;
            AudioLevelAccumulator level={0};
            for (size_t i = 0; i < count; ++i) {
                uint64_t p = at + i; if (p < a->accept_after) continue;
                for (unsigned ch = 0; ch < 2; ++ch) a->desktop_samples[(p % RING)*2+ch] = audio_level_sample(&level,((float *)buffers->mBuffers[planar ? ch : 0].mData)[planar ? i : i*2+ch],a->cfg.desktop_gain);
            }
            audio_level_finish(&a->levels[1],&level,audio_level_capture_end(at,(unsigned)count));
            a->desktop_end = at + count;
            if (a->desktop_end > RING && a->desktop_begin < a->desktop_end - RING) a->desktop_begin = a->desktop_end - RING;
            pthread_mutex_unlock(&a->lock);
        }
    }
    if (retained) CFRelease(retained); if (buffers != &list) free(buffers);
    }
}
@end
static int desktop_start(CastAudio *a, char *error, size_t size)
{
    (void)error; (void)size;
    if (!a->cfg.desktop) return 0;
    /* Each start owns a fresh token. Shutdown retires its owner under the same
     * monitor used by every completion and sample callback, so callbacks never
     * keep a raw owner alive after close or revive a previous configuration. */
    CastDesktopAudioSink *sink = [CastDesktopAudioSink new]; sink.owner = a;
    a->desktop_sink = sink;
    a->desktop_queue = dispatch_queue_create("cast.desktop.audio", DISPATCH_QUEUE_SERIAL);
    pthread_mutex_lock(&a->lock); a->desktop_pending = true; a->desktop_ready = false; pthread_mutex_unlock(&a->lock);
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *content, NSError *failure) {
        @autoreleasepool { @synchronized(sink) {
            CastAudio *owner = sink.owner; if (!owner) return;
            if (failure || !content.displays.count) {
                pthread_mutex_lock(&owner->lock);
                owner->desktop_pending = false;
                snprintf(owner->error, sizeof owner->error, "Desktop audio requires Screen Recording permission: %s", failure ? failure.localizedDescription.UTF8String : "no display available");
                pthread_mutex_unlock(&owner->lock); return;
            }
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject excludingWindows:@[]];
            SCStreamConfiguration *cfg = [SCStreamConfiguration new];
            cfg.width = 2; cfg.height = 2; cfg.minimumFrameInterval = CMTimeMake(1, 1);
            cfg.capturesAudio = YES; cfg.sampleRate = RATE; cfg.channelCount = 2; cfg.excludesCurrentProcessAudio = YES;
            SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:sink];
            NSError *err = nil;
            if (![stream addStreamOutput:sink type:SCStreamOutputTypeAudio sampleHandlerQueue:owner->desktop_queue error:&err]) {
                pthread_mutex_lock(&owner->lock); owner->desktop_pending = false;
                snprintf(owner->error, sizeof owner->error, "Desktop audio output failed: %s", err.localizedDescription.UTF8String);
                pthread_mutex_unlock(&owner->lock); return;
            }
            owner->desktop = stream;
            __weak SCStream *weakStream = stream;
            [stream startCaptureWithCompletionHandler:^(NSError *err) {
                @synchronized(sink) {
                    CastAudio *current = sink.owner;
                    if (!current) { [weakStream stopCaptureWithCompletionHandler:^(NSError *ignored) { (void)ignored; }]; return; }
                    pthread_mutex_lock(&current->lock);
                    current->desktop_pending = false; current->desktop_ready = err == nil;
                    if (err) snprintf(current->error, sizeof current->error, "Desktop audio capture failed: %s", err.localizedDescription.UTF8String);
                    pthread_mutex_unlock(&current->lock);
                }
            }];
        } }
    }];
    return 0;
}
static bool permission(void)
{
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) { (void)granted; }];
        return false; /* Let the daemon continue handling privacy commands while consent is shown. */
    }
    return status == AVAuthorizationStatusAuthorized;
}
static void stop(CastAudio *a)
{
    CastDesktopAudioSink *sink = a->desktop_sink;
    SCStream *desktop = nil;
    @synchronized(sink) {
        sink.owner = NULL;
        desktop = a->desktop; a->desktop = nil;
        pthread_mutex_lock(&a->lock); a->desktop_pending = a->desktop_ready = false; pthread_mutex_unlock(&a->lock);
    }
    if (desktop) {
        [desktop removeStreamOutput:sink type:SCStreamOutputTypeAudio error:NULL];
        [desktop stopCaptureWithCompletionHandler:^(NSError *err) { (void)err; (void)sink; }];
    }
    a->desktop_sink = nil; a->desktop_queue = nil;
    [a->engine stop];
    if (a->tap) [a->engine.inputNode removeTapOnBus:0];
    @synchronized(a->microphone_token) { a->microphone_token.owner = NULL; }
    a->microphone_token = nil; a->tap = false; a->engine = nil;
    swr_free(&a->resampler);
    pthread_mutex_lock(&a->lock);
    uint64_t epoch=++a->level_epoch;
    for(unsigned i=0;i<2;++i)audio_level_clear(&a->levels[i],epoch);
    pthread_mutex_unlock(&a->lock);
}
static int start(CastAudio *a, char *error, size_t size)
{
    pthread_mutex_lock(&a->lock); a->error[0] = 0; pthread_mutex_unlock(&a->lock);
    if (desktop_start(a, error, size) < 0) return -1;
    if (!a->cfg.mic) return 0;
    if (!permission()) { snprintf(error, size, "Microphone permission required; approve the prompt then enable microphone again, or enable it in System Settings > Privacy & Security > Microphone"); return -1; }
    a->engine = [AVAudioEngine new];
    AVAudioInputNode *input = a->engine.inputNode;
    AVAudioFormat *format = [input outputFormatForBus:0];
    if (!format.channelCount || !format.sampleRate || format.commonFormat != AVAudioPCMFormatFloat32 || format.interleaved) {
        snprintf(error, size, "Microphone does not expose planar float audio"); stop(a); return -1;
    }
    AVChannelLayout inLayout, outLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&inLayout, (int)format.channelCount);
    int rc = swr_alloc_set_opts2(&a->resampler, &outLayout, AV_SAMPLE_FMT_FLT, RATE,
                               &inLayout, AV_SAMPLE_FMT_FLTP, (int)format.sampleRate, 0, NULL);
    av_channel_layout_uninit(&inLayout);
    if (rc < 0 || swr_init(a->resampler) < 0) { snprintf(error, size, "Microphone resampler initialization failed"); stop(a); return -1; }
    CastMicrophoneToken *token = [CastMicrophoneToken new]; token.owner = a; a->microphone_token = token;
    [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
        @autoreleasepool { @synchronized(token) {
            CastAudio *a = token.owner; if (!a) return;
            if (!when.hostTimeValid || !buffer.floatChannelData) return;
            double age = [AVAudioTime secondsForHostTime:mach_absolute_time()] - [AVAudioTime secondsForHostTime:when.hostTime];
            if (!isfinite(age) || age < -0.05 || age > 1) return;
            uint64_t now = cast_now_ns();
            uint64_t ts = age > 0 ? now - (uint64_t)(age * 1e9) : now;
            /* Include converter delay in the first output sample timestamp. */
            int64_t delay = swr_get_delay(a->resampler, RATE);
            uint64_t at = sample_time(ts);
            at = at > (uint64_t)delay ? at - (uint64_t)delay : 0;
            float converted[8192 * 2]; uint8_t *dest[] = {(uint8_t *)converted};
            int count = swr_convert(a->resampler, dest, 8192, (const uint8_t **)buffer.floatChannelData, (int)buffer.frameLength);
            if (count < 0) return;
            pthread_mutex_lock(&a->lock);
            if (!a->end || at > a->end + RATE/4 || at + (uint64_t)count + RATE/4 < a->end) a->begin = at;
            else for (uint64_t p = a->end; p < at; ++p) a->samples[(p%RING)*2] = a->samples[(p%RING)*2+1] = 0;
            AudioLevelAccumulator level={0};
            for (int i = 0; i < count; ++i) {
                uint64_t p = at + (uint64_t)i;
                if (p < a->accept_after) continue;
                a->samples[(p % RING)*2] = audio_level_sample(&level,converted[i*2],a->cfg.mic_gain);
                a->samples[(p % RING)*2+1] = audio_level_sample(&level,converted[i*2+1],a->cfg.mic_gain);
            }
            audio_level_finish(&a->levels[0],&level,audio_level_capture_end(at,(unsigned)count));
            a->end = at + (uint64_t)count;
            if (a->end > RING && a->begin < a->end - RING) a->begin = a->end - RING;
            pthread_mutex_unlock(&a->lock);
        } }
    }];
    a->tap = true;
    NSError *err = nil;
    if (![a->engine startAndReturnError:&err]) { snprintf(error, size, "Microphone start failed: %s", err.localizedDescription.UTF8String); stop(a); return -1; }
    return 0;
}
void audio_set_extra_audio(CastAudio *a, const CastAudioExtra *extra)
{ if (a) atomic_store(&a->extra_audio, extra); }
CastAudio *audio_open(const Config *cfg, char *error, size_t size)
{
    if (audio_validate(NULL, cfg, error, size) < 0) return NULL;
    CastAudio *a = calloc(1, sizeof(*a));
    if (!a) { snprintf(error, size, "Audio allocation failed"); return NULL; }
    atomic_init(&a->extra_audio, NULL);
    pthread_mutex_init(&a->lock, NULL); a->cfg = *cfg;
    if (start(a, error, size) < 0) { audio_close(a); return NULL; }
    return a;
}
void audio_close(CastAudio *a)
{ if (a) { stop(a); pthread_mutex_destroy(&a->lock); free(a); } }
int audio_configure(CastAudio *a, const Config *cfg, char *error, size_t size)
{
    if (audio_validate(a, cfg, error, size) < 0) return -1;
    pthread_mutex_lock(&a->lock);
    bool unchanged = cfg->mic == a->cfg.mic && cfg->desktop == a->cfg.desktop &&
        (!cfg->mic || a->engine.running) &&
        (!cfg->desktop || a->desktop_pending || a->desktop_ready);
    Config old = a->cfg;
    if (unchanged) {
        a->cfg = *cfg;
        uint64_t epoch=++a->level_epoch;
        for(unsigned i=0;i<2;++i)audio_level_clear(&a->levels[i],epoch);
    }
    pthread_mutex_unlock(&a->lock);
    if (unchanged) return 0;
    stop(a);
    pthread_mutex_lock(&a->lock); a->cfg = *cfg; pthread_mutex_unlock(&a->lock);
    audio_barrier(a, a->silent);
    if (start(a, error, size) == 0) return 0;
    stop(a); pthread_mutex_lock(&a->lock); a->cfg = old; pthread_mutex_unlock(&a->lock); char ignored[CAST_ERR];
    if (start(a, ignored, sizeof ignored) < 0) { pthread_mutex_lock(&a->lock); snprintf(a->error, sizeof a->error, "%s", ignored); pthread_mutex_unlock(&a->lock); }
    return -1;
}
void audio_barrier(CastAudio *a, bool silent)
{
    pthread_mutex_lock(&a->lock); a->silent = silent;
    uint64_t epoch=++a->level_epoch;
    for(unsigned i=0;i<2;++i)audio_level_clear(&a->levels[i],epoch);
    a->accept_after = a->record_after = a->stream_after = sample_time(cast_now_ns());
    a->begin = a->end = a->desktop_begin = a->desktop_end = 0; memset(a->samples, 0, sizeof a->samples); memset(a->desktop_samples, 0, sizeof a->desktop_samples); pthread_mutex_unlock(&a->lock);
}
void audio_virtual_privacy(CastAudio *a, bool silent)
{ pthread_mutex_lock(&a->lock); a->silent = silent; pthread_mutex_unlock(&a->lock); }
static uint64_t privacy_after(CastAudio *a)
{ uint64_t now = sample_time(cast_now_ns()); if (a->end > now) now = a->end; return a->desktop_end > now ? a->desktop_end : now; }
void audio_record_privacy(CastAudio *a)
{ pthread_mutex_lock(&a->lock); a->record_after = privacy_after(a); pthread_mutex_unlock(&a->lock); }
void audio_stream_privacy(CastAudio *a)
{ pthread_mutex_lock(&a->lock); a->stream_after = privacy_after(a); pthread_mutex_unlock(&a->lock); }
static void read_locked(CastAudio *a, uint64_t ns, float *dst, unsigned count, uint64_t after, int source)
{
    memset(dst, 0, count * 2 * sizeof *dst);
    uint64_t at = sample_time(ns);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t p = at + i;
        if (p < after || p < a->accept_after) continue;
        for (unsigned ch = 0; ch < 2; ++ch) {
            float v = 0;
            if (source != 1 && a->cfg.mic && p >= a->begin && p < a->end) v += a->samples[(p%RING)*2+ch] * a->cfg.mic_gain;
            if (source != 0 && a->cfg.desktop && p >= a->desktop_begin && p < a->desktop_end) v += a->desktop_samples[(p%RING)*2+ch] * a->cfg.desktop_gain;
            dst[i*2+ch] = fmaxf(-1, fminf(1, v));
        }
    }
}
bool audio_record_pending(CastAudio *a, uint64_t ns, int count)
{
    if (!a || count <= 0) return false;
    /* AVAudioEngine may deliver 100 ms batches despite bufferSize:1024. Never
     * commit silence merely because the recording worker beat that callback.
     * A stopped/stalled source still advances after 250 ms; no clock shift or
     * destructive ring read is needed. Privacy-retired windows need no wait. */
    uint64_t now = cast_now_ns();
    if (now > ns && now - ns >= 250000000ULL) return false;
    uint64_t end = sample_time(ns) + (unsigned)count;
    pthread_mutex_lock(&a->lock);
    bool pending = end > a->accept_after && end > a->record_after &&
        ((a->cfg.mic && a->end < end) ||
         (a->cfg.desktop && !a->error[0] && a->desktop_end < end));
    pthread_mutex_unlock(&a->lock);
    return pending;
}
void audio_read(CastAudio *a, uint64_t ns, float *dst, int count)
{ if (count <= 0) return; pthread_mutex_lock(&a->lock); read_locked(a, ns, dst, (unsigned)count, a->record_after, 2); pthread_mutex_unlock(&a->lock); }
void audio_stream_read(CastAudio *a, uint64_t ns, float *dst, int count)
{ if (count <= 0) return; pthread_mutex_lock(&a->lock); read_locked(a, ns, dst, (unsigned)count, a->stream_after, 2); pthread_mutex_unlock(&a->lock); }
int audio_read_selection(CastAudio *a, int source, uint64_t ns, float *dst, unsigned count, char *error, size_t size)
{
    if (!a || !dst || !count || count > 4800 || source < 0 || source > 2) { snprintf(error, size, "Invalid bounded speech audio request"); return -1; }
    memset(dst, 0, count*2*sizeof *dst); pthread_mutex_lock(&a->lock);
    int result = 0;
    if ((source == 0 && (!a->cfg.mic || !a->engine.running)) || (source == 1 && (!a->cfg.desktop || (!a->desktop_ready && !a->desktop_pending) || a->error[0])) || (source == 2 && ((!a->cfg.mic && !a->cfg.desktop) || (a->cfg.mic && !a->engine.running) || (a->cfg.desktop && ((!a->desktop_ready && !a->desktop_pending) || a->error[0]))))) { snprintf(error, size, "Requested audio source disabled or unavailable; no fallback"); result = -1; }
    else if (source != 0 && a->cfg.desktop && a->desktop_pending) { snprintf(error, size, "Waiting for desktop audio capture startup"); result = 1; }
    else if (((source != 1 && a->cfg.mic && a->end < sample_time(ns) + count) || (source != 0 && a->cfg.desktop && a->desktop_end < sample_time(ns) + count))) { snprintf(error, size, "Waiting for captured audio"); result = 1; }
    else read_locked(a, ns, dst, count, a->accept_after, source);
    pthread_mutex_unlock(&a->lock); return result;
}
void audio_list(CastAudio *a, char *out, size_t size)
{ (void)a; snprintf(out, size, "default [microphone] System Settings > Sound input\ndefault [desktop] ScreenCaptureKit system audio; virtual audio unavailable\n"); }
void audio_levels(CastAudio *a, AudioLevels *out)
{
    if(!out)return;
    memset(out,0,sizeof *out);if(!a)return;
    pthread_mutex_lock(&a->lock);
    out->supported=true;out->mic=a->levels[0];out->desktop=a->levels[1];
    uint64_t now=cast_now_ns();
    audio_level_gate(&out->mic,a->cfg.mic,out->mic.timestamp_ns!=0,now);
    audio_level_gate(&out->desktop,a->cfg.desktop,out->desktop.timestamp_ns!=0,now);
    pthread_mutex_unlock(&a->lock);
}
void audio_status(CastAudio *a, char *out, size_t size)
{ pthread_mutex_lock(&a->lock); snprintf(out, size, "{\"available\":true,\"mic_ready\":%s,\"desktop_ready\":%s,\"virtual_ready\":false,\"virtual_silent\":%s}", a->engine.running ? "true" : "false", a->desktop_ready && !a->error[0] ? "true" : "false", a->silent ? "true" : "false"); pthread_mutex_unlock(&a->lock); }
void audio_error(CastAudio *a, char *out, size_t size)
{ pthread_mutex_lock(&a->lock); snprintf(out, size, "%s", a->cfg.mic && !a->engine.running ? "Microphone capture unavailable" : a->error[0] ? a->error : a->desktop_pending ? "Desktop audio starting; waiting for ScreenCaptureKit" : ""); pthread_mutex_unlock(&a->lock); }

#ifdef CAST_TEST
CastAudio *audio_test_open(const Config *cfg)
{
    CastAudio *a = calloc(1, sizeof *a);
    if (a) { atomic_init(&a->extra_audio, NULL); pthread_mutex_init(&a->lock, NULL); a->cfg = *cfg; }
    return a;
}
void audio_test_push(CastAudio *a, int source, uint64_t ns, const float *samples, int count)
{
    pthread_mutex_lock(&a->lock);
    uint64_t at = sample_time(ns);
    float *ring = source == 0 ? a->samples : a->desktop_samples;
    uint64_t *begin = source == 0 ? &a->begin : &a->desktop_begin;
    uint64_t *end = source == 0 ? &a->end : &a->desktop_end;
    if (!*end) *begin = at;
    AudioLevelAccumulator level={0};
    for (int i = 0; i < count; ++i) {
        uint64_t p = at + (unsigned)i;
        if (p < a->accept_after) continue;
        ring[(p % RING)*2] = audio_level_sample(&level,samples[i*2],source?a->cfg.desktop_gain:a->cfg.mic_gain);
        ring[(p % RING)*2+1] = audio_level_sample(&level,samples[i*2+1],source?a->cfg.desktop_gain:a->cfg.mic_gain);
    }
    audio_level_finish(&a->levels[source?1:0],&level,audio_level_capture_end(at,(unsigned)count));
    *end = at + (unsigned)count;
    if (*end > RING && *begin < *end - RING) *begin = *end - RING;
    pthread_mutex_unlock(&a->lock);
}
#endif
