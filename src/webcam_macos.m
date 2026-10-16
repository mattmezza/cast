/* AVFoundation camera adapter. Compile with ARC. */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include "media_internal.h"
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

@interface CastCameraSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) CastCamera *owner;
@end
struct CastCamera {
    AVCaptureSession *session;
    AVCaptureVideoDataOutput *output;
    CastCameraSink *sink;
    dispatch_queue_t queue;
    pthread_mutex_t lock;
    Frame cached;
    uint64_t accept_after;
};
struct CastOutput { int unused; };
@implementation CastCameraSink
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output; (void)connection;
    CastCamera *c = self.owner;
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!c || !pixels || !CMSampleBufferIsValid(sample) ||
        CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA) return;
    size_t width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels);
    if (!width || !height || width > 8192 || height > 8192) return;
    /* AV capture timestamps use the host clock; translate its epoch into cast's
     * CLOCK_MONOTONIC epoch rather than stamping delayed frames as fresh. */
    double age = CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
                                                CMSampleBufferGetPresentationTimeStamp(sample)));
    if (!isfinite(age) || age < -0.05 || age > 0.5) return;
    uint64_t now = cast_now_ns();
    uint64_t ts = age > 0 ? now - (uint64_t)(age * 1e9) : now;
    if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixels);
    size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    if (!base || stride < width * 4) {
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        return;
    }
    pthread_mutex_lock(&c->lock);
    if (ts >= c->accept_after && frame_alloc(&c->cached, (int)CVPixelBufferGetWidth(pixels),
                                            (int)CVPixelBufferGetHeight(pixels)) == 0) {
        for (int y = 0; y < c->cached.height; ++y) {
            const uint8_t *s = base + y * stride;
            uint8_t *d = c->cached.data + y * c->cached.stride;
            for (int x = 0; x < c->cached.width; ++x) {
                d[4*x] = s[4*x+2]; d[4*x+1] = s[4*x+1];
                d[4*x+2] = s[4*x]; d[4*x+3] = 255;
            }
        }
        c->cached.ts_ns = ts;
    }
    pthread_mutex_unlock(&c->lock);
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}
@end
static bool camera_permission(void)
{
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusNotDetermined) {
        /* Never stall the daemon behind an unanswered system permission alert.
         * Camera can be enabled again after permission has been granted. */
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
            (void)granted;
        }];
    }
    return status == AVAuthorizationStatusAuthorized;
}
CastCamera *camera_open(const Config *cfg, char *error, size_t size)
{
    @autoreleasepool {
        if (!camera_permission()) { snprintf(error, size, "Camera permission required; grant it in System Settings > Privacy & Security > Camera, then enable the camera again"); return NULL; }
        AVCaptureDevice *device = nil;
        bool defaults = !cfg->camera_device[0] || !strcmp(cfg->camera_device, "default");
        for (AVCaptureDevice *candidate in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
            if (!defaults && !strcmp(candidate.uniqueID.UTF8String, cfg->camera_device)) device = candidate;
        }
        if (defaults) device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        NSError *err = nil;
        AVCaptureDeviceInput *input = device ? [AVCaptureDeviceInput deviceInputWithDevice:device error:&err] : nil;
        if (!input) { snprintf(error, size, "Camera unavailable: %s", err ? err.localizedDescription.UTF8String : "unknown device; use camera list"); return NULL; }
        CastCamera *c = calloc(1, sizeof(*c));
        if (!c) { snprintf(error, size, "Camera allocation failed"); return NULL; }
        pthread_mutex_init(&c->lock, NULL);
        c->session = [AVCaptureSession new]; c->sink = [CastCameraSink new]; c->sink.owner = c;
        c->queue = dispatch_queue_create("cast.camera", DISPATCH_QUEUE_SERIAL);
        c->output = [AVCaptureVideoDataOutput new];
        c->output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        c->output.alwaysDiscardsLateVideoFrames = YES;
        [c->output setSampleBufferDelegate:c->sink queue:c->queue];
        if (![c->session canAddInput:input] || ![c->session canAddOutput:c->output]) {
            snprintf(error, size, "Cannot configure camera capture session"); camera_close(c); return NULL;
        }
        [c->session addInput:input]; [c->session addOutput:c->output];
        [c->session startRunning];
        if (!c->session.running) { snprintf(error, size, "Camera session did not start"); camera_close(c); return NULL; }
        return c;
    }
}
void camera_close(CastCamera *c)
{
    if (!c) return;
    [c->session stopRunning]; [c->output setSampleBufferDelegate:nil queue:NULL];
    if (c->queue) dispatch_sync(c->queue, ^{});
    c->sink.owner = NULL; c->sink = nil; c->output = nil; c->session = nil; c->queue = nil;
    frame_free(&c->cached); pthread_mutex_destroy(&c->lock); free(c);
}
void camera_barrier(CastCamera *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock); c->accept_after = cast_now_ns(); frame_free(&c->cached); pthread_mutex_unlock(&c->lock);
}
bool camera_failed(CastCamera *c) { return c && !c->session.running; }
int camera_frame(CastCamera *c, Frame *out, char *error, size_t size)
{
    if (!c) { snprintf(error, size, "Camera unavailable"); return -1; }
    pthread_mutex_lock(&c->lock);
    int result = -1;
    if (c->cached.data && cast_now_ns() - c->cached.ts_ns <= 500000000) result = frame_copy(out, &c->cached);
    if (result < 0) snprintf(error, size, "Waiting for fresh camera frames");
    pthread_mutex_unlock(&c->lock); return result;
}
void camera_list(char *out, size_t size)
{
    @autoreleasepool {
        size_t used = 0; if (size) out[0] = 0;
        for (AVCaptureDevice *d in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
            if (used >= size) break;
            int n = snprintf(out + used, size - used, "%s %s\n", d.uniqueID.UTF8String, d.localizedName.UTF8String);
            if (n > 0) used += (size_t)n;
        }
    }
}
void camera_doctor(const Config *cfg, char *out, size_t size)
{ (void)cfg; snprintf(out, size, "macOS camera: AVFoundation; permission required. Virtual camera unavailable in this build.\n"); }
CastOutput *output_open(const Config *cfg, char *error, size_t size)
{
    if (cfg->virtual_enabled && strcmp(cfg->output_device, "none")) {
        snprintf(error, size, "Virtual camera is not supported on macOS yet; set output.enabled=false or use --no-virtual");
        return NULL;
    }
    CastOutput *output = calloc(1, sizeof(*output));
    if (!output) snprintf(error, size, "Output allocation failed");
    return output;
}
void output_close(CastOutput *output) { free(output); }
int output_frame(CastOutput *output, const Frame *frame, char *error, size_t size)
{ (void)output; (void)frame; (void)error; (void)size; return 0; }
