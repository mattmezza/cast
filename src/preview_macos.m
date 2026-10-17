/* Native preview UI is owned by the daemon's main thread. Frames are copied and
 * painted synchronously, so privacy barriers cannot leave stale queued paints. */
#import <AppKit/AppKit.h>
#include "preview_macos.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

@interface CastPreviewView : NSView {
    CGImageRef pixels;
}
@property(nonatomic) uint64_t countdownRemaining;
- (BOOL)replaceFrame:(const Frame *)frame;
- (void)clear;
@end
@implementation CastPreviewView
- (BOOL)replaceFrame:(const Frame *)frame
{
    [self clear];
    if (!frame || !frame->data || frame->width < 1 || frame->height < 1 ||
        frame->width > 8192 || frame->height > 8192 || frame->stride < frame->width * 4)
        return NO;
    CFDataRef bytes = CFDataCreate(NULL, frame->data, (CFIndex)frame->stride * frame->height);
    CGDataProviderRef provider = bytes ? CGDataProviderCreateWithCFData(bytes) : NULL;
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    if (provider && color)
        pixels = CGImageCreate(frame->width, frame->height, 8, 32, frame->stride, color,
            kCGBitmapByteOrder32Big | kCGImageAlphaLast, provider, NULL, false,
            kCGRenderingIntentDefault);
    if (color) CGColorSpaceRelease(color);
    if (provider) CGDataProviderRelease(provider);
    if (bytes) CFRelease(bytes);
    self.needsDisplay = YES;
    return pixels != NULL;
}
- (void)clear
{
    if (pixels) CGImageRelease(pixels);
    pixels = NULL;
    self.countdownRemaining = 0;
    self.needsDisplay = YES;
}
- (void)dealloc { if (pixels) CGImageRelease(pixels); }
- (BOOL)isOpaque { return YES; }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor colorWithWhite:0.08 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!pixels) return;
    CGFloat w = CGImageGetWidth(pixels), h = CGImageGetHeight(pixels);
    CGFloat scale = MIN(self.bounds.size.width / w, self.bounds.size.height / h);
    NSRect rect = NSMakeRect((self.bounds.size.width-w*scale)/2,
                            (self.bounds.size.height-h*scale)/2,w*scale,h*scale);
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, NSRectToCGRect(rect), pixels);
    if (self.countdownRemaining) {
        /* Local guide only: never modify the staged recording frame. */
        [[NSColor colorWithWhite:0 alpha:0.70] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        CGFloat cx = NSMidX(self.bounds), cy = NSMidY(self.bounds);
        CGFloat radius = MAX(34, MIN(self.bounds.size.width, self.bounds.size.height) / 3);
        [[NSColor colorWithWhite:0.75 alpha:1] setStroke];
        NSBezierPath *leader = [NSBezierPath bezierPath];
        [leader moveToPoint:NSMakePoint(0,cy)];
        [leader lineToPoint:NSMakePoint(NSWidth(self.bounds),cy)];
        [leader moveToPoint:NSMakePoint(cx,0)];
        [leader lineToPoint:NSMakePoint(cx,NSHeight(self.bounds))];
        [leader appendBezierPathWithOvalInRect:NSMakeRect(cx-radius,cy-radius,2*radius,2*radius)];
        [leader appendBezierPathWithOvalInRect:NSMakeRect(cx-radius+5,cy-radius+5,2*radius-10,2*radius-10)];
        double phase = (double)(1000000000 - self.countdownRemaining % 1000000000) / 1000000000;
        double angle = M_PI / 2 - 2 * M_PI * phase;
        [leader moveToPoint:NSMakePoint(cx,cy)];
        [leader lineToPoint:NSMakePoint(cx+cos(angle)*radius,cy+sin(angle)*radius)];
        [leader stroke];
        NSString *number = [NSString stringWithFormat:@"%llu",
            (unsigned long long)((self.countdownRemaining-1)/1000000000+1)];
        NSDictionary *attributes = @{NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:48
            weight:NSFontWeightSemibold], NSForegroundColorAttributeName:NSColor.whiteColor};
        NSSize textSize = [number sizeWithAttributes:attributes];
        [number drawAtPoint:NSMakePoint(cx-textSize.width/2,cy-textSize.height/2)
            withAttributes:attributes];
    }
}
@end

