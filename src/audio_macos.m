/* Default microphone capture through AVAudioEngine. Compile with ARC. */
#import <AVFoundation/AVFoundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include "media_internal.h"
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#define RATE 48000
#define RING 96000
@interface CastDesktopAudioSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) CastAudio *owner;
@end
struct CastAudio {
    SCStream *desktop;
    CastDesktopAudioSink *desktop_sink;
    dispatch_queue_t desktop_queue;
    float desktop_samples[RING * 2];
    uint64_t desktop_begin, desktop_end;
    AVAudioEngine *engine;
    SwrContext *resampler;
    pthread_mutex_t lock;
    Config cfg;
    float samples[RING * 2];
    uint64_t begin, end, accept_after, record_after, stream_after;
    bool tap, silent;
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
    (void)stream; CastAudio *a = self.owner; if (!a) return;
    pthread_mutex_lock(&a->lock); snprintf(a->error, sizeof a->error, "Desktop capture stopped: %s", error.localizedDescription.UTF8String); pthread_mutex_unlock(&a->lock);
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    (void)stream; CastAudio *a = self.owner;
    if (!a || type != SCStreamOutputTypeAudio || !CMSampleBufferDataIsReady(sample)) return;
    const AudioStreamBasicDescription *format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mSampleRate != RATE || format->mChannelsPerFrame != 2 || !(format->mFormatFlags & kAudioFormatFlagIsFloat) || format->mBitsPerChannel != 32) return;
    AudioBufferList list; size_t required = 0; CMBlockBufferRef retained = NULL;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &required, NULL, 0, NULL, NULL, 0, NULL);
    if (!required) return;
    AudioBufferList *buffers = required <= sizeof list ? &list : malloc(required);
    if (!buffers) return;
    OSStatus rc = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, NULL, buffers, required, NULL, NULL, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained);
    double age = CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), CMSampleBufferGetPresentationTimeStamp(sample)));
    if (rc == noErr && isfinite(age) && age >= -0.05 && age < 1) {
        uint64_t now = cast_now_ns(); uint64_t at = sample_time(age > 0 ? now - (uint64_t)(age * 1e9) : now);
        size_t count = (size_t)CMSampleBufferGetNumSamples(sample);
        bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        if (buffers->mNumberBuffers >= (planar ? 2u : 1u) &&
            buffers->mBuffers[0].mDataByteSize >= count * sizeof(float) * (planar ? 1 : 2) &&
            (!planar || buffers->mBuffers[1].mDataByteSize >= count * sizeof(float))) {
            pthread_mutex_lock(&a->lock);
            if (at > a->desktop_end || at + count < a->desktop_end) a->desktop_begin = at;
            for (size_t i = 0; i < count; ++i) {
                uint64_t p = at + i; if (p < a->accept_after) continue;
                for (unsigned ch = 0; ch < 2; ++ch) a->desktop_samples[(p % RING)*2+ch] = ((float *)buffers->mBuffers[planar ? ch : 0].mData)[planar ? i : i*2+ch];
            }
            a->desktop_end = at + count;
            if (a->desktop_end > RING && a->desktop_begin < a->desktop_end - RING) a->desktop_begin = a->desktop_end - RING;
            pthread_mutex_unlock(&a->lock);
        }
    }
    if (retained) CFRelease(retained); if (buffers != &list) free(buffers);
}
@end
static int desktop_start(CastAudio *a, char *error, size_t size)
{
    if (!a->cfg.desktop) return 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block SCShareableContent *content = nil; __block NSError *failure = nil;
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *result, NSError *err) {
        content = result; failure = err; dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!content.displays.count) { snprintf(error, size, "Desktop audio requires Screen Recording permission: %s", failure ? failure.localizedDescription.UTF8String : "no display available"); return -1; }
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject excludingWindows:@[]];
    SCStreamConfiguration *cfg = [SCStreamConfiguration new];
    cfg.width = 2; cfg.height = 2; cfg.minimumFrameInterval = CMTimeMake(1, 1);
    cfg.capturesAudio = YES; cfg.sampleRate = RATE; cfg.channelCount = 2; cfg.excludesCurrentProcessAudio = YES;
    a->desktop_sink = [CastDesktopAudioSink new]; a->desktop_sink.owner = a;
    a->desktop_queue = dispatch_queue_create("cast.desktop.audio", DISPATCH_QUEUE_SERIAL);
    a->desktop = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:a->desktop_sink];
    NSError *err = nil;
    if (![a->desktop addStreamOutput:a->desktop_sink type:SCStreamOutputTypeAudio sampleHandlerQueue:a->desktop_queue error:&err]) {
        snprintf(error, size, "Desktop audio output failed: %s", err.localizedDescription.UTF8String); return -1;
    }
    failure = nil;
    [a->desktop startCaptureWithCompletionHandler:^(NSError *err) { failure = err; dispatch_semaphore_signal(sem); }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (failure) { snprintf(error, size, "Desktop audio capture failed: %s", failure.localizedDescription.UTF8String); return -1; }
    return 0;
}
static bool permission(void)
{
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            (void)granted; dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    }
    return status == AVAuthorizationStatusAuthorized;
}
static void stop(CastAudio *a)
{
    if (a->desktop) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [a->desktop stopCaptureWithCompletionHandler:^(NSError *err) { (void)err; dispatch_semaphore_signal(sem); }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        dispatch_sync(a->desktop_queue, ^{});
        a->desktop_sink.owner = NULL; a->desktop = nil; a->desktop_sink = nil; a->desktop_queue = nil;
    }
    [a->engine stop];
    if (a->tap) [a->engine.inputNode removeTapOnBus:0];
    a->tap = false; a->engine = nil;
    swr_free(&a->resampler);
}
static int start(CastAudio *a, char *error, size_t size)
{
    a->error[0] = 0;
    if (desktop_start(a, error, size) < 0) return -1;
    if (!a->cfg.mic) return 0;
    if (!permission()) { snprintf(error, size, "Microphone permission denied; enable it in System Settings > Privacy & Security > Microphone"); return -1; }
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
    [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
        @autoreleasepool {
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
            if (at > a->end || at + (uint64_t)count < a->end) a->begin = at;
            for (int i = 0; i < count; ++i) {
                uint64_t p = at + (uint64_t)i;
                if (p < a->accept_after) continue;
                a->samples[(p % RING)*2] = converted[i*2];
                a->samples[(p % RING)*2+1] = converted[i*2+1];
            }
            a->end = at + (uint64_t)count;
            if (a->end > RING && a->begin < a->end - RING) a->begin = a->end - RING;
            pthread_mutex_unlock(&a->lock);
        }
    }];
    a->tap = true;
    NSError *err = nil;
    if (![a->engine startAndReturnError:&err]) { snprintf(error, size, "Microphone start failed: %s", err.localizedDescription.UTF8String); stop(a); return -1; }
    return 0;
}
CastAudio *audio_open(const Config *cfg, char *error, size_t size)
{
    if (audio_validate(NULL, cfg, error, size) < 0) return NULL;
    CastAudio *a = calloc(1, sizeof(*a));
    if (!a) { snprintf(error, size, "Audio allocation failed"); return NULL; }
    pthread_mutex_init(&a->lock, NULL); a->cfg = *cfg;
    if (start(a, error, size) < 0) { audio_close(a); return NULL; }
    return a;
}
void audio_close(CastAudio *a)
{ if (a) { stop(a); pthread_mutex_destroy(&a->lock); free(a); } }
int audio_configure(CastAudio *a, const Config *cfg, char *error, size_t size)
{
    if (audio_validate(a, cfg, error, size) < 0) return -1;
    if (cfg->mic == a->cfg.mic && cfg->desktop == a->cfg.desktop) { pthread_mutex_lock(&a->lock); a->cfg = *cfg; pthread_mutex_unlock(&a->lock); return 0; }
    Config old = a->cfg; stop(a);
    pthread_mutex_lock(&a->lock); a->cfg = *cfg; pthread_mutex_unlock(&a->lock);
    audio_barrier(a, a->silent);
    if (start(a, error, size) == 0) return 0;
    stop(a); a->cfg = old; char ignored[CAST_ERR];
    if (start(a, ignored, sizeof ignored) < 0) snprintf(a->error, sizeof a->error, "%s", ignored);
    return -1;
}
void audio_barrier(CastAudio *a, bool silent)
{
    pthread_mutex_lock(&a->lock); a->silent = silent;
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
void audio_read(CastAudio *a, uint64_t ns, float *dst, int count)
{ if (count <= 0) return; pthread_mutex_lock(&a->lock); read_locked(a, ns, dst, (unsigned)count, a->record_after, 2); pthread_mutex_unlock(&a->lock); }
void audio_stream_read(CastAudio *a, uint64_t ns, float *dst, int count)
{ if (count <= 0) return; pthread_mutex_lock(&a->lock); read_locked(a, ns, dst, (unsigned)count, a->stream_after, 2); pthread_mutex_unlock(&a->lock); }
int audio_read_selection(CastAudio *a, int source, uint64_t ns, float *dst, unsigned count, char *error, size_t size)
{
    if (!a || !dst || !count || count > 4800 || source < 0 || source > 2) { snprintf(error, size, "Invalid bounded speech audio request"); return -1; }
    memset(dst, 0, count*2*sizeof *dst); pthread_mutex_lock(&a->lock);
    int result = 0;
    if ((source == 0 && (!a->cfg.mic || !a->engine.running)) || (source == 1 && (!a->cfg.desktop || !a->desktop || a->error[0])) || (source == 2 && ((!a->cfg.mic && !a->cfg.desktop) || (a->cfg.mic && !a->engine.running) || (a->cfg.desktop && (!a->desktop || a->error[0]))))) { snprintf(error, size, "Requested audio source disabled or unavailable; no fallback"); result = -1; }
    else if (((source != 1 && a->cfg.mic && a->end < sample_time(ns) + count) || (source != 0 && a->cfg.desktop && a->desktop_end < sample_time(ns) + count))) { snprintf(error, size, "Waiting for captured audio"); result = 1; }
    else read_locked(a, ns, dst, count, a->accept_after, source);
    pthread_mutex_unlock(&a->lock); return result;
}
void audio_list(CastAudio *a, char *out, size_t size)
{ (void)a; snprintf(out, size, "default [microphone] System Settings > Sound input\ndefault [desktop] ScreenCaptureKit system audio; virtual audio unavailable\n"); }
void audio_status(CastAudio *a, char *out, size_t size)
{ pthread_mutex_lock(&a->lock); snprintf(out, size, "{\"available\":true,\"mic_ready\":%s,\"desktop_ready\":%s,\"virtual_ready\":false,\"virtual_silent\":%s}", a->engine.running ? "true" : "false", a->desktop && !a->error[0] ? "true" : "false", a->silent ? "true" : "false"); pthread_mutex_unlock(&a->lock); }
void audio_error(CastAudio *a, char *out, size_t size)
{ pthread_mutex_lock(&a->lock); snprintf(out, size, "%s", a->cfg.mic && !a->engine.running ? "Microphone capture unavailable" : a->error); pthread_mutex_unlock(&a->lock); }
