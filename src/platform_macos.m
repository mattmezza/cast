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
#include <unistd.h>
#include <math.h>

@interface CastScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    pthread_mutex_t lock;
    Frame latest;
    Config config;
    uint64_t generation, epoch, accept_after;
    bool closed, ready, failed;
    CFMachPortRef input_tap;
    CFRunLoopRef input_loop;
    pthread_t input_thread;
    bool input_started;
    struct { CGPoint point; int button; uint64_t at; } clicks[64];
    unsigned click_count;
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
static bool display_pixels(CGDirectDisplayID display, size_t *width, size_t *height)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) return false;
    /* CGDisplayPixelsWide/High report logical dimensions on Retina displays.
     * ScreenCaptureKit's output dimensions need the active mode's pixels. */
    *width = CGDisplayModeGetPixelWidth(mode);
    *height = CGDisplayModeGetPixelHeight(mode);
    CGDisplayModeRelease(mode);
    return *width && *height;
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
/* Listen-only input never consumes or changes another application's events.
 * Creating the tap does not request permission; Input Monitoring can be granted
 * explicitly in System Settings and takes effect after restarting capture. */
static CGEventRef input_event(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                             void *context)
{
    (void)proxy;
    CastScreenCapture *capture = (__bridge CastScreenCapture *)context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(capture->input_tap, true);
        return event;
    }
    if (type != kCGEventLeftMouseDown && type != kCGEventRightMouseDown &&
        type != kCGEventOtherMouseDown) return event;
    int button = type == kCGEventLeftMouseDown ? 1 : type == kCGEventRightMouseDown ? 3 : 2;
    pthread_mutex_lock(&capture->lock);
    if (!capture->closed && capture->click_count < 64) {
        unsigned index = capture->click_count++;
        capture->clicks[index].point = CGEventGetLocation(event);
        capture->clicks[index].button = button;
        capture->clicks[index].at = cast_now_ns();
    }
    pthread_mutex_unlock(&capture->lock);
    return event;
}
static void *input_run(void *context)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)context;
    @autoreleasepool {
        CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL, capture->input_tap, 0);
        if (!source) return NULL;
        CFRunLoopRef loop = CFRunLoopGetCurrent();
        pthread_mutex_lock(&capture->lock);
        capture->input_loop = (CFRunLoopRef)CFRetain(loop);
        bool closed = capture->closed;
        pthread_mutex_unlock(&capture->lock);
        CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
        if (!closed) CFRunLoopRun();
        CFRunLoopRemoveSource(loop, source, kCFRunLoopCommonModes);
        CFRelease(source);
    }
    return NULL;
}
static CGDirectDisplayID selected_display(const Config *cfg)
{
    return (!cfg->monitor[0] || !strcmp(cfg->monitor, "main"))
        ? CGMainDisplayID() : (CGDirectDisplayID)strtoul(cfg->monitor, NULL, 10);
}
static Cursor pointer_at(CGPoint point, const Config *cfg, int width, int height, uint64_t at)
{
    CGRect bounds = CGDisplayBounds(selected_display(cfg));
    Cursor cursor = {.ts_ns = at};
    if (bounds.size.width > 0 && bounds.size.height > 0 && width > 0 && height > 0 &&
        CGRectContainsPoint(bounds, point)) {
        cursor.x = (int)((point.x - bounds.origin.x) * width / bounds.size.width);
        cursor.y = (int)((point.y - bounds.origin.y) * height / bounds.size.height);
        cursor.valid = true;
    }
    return cursor;
}
@implementation CastScreenCapture
- (instancetype)init
{
    self = [super init];
    if (self) {
        pthread_mutex_init(&lock, NULL);
        _samples = dispatch_queue_create("cast.screen.samples", DISPATCH_QUEUE_SERIAL);
        generation = 1;
        CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventOtherMouseDown);
        input_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
            kCGEventTapOptionListenOnly, mask, input_event, (__bridge void *)self);
        if (input_tap && !pthread_create(&input_thread, NULL, input_run, (__bridge void *)self))
            input_started = true;
    }
    return self;
}
- (void)dealloc
{
    if (input_started) [self shutdown];
    if (input_loop) CFRelease(input_loop);
    if (input_tap) CFRelease(input_tap);
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
    pthread_mutex_lock(&lock);
    CFRunLoopRef loop = input_loop;
    if (loop) CFRetain(loop);
    pthread_mutex_unlock(&lock);
    if (loop) { CFRunLoopStop(loop); CFRelease(loop); }
    if (input_started) { pthread_join(input_thread, NULL); input_started = false; }
    if (input_tap) CFMachPortInvalidate(input_tap);
}
- (void)begin
{
    pthread_mutex_lock(&lock);
    uint64_t request = ++epoch;
    accept_after = cast_now_ns();
    Config cfg = config;
    ready = false;
    failed = false;
    frame_free(&latest);
    SCStream *old = self.stream;
    self.stream = nil;
    pthread_mutex_unlock(&lock);
    [old stopCaptureWithCompletionHandler:^(NSError *error) { (void)error; }];
    __weak CastScreenCapture *weakSelf = self;
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent *content, NSError *error) {
        @autoreleasepool {
            CastScreenCapture *self = weakSelf;
            if (!self) return;
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
            size_t pixelWidth = 0, pixelHeight = 0;
            if (!display_pixels(display.displayID, &pixelWidth, &pixelHeight) ||
                pixelWidth > 8192 || pixelHeight > 8192) {
                self->failed = true;
                snprintf(self->failure, sizeof self->failure, "macOS display dimensions exceed capture bounds (8192 pixels per axis)");
                pthread_mutex_unlock(&self->lock);
                return;
            }
            /* Exclude every process in our app bundle: the panel and daemon are
             * separate processes, and a window ID alone does not cover both. */
            NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
            NSMutableArray<SCRunningApplication *> *excluded = [NSMutableArray array];
            for (SCRunningApplication *application in content.applications) {
                if (application.processID == getpid() ||
                    (bundle.length && [application.bundleIdentifier isEqualToString:bundle]))
                    [excluded addObject:application];
            }
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display
                excludingApplications:excluded exceptingWindows:@[]];
            SCStreamConfiguration *settings = [SCStreamConfiguration new];
            settings.width = pixelWidth;
            settings.height = pixelHeight;
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
                CastScreenCapture *self = weakSelf;
                if (!self) return;
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
    double age = CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
        CMSampleBufferGetPresentationTimeStamp(sample)));
    if (!isfinite(age) || age < -0.05 || age > 0.5) return;
    uint64_t now = cast_now_ns();
    uint64_t timestamp = age > 0 && now > (uint64_t)(age * 1e9)
        ? now - (uint64_t)(age * 1e9) : now;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || !CFArrayGetCount(attachments)) return;
    NSDictionary *metadata = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = metadata[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) {
        pthread_mutex_lock(&lock);
        if (!closed && stream == self.stream && timestamp >= accept_after) {
            if (status && status.integerValue == SCFrameStatusIdle && ready)
                latest.ts_ns = timestamp; /* Unchanged desktop is still live. */
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
    if (!closed && !failed && stream == self.stream && timestamp >= accept_after && frame_alloc(&latest, (int)width, (int)height) == 0) {
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
            latest.ts_ns = timestamp;
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
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    bool exclusion = NSBundle.mainBundle.bundleIdentifier.length > 0;
    Capabilities cap = {.capture = true, .cursor_metadata = true, .input = capture->input_started, .embedded_cursor = true, .preview = true, .panel_exclusion = exclusion};
    snprintf(cap.description, sizeof cap.description, "ScreenCaptureKit monitor capture; embedded cursor; %s; native preview; pointer metadata; click input requires Input Monitoring",
        exclusion ? "Cast app bundle excluded" : "bundle exclusion unavailable outside .app");
    return cap;
}
uint64_t macos_source_generation(Platform *p)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    pthread_mutex_lock(&capture->lock);
    uint64_t generation = capture->generation;
    pthread_mutex_unlock(&capture->lock);
    return generation;
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
    Config cfg = capture->config;
    pthread_mutex_unlock(&capture->lock);
    *cursor = (Cursor){0};
    if (!result) {
        CGEventRef event = CGEventCreate(NULL);
        if (event) {
            *cursor = pointer_at(CGEventGetLocation(event), &cfg, frame->width, frame->height,
                                 frame->ts_ns);
            CFRelease(event);
        }
    }
    return result;
}
int macos_reconfigure(Platform *p, const Config *cfg, char *error, size_t size)
{
    if (mac_config(cfg, error, size)) return -1;
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    pthread_mutex_lock(&capture->lock);
    bool changed = strcmp(capture->config.monitor, cfg->monitor) || capture->config.fps != cfg->fps || capture->config.cursor != cfg->cursor;
    capture->config = *cfg;
    if (changed) capture->generation++;
    pthread_mutex_unlock(&capture->lock);
    if (changed) [capture begin];
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
            size_t width = 0, height = 0;
            if (!display_pixels(displays[i], &width, &height))
                return mac_error(error, size, "cannot read macOS display pixel dimensions");
            int n = snprintf(error + used, size - used, "%u%s: %zux%zu\n", displays[i],
                displays[i] == CGMainDisplayID() ? " (main)" : "", width, height);
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
    snprintf(error, size, "capture: ScreenCaptureKit; screen recording permission %s; monitor capture only; Cast .app automatically excluded; native preview available; no global input", CGPreflightScreenCaptureAccess() ? "granted" : "required");
}

void macos_panel_status(Platform *p, char *error, size_t size)
{
    (void)p;
    snprintf(error, size, "%s", NSBundle.mainBundle.bundleIdentifier.length
        ? "Cast app bundle automatically excluded from screen capture"
        : "unsupported outside .app: bundle identity required for panel exclusion");
}

int macos_panel_register(Platform *p, char *error, size_t size)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    /* The panel may have become visible since startup enumeration. Refresh the
     * application filter before accepting more source frames. Source generation
     * also retires compositor caches that might contain the panel. */
    pthread_mutex_lock(&capture->lock);
    capture->generation++;
    pthread_mutex_unlock(&capture->lock);
    [capture begin];
    macos_panel_status(p, error, size);
    return 0;
}

void macos_events(Platform *p, Compositor *comp, const Config *cfg, bool privacy)
{
    CastScreenCapture *capture = (__bridge CastScreenCapture *)p;
    pthread_mutex_lock(&capture->lock);
    for (unsigned i = 0; !privacy && i < capture->click_count; ++i) {
        Cursor point = pointer_at(capture->clicks[i].point, cfg, capture->latest.width,
                                  capture->latest.height, capture->clicks[i].at);
        if (point.valid)
            compositor_click(comp, point.x, point.y, capture->clicks[i].button, point.ts_ns);
    }
    capture->click_count = 0;
    pthread_mutex_unlock(&capture->lock);
}
