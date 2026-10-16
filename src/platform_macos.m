/* MIT licensed. Compile with ARC; Apple objects never escape this adapter.
 * Startup is asynchronous on private queues: no main-thread dispatch or blocking
 * wait is used, so the daemon need not run an AppKit event loop. */
#include "platform_backend.h"
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface CastScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    pthread_mutex_t lock;
    Frame latest;
    Config config;
    uint64_t generation, epoch;
    bool closed, ready, failed;
    char failure[CAST_ERR];
}
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic, strong) dispatch_queue_t samples;
- (void)begin;
- (void)shutdown;
@end

static int mac_error(char *error, size_t size, const char *message)
{
    snprintf(error, size, "%s", message);
    return -1;
}
static int mac_config(const Config *cfg, char *error, size_t size)
{
    if (strcmp(cfg->capture_kind, "monitor"))
        return mac_error(error, size, "macOS capture currently supports monitors only; use capture monitor");
    if (cfg->monitor[0] && strcmp(cfg->monitor, "main")) {
        char *end = NULL;
        unsigned long id = strtoul(cfg->monitor, &end, 10);
        if (!id || !end || *end || id > UINT32_MAX)
            return mac_error(error, size, "macOS monitor must be main or a numeric display ID from screen list");
    }
    CGDirectDisplayID display = (!cfg->monitor[0] || !strcmp(cfg->monitor, "main"))
        ? CGMainDisplayID() : (CGDirectDisplayID)strtoul(cfg->monitor, NULL, 10);
    if (!CGDisplayIsActive(display))
        return mac_error(error, size, "selected macOS display is unavailable; use screen list");
    return 0;
}
@implementation CastScreenCapture
- (instancetype)init
{
    self = [super init];
    if (self) {
        pthread_mutex_init(&lock, NULL);
        _samples = dispatch_queue_create("cast.screen.samples", DISPATCH_QUEUE_SERIAL);
        generation = 1;
    }
    return self;
}
- (void)dealloc
{
    frame_free(&latest);
    pthread_mutex_destroy(&lock);
}
- (void)shutdown
{
    pthread_mutex_lock(&lock);
    closed = true;
    epoch++;
    ready = false;
    frame_free(&latest);
    SCStream *old = self.stream;
    self.stream = nil;
    pthread_mutex_unlock(&lock);
    [old stopCaptureWithCompletionHandler:^(NSError *error) { (void)error; }];
}
- (void)begin
{
    pthread_mutex_lock(&lock);
    uint64_t request = ++epoch;
    Config cfg = config;
    ready = false;
    failed = false;
    frame_free(&latest);
    SCStream *old = self.stream;
    self.stream = nil;
    pthread_mutex_unlock(&lock);
    [old stopCaptureWithCompletionHandler:^(NSError *error) { (void)error; }];
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent *content, NSError *error) {
        @autoreleasepool {
            pthread_mutex_lock(&self->lock);
            if (self->closed || request != self->epoch) {
                pthread_mutex_unlock(&self->lock);
                return;
            }
            CGDirectDisplayID wanted = (!cfg.monitor[0] || !strcmp(cfg.monitor, "main"))
                ? CGMainDisplayID() : (CGDirectDisplayID)strtoul(cfg.monitor, NULL, 10);
            SCDisplay *display = nil;
            for (SCDisplay *candidate in content.displays)
                if (candidate.displayID == wanted) { display = candidate; break; }
            if (error || !display) {
                self->failed = true;
                snprintf(self->failure, sizeof self->failure, "macOS capture: %s",
                    error ? error.localizedDescription.UTF8String : "selected display unavailable");
                pthread_mutex_unlock(&self->lock);
                return;
            }
            if (!display.width || !display.height || display.width > 8192 || display.height > 8192) {
                self->failed = true;
                snprintf(self->failure, sizeof self->failure, "macOS display dimensions exceed capture bounds (8192 pixels per axis)");
                pthread_mutex_unlock(&self->lock);
                return;
            }
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            SCStreamConfiguration *settings = [SCStreamConfiguration new];
            settings.width = display.width;
            settings.height = display.height;
            settings.pixelFormat = kCVPixelFormatType_32BGRA;
            settings.minimumFrameInterval = CMTimeMake(1, cfg.fps);
            settings.queueDepth = 3;
            settings.showsCursor = cfg.cursor;
            SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:settings delegate:self];
            NSError *attach = nil;
            if (![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:self.samples error:&attach]) {
                self->failed = true;
                snprintf(self->failure, sizeof self->failure, "macOS capture output: %s", attach.localizedDescription.UTF8String);
                pthread_mutex_unlock(&self->lock);
                return;
            }
            self.stream = stream;
            pthread_mutex_unlock(&self->lock);
            [stream startCaptureWithCompletionHandler:^(NSError *startError) {
                pthread_mutex_lock(&self->lock);
                if (!self->closed && request == self->epoch && startError) {
                    self->failed = true;
                    self->ready = false;
                    frame_free(&self->latest);
                    snprintf(self->failure, sizeof self->failure, "macOS capture start: %s", startError.localizedDescription.UTF8String);
                }
                pthread_mutex_unlock(&self->lock);
            }];
        }
    }];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    pthread_mutex_lock(&lock);
    if (!closed && stream == self.stream) {
        failed = true;
        ready = false;
        frame_free(&latest);
        snprintf(failure, sizeof failure, "macOS capture stopped: %s", error.localizedDescription.UTF8String);
    }
    pthread_mutex_unlock(&lock);
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || !CFArrayGetCount(attachments)) return;
    NSDictionary *metadata = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = metadata[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) {
        pthread_mutex_lock(&lock);
        if (!closed && stream == self.stream) {
            if (status && status.integerValue == SCFrameStatusIdle && ready)
                latest.ts_ns = cast_now_ns(); /* Unchanged desktop is still live. */
            else {
                ready = false;
                frame_free(&latest); /* Blank/suspended/stopped content must not linger. */
            }
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA) return;
    size_t width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels);
    if (!width || !height || width > 8192 || height > 8192) return;
    if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    pthread_mutex_lock(&lock);
    if (!closed && !failed && stream == self.stream && frame_alloc(&latest, (int)width, (int)height) == 0) {
        const uint8_t *base = CVPixelBufferGetBaseAddress(pixels);
        size_t stride = CVPixelBufferGetBytesPerRow(pixels);
        if (base && stride >= width * 4) {
            for (size_t y = 0; y < height; y++) {
                const uint8_t *src = base + y * stride;
                uint8_t *dst = latest.data + y * latest.stride;
                for (size_t x = 0; x < width; x++, src += 4, dst += 4) {
                    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
                }
            }
            latest.ts_ns = cast_now_ns();
            ready = true;
        }
    }
    pthread_mutex_unlock(&lock);
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}
@end