@interface CastPreviewController : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) CastPreviewView *view;
@property(nonatomic, strong) NSSegmentedControl *selector;
@property(nonatomic) int pending;
@property(nonatomic) BOOL countdown;
@property(nonatomic) Config config;
@property(nonatomic) int tracking;
@property(nonatomic) NSPoint dragStart;
@property(nonatomic) NSRect dragFrame;
- (void)choose:(NSSegmentedControl *)sender;
@end
@implementation CastPreviewController
- (instancetype)init
{
    self = [super init];
    if (self) _pending = -1;
    return self;
}
- (void)choose:(NSSegmentedControl *)sender
{ self.pending = sender.selectedSegment == 0 ? 3 : 1; [self.view clear]; [self.view displayIfNeeded]; }
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    self.tracking = 0;
    [self.view clear];
    [self.view displayIfNeeded];
    [sender orderOut:nil];
    self.pending = -2;
    return NO;
}
@end
struct CastMacPreview { const void *controller; };
static CastPreviewController *controller(CastMacPreview *p)
{ return (__bridge CastPreviewController *)p->controller; }
static int fail(char *error, size_t size, const char *message)
{ snprintf(error, size, "%s", message); return -1; }
CastMacPreview *mac_preview_open(void)
{
    CastMacPreview *p = calloc(1, sizeof *p);
    if (p) p->controller = CFBridgingRetain([CastPreviewController new]);
    return p;
}
void mac_preview_close(CastMacPreview *p)
{
    if (!p) return;
    @autoreleasepool {
        CastPreviewController *c = CFBridgingRelease(p->controller);
        [c.view clear]; c.window.delegate = nil; [c.window orderOut:nil]; [c.window close];
        free(p);
    }
}
/* AppKit's normal mouseDown can run a nested tracking loop until mouse release.
 * The daemon must keep capturing and handling privacy commands meanwhile, so
 * this simple window handles its mouse gestures as individual queued events. */