Platform *macos_open(const Config *cfg, char *error, size_t size)
{
    if (mac_config(cfg, error, size)) return NULL;
    @autoreleasepool {
        if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
            mac_error(error, size, "allow Cast in System Settings > Privacy & Security > Screen Recording, then restart Cast");
            return NULL;
        }
        CastScreenCapture *capture = [CastScreenCapture new];
        capture->config = *cfg;
        [capture begin];
        return (Platform *)CFBridgingRetain(capture);
    }
}
void macos_close(Platform *p)
{
    if (!p) return;
    @autoreleasepool {
        CastScreenCapture *capture = CFBridgingRelease(p);
        [capture shutdown];
    }
}
Capabilities macos_capabilities(Platform *p)
{
    (void)p;
    Capabilities cap = {.capture = true, .embedded_cursor = true};
    snprintf(cap.description, sizeof cap.description, "ScreenCaptureKit monitor capture; embedded cursor; no global input, selection UI, preview or panel exclusion");
    return cap;
}
uint64_t macos_source_generation(Platform *p)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    return capture->generation;
}
int macos_capture(Platform *p, Frame *frame, Cursor *cursor, char *error, size_t size)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    pthread_mutex_lock(&capture->lock);
    int result;
    if (capture->failed) result = mac_error(error, size, capture->failure);
    else if (!capture->ready) result = mac_error(error, size, "macOS capture awaiting first screen frame");
    else if (cast_now_ns() - capture->latest.ts_ns > 2000000000ULL)
        result = mac_error(error, size, "macOS capture stalled; refusing retained screen pixels");
    else {
        result = frame_copy(frame, &capture->latest);
        if (result < 0) mac_error(error, size, "cannot copy macOS screen frame");
    }
    pthread_mutex_unlock(&capture->lock);
    *cursor = (Cursor){0};
    return result;
}
int macos_reconfigure(Platform *p, const Config *cfg, char *error, size_t size)
{
    if (mac_config(cfg, error, size)) return -1;
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    bool changed = strcmp(capture->config.monitor, cfg->monitor) || capture->config.fps != cfg->fps || capture->config.cursor != cfg->cursor;
    capture->config = *cfg;
    if (changed) { capture->generation++; [capture begin]; }
    return 0;
}
int macos_command(Platform *p, Config *cfg, int argc, char **argv, char *error, size_t size)
{
    if (argc == 2 && !strcmp(argv[0], "screen") && !strcmp(argv[1], "list")) {
        CGDirectDisplayID displays[64]; uint32_t count = 0;
        if (CGGetActiveDisplayList(64, displays, &count) != kCGErrorSuccess)
            return mac_error(error, size, "cannot enumerate macOS displays");
        size_t used = 0;
        if (size) error[0] = 0;
        for (uint32_t i = 0; i < count && used < size; i++) {
            int n = snprintf(error + used, size - used, "%u%s: %zux%zu\n", displays[i],
                displays[i] == CGMainDisplayID() ? " (main)" : "", CGDisplayPixelsWide(displays[i]), CGDisplayPixelsHigh(displays[i]));
            if (n < 0 || (size_t)n >= size - used) break;
            used += (size_t)n;
        }
        return 0;
    }
    if ((argc == 3 && !strcmp(argv[0], "screen") && !strcmp(argv[1], "select")) ||
        (argc == 2 && !strcmp(argv[0], "capture") && !strcmp(argv[1], "monitor"))) {
        Config candidate = *cfg;
        snprintf(candidate.capture_kind, sizeof candidate.capture_kind, "monitor");
        if (argc == 3) {
            if (strlen(argv[2]) >= sizeof candidate.monitor) return mac_error(error, size, "monitor ID too long");
            snprintf(candidate.monitor, sizeof candidate.monitor, "%s", argv[2]);
        }
        if (macos_reconfigure(p, &candidate, error, size)) return -1;
        *cfg = candidate;
        snprintf(error, size, "macOS monitor: %s", cfg->monitor[0] ? cfg->monitor : "main");
        return 0;
    }
    return mac_error(error, size, "macOS supports screen list, screen select DISPLAY_ID|main and capture monitor; other capture controls are unavailable");
}
void macos_doctor(const Config *cfg, char *error, size_t size)
{
    (void)cfg;
    snprintf(error, size, "capture: ScreenCaptureKit; screen recording permission %s; monitor capture only; no global input/preview/panel exclusion", CGPreflightScreenCaptureAccess() ? "granted" : "required");
}