static BOOL mouse_event(CastPreviewController *c, NSEvent *event)
{
    NSEventType type = event.type;
    if (type == NSEventTypeLeftMouseUp && c.tracking) { c.tracking = 0; return YES; }
    if (type == NSEventTypeLeftMouseDragged && c.tracking) {
        NSPoint now = NSEvent.mouseLocation;
        CGFloat dx = now.x-c.dragStart.x, dy = now.y-c.dragStart.y;
        NSRect frame = c.dragFrame;
        if (c.tracking == 1) {
            frame.origin.x += dx; frame.origin.y += dy;
        } else {
            if (c.tracking & 4) {
                CGFloat width = MAX(340,frame.size.width-dx);
                frame.origin.x += frame.size.width-width;
                frame.size.width = width;
            }
            if (c.tracking & 8) frame.size.width = MAX(340,frame.size.width+dx);
            if (c.tracking & 16) {
                CGFloat height = MAX(260,frame.size.height-dy);
                frame.origin.y += frame.size.height-height;
                frame.size.height = height;
            }
            if (c.tracking & 32) frame.size.height = MAX(260,frame.size.height+dy);
        }
        [c.window setFrame:frame display:YES];
        return YES;
    }
    if (event.window != c.window) return NO;
    if (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp ||
        type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseUp ||
        type == NSEventTypeLeftMouseUp || type == NSEventTypeLeftMouseDragged) return YES;
    if (type != NSEventTypeLeftMouseDown) return NO;
    NSPoint point = event.locationInWindow;
    NSWindow *window = c.window;
    if (NSPointInRect(point,[c.selector convertRect:c.selector.bounds toView:nil])) {
        if (c.countdown) return YES;
        NSRect rect = [c.selector convertRect:c.selector.bounds toView:nil];
        c.selector.selectedSegment = point.x < NSMidX(rect) ? 0 : 1;
        [c choose:c.selector];
    } else {
        NSButton *close = [window standardWindowButton:NSWindowCloseButton];
        NSButton *mini = [window standardWindowButton:NSWindowMiniaturizeButton];
        NSButton *zoom = [window standardWindowButton:NSWindowZoomButton];
        if (NSPointInRect(point,[close convertRect:close.bounds toView:nil]))
            [c windowShouldClose:window];
        else if (NSPointInRect(point,[mini convertRect:mini.bounds toView:nil]))
            [window miniaturize:nil];
        else if (NSPointInRect(point,[zoom convertRect:zoom.bounds toView:nil]))
            [window zoom:nil];
        else {
            c.tracking = (point.x <= 8 ? 4 : point.x >= window.frame.size.width-8 ? 8 : 0) |
                         (point.y <= 8 ? 16 : point.y >= window.frame.size.height-8 ? 32 : 0);
            if (!c.tracking && point.y >= window.contentView.bounds.size.height)
                c.tracking = 1; /* Title-bar drag. */
        }
        if (c.tracking) { c.dragStart = NSEvent.mouseLocation; c.dragFrame = window.frame; }
    }
    return YES;
}
void mac_preview_events(CastMacPreview *p)
{
    if (!p || !controller(p).window) return;
    @autoreleasepool {
        /* Bound event work to keep recording and IPC responsive during dragging. */
        for (unsigned i=0;i<32;i++) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:NSDate.distantPast
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!event) break;
            CastPreviewController *c = controller(p);
            if (c.countdown && event.window == c.window && event.type == NSEventTypeKeyDown &&
                event.keyCode == 53) {
                [c windowShouldClose:c.window];
                continue;
            }
            if (mouse_event(c,event)) continue;
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}
int mac_preview_target(CastMacPreview *p)
{
    if (!p) return -1;
    CastPreviewController *c = controller(p);
    if (c.countdown && c.pending == -2) return -1; /* Countdown consumes cancellation. */
    int pending = c.pending; c.pending = -1;
    return pending;
}
static void create_window(CastPreviewController *c)
{
    if (c.window) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,580)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.delegate = c;
    window.contentMinSize = NSMakeSize(320,220);
    CastPreviewView *view = [[CastPreviewView alloc] initWithFrame:NSMakeRect(0,0,960,540)];
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [window.contentView addSubview:view];
    NSSegmentedControl *selector = [NSSegmentedControl segmentedControlWithLabels:@[@"Composition",@"Recording"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:c action:@selector(choose:)];
    selector.frame = NSMakeRect(12,548,300,24);
    selector.autoresizingMask = NSViewMinYMargin;
    [window.contentView addSubview:selector];
    [window center];
    c.window = window; c.view = view; c.selector = selector;
}
int mac_preview_frame(CastMacPreview *p, const Frame *frame, const State *state,
                      const Config *cfg, char *error, size_t size)
{
    if (!p) return fail(error,size,"cannot allocate macOS preview");
    @autoreleasepool {
        CastPreviewController *c = controller(p);
        if (c.countdown) return 0; /* Its staged guide owns the window this tick. */
        c.config = *cfg;
        if (!cfg->preview && !c.countdown) {
            c.tracking = 0;
            [c.view clear]; [c.view displayIfNeeded]; [c.window orderOut:nil]; return 0;
        }
        create_window(c);
        BOOL composition = !strcmp(cfg->preview_target,"composition");
        c.selector.selectedSegment = composition ? 0 : !strcmp(cfg->preview_target,"record") ? 1 : -1;
        NSString *label = composition ? (state->group_paused ? @"Composition — Paused" : @"Composition — local preview")
            : !strcmp(cfg->preview_target,"record") ? (!state->recording ? @"Recording — Stopped"
                : state->record_paused ? @"Recording — Paused" : @"Recording")
            : !strcmp(cfg->preview_target,"stream") ? @"Stream output" : @"Virtual output";
        if (!c.countdown) c.window.title = [@"SuperScreencast Preview — " stringByAppendingString:label];
        if (![c.view replaceFrame:frame]) {
            [c.view displayIfNeeded];
            return fail(error,size,"cannot copy macOS preview frame");
        }
        [c.view displayIfNeeded];
        if (!c.window.visible) [c.window makeKeyAndOrderFront:nil];
        return 0;
    }
}
int mac_preview_countdown_frame(CastMacPreview *p, const Frame *frame, const Config *cfg,
                                char *error, size_t size)
{
    if (!p) return fail(error,size,"cannot allocate macOS preview");
    CastPreviewController *c = controller(p);
    if (c.countdown && c.pending == -2) return 0;
    /* Keep the overlay while replacing its owned background frame. */
    c.countdown = NO;
    Config visible = *cfg;
    visible.preview = true;
    State state = {0};
    int result = mac_preview_frame(p,frame,&state,&visible,error,size);
    c.config = *cfg;
    c.countdown = YES;
    c.selector.enabled = NO;
    return result;
}
int mac_preview_countdown(CastMacPreview *p, uint64_t remaining, char *error, size_t size)
{
    (void)error; (void)size;
    if (!p) return 0;
    @autoreleasepool {
    CastPreviewController *c = controller(p);
    /* The daemon reports zero on every ordinary tick. Only retire a guide on
     * its transition to zero, or the live image would flash black each frame. */
    if (!remaining && !c.countdown) return 0;
    if (remaining) {
        c.view.countdownRemaining = remaining;
        c.view.needsDisplay = YES;
        [c.view displayIfNeeded];
        c.window.title = [NSString stringWithFormat:@"SuperScreencast — Recording in %llu",
            (unsigned long long)((remaining+999999999)/1000000000)];
        if (c.pending == -2) {
            c.countdown = NO;
            c.selector.enabled = YES;
            return 1; /* Keep dismissal pending so the daemon also turns preview off. */
        }
    } else {
        c.view.countdownRemaining = 0;
        c.countdown = NO;
        c.selector.enabled = YES;
        [c.view clear]; [c.view displayIfNeeded];
        if (!c.config.preview) [c.window orderOut:nil];
    }
    return 0;
    }
}
