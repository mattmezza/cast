/* Native preview contract tests: no capture devices, TCC grants or user input. */
#import <AppKit/AppKit.h>
#include "preview_macos.h"
#include <assert.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    const char message[] = "native preview test watchdog expired\n";
    write(STDERR_FILENO, message, sizeof message - 1);
    _exit(124);
}
static NSView *image_view(NSWindow *window)
{
    for (NSView *view in window.contentView.subviews)
        if (![view isKindOfClass:NSSegmentedControl.class]) return view;
    assert(!"missing preview image view");
    return nil;
}
static NSSegmentedControl *selector(NSWindow *window)
{
    for (NSView *view in window.contentView.subviews)
        if ([view isKindOfClass:NSSegmentedControl.class]) return (NSSegmentedControl *)view;
    assert(!"missing preview selector");
    return nil;
}
static NSBitmapImageRep *snapshot(NSView *view)
{
    NSBitmapImageRep *image = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)view.bounds.size.width
        pixelsHigh:(NSInteger)view.bounds.size.height bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:0 bitsPerPixel:0];
    assert(image);
    NSGraphicsContext *context = [NSGraphicsContext graphicsContextWithBitmapImageRep:image];
    [view displayRectIgnoringOpacity:view.bounds inContext:context];
    return image;
}
static void color_at(NSBitmapImageRep *image, double x, double y, int r, int g, int b)
{
    NSColor *color = [[image colorAtX:(NSInteger)(x * image.pixelsWide)
                                   y:(NSInteger)(y * image.pixelsHigh)]
                     colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
    assert(color);
    int red = (int)lround(color.redComponent * 255);
    int green = (int)lround(color.greenComponent * 255);
    int blue = (int)lround(color.blueComponent * 255);
    if (abs(red-r) > 4 || abs(green-g) > 4 || abs(blue-b) > 4)
        fprintf(stderr, "pixel %.2f,%.2f: got %d,%d,%d expected %d,%d,%d\n", x,y,red,green,blue,r,g,b);
    assert(abs(red-r) <= 4 && abs(green-g) <= 4 && abs(blue-b) <= 4);
}
static void mouse_event(CastMacPreview *preview, NSWindow *window, NSEventType type,
                        NSPoint point)
{
    NSEvent *event = [NSEvent mouseEventWithType:type location:point modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
        context:nil eventNumber:1 clickCount:1 pressure:type == NSEventTypeLeftMouseDown ? 1 : 0];
    assert(event);
    [NSApp postEvent:event atStart:YES];
    mac_preview_events(preview);
    /* mouseDown must return before any mouseUp is posted. The process watchdog
     * catches a nested tracking loop; AppKit startup on a shared CI runner is
     * not subject to an arbitrary half-second performance requirement. */
}
int main(void)
{
    signal(SIGALRM, timeout_handler);
    alarm(30);
    @autoreleasepool {
        CastMacPreview *preview = mac_preview_open();
        assert(preview && mac_preview_target(preview) == -1);
        Config cfg = {0}; State state = {0}; char error[CAST_ERR];
        cfg.preview = true;
        strcpy(cfg.preview_target, "composition");
        /* Padded rows catch stride errors. Top quadrants are red/green, bottom blue/white. */
        uint8_t pixels[16 * 68];
        memset(pixels,99,sizeof pixels);
        for (int y=0;y<16;y++) for (int x=0;x<16;x++) {
            uint8_t *p = pixels+y*68+x*4;
            p[0] = y<8 ? (x<8 ? 255 : 0) : (x<8 ? 0 : 255);
            p[1] = x<8 ? 0 : 255;
            p[2] = y<8 ? 0 : 255;
            p[3] = 255;
        }
        Frame frame = {.data=pixels,.width=16,.height=16,.stride=68};
        assert(!mac_preview_frame(preview,&frame,&state,&cfg,error,sizeof error));
        mac_preview_events(preview);
        NSWindow *window = NSApp.windows.firstObject;
        assert(window && window.visible);
        NSView *view = image_view(window);
        assert([window.title containsString:@"Composition"]);
        memset(pixels, 0, sizeof pixels); /* Submission must own the original bytes. */
        NSString *liveTitle = [window.title copy];
        for (unsigned i=0;i<3;i++)
            assert(!mac_preview_countdown(preview,0,error,sizeof error));
        assert(window.visible && [window.title isEqualToString:liveTitle]);
        NSBitmapImageRep *image = snapshot(view);
        color_at(image,.4,.25,255,0,0);
        color_at(image,.6,.25,0,255,0);
        color_at(image,.4,.75,0,0,255);
        color_at(image,.6,.75,255,255,255);
        color_at(image,.02,.5,20,20,20); /* Square frame is letterboxed. */
        [window setContentSize:NSMakeSize(400,600)];
        mac_preview_events(preview);
        image = snapshot(view);
        color_at(image,.5,.02,20,20,20); /* Resizing moves bars to top/bottom. */
        color_at(image,.25,.4,255,0,0);
        color_at(image,.75,.6,255,255,255);

        uint8_t neutral[16] = {32,37,43,255,32,37,43,255,32,37,43,255,32,37,43,255};
        Frame private_frame = {.data=neutral,.width=2,.height=2,.stride=8};
        state.group_paused = true;
        assert(!mac_preview_frame(preview,&private_frame,&state,&cfg,error,sizeof error));
        assert([window.title containsString:@"Paused"]);
        image = snapshot(view);
        color_at(image,.3,.4,32,37,43);
        color_at(image,.7,.6,32,37,43);
        NSSegmentedControl *control = selector(window);
        control.selectedSegment = 1;
        [NSApp sendAction:control.action to:control.target from:control];
        assert(mac_preview_target(preview) == 1 && mac_preview_target(preview) == -1);
        color_at(snapshot(view),.5,.5,20,20,20); /* Selection clears old imagery. */
        control.selectedSegment = 0;
        [NSApp sendAction:control.action to:control.target from:control];
        assert(mac_preview_target(preview) == 3 && mac_preview_target(preview) == -1);
        NSRect segments = [control convertRect:control.bounds toView:nil];
        NSPoint recording = NSMakePoint(NSMinX(segments)+NSWidth(segments)*.75,
                                        NSMidY(segments));
        mouse_event(preview,window,NSEventTypeLeftMouseDown,recording);
        assert(mac_preview_target(preview) == 1 && mac_preview_target(preview) == -1);
        mouse_event(preview,window,NSEventTypeLeftMouseUp,recording);
        NSPoint composition = NSMakePoint(NSMinX(segments)+NSWidth(segments)*.25,
                                          NSMidY(segments));
        mouse_event(preview,window,NSEventTypeLeftMouseDown,composition);
        assert(mac_preview_target(preview) == 3 && mac_preview_target(preview) == -1);
        mouse_event(preview,window,NSEventTypeLeftMouseUp,composition);
        NSPoint title = NSMakePoint(200,NSHeight(window.contentView.bounds)+10);
        mouse_event(preview,window,NSEventTypeLeftMouseDown,title);
        mouse_event(preview,window,NSEventTypeLeftMouseDragged,title);
        mouse_event(preview,window,NSEventTypeLeftMouseUp,title);
        NSPoint grip = NSMakePoint(NSWidth(window.contentView.bounds)-2,2);
        mouse_event(preview,window,NSEventTypeLeftMouseDown,grip);
        mouse_event(preview,window,NSEventTypeLeftMouseDragged,grip);
        mouse_event(preview,window,NSEventTypeLeftMouseUp,grip);
        assert(window.visible && mac_preview_target(preview) == -1);
        Frame invalid = frame; invalid.stride = 1;
        assert(mac_preview_frame(preview,&invalid,&state,&cfg,error,sizeof error) < 0);
        color_at(snapshot(view),.5,.5,20,20,20); /* Invalid input cannot retain old pixels. */
        cfg.preview = false;
        assert(!mac_preview_frame(preview,NULL,&state,&cfg,error,sizeof error));
        assert(!window.visible);
        color_at(snapshot(view),.5,.5,20,20,20);
        cfg.preview = true;
        assert(!mac_preview_frame(preview,&private_frame,&state,&cfg,error,sizeof error));
        NSButton *close = [window standardWindowButton:NSWindowCloseButton];
        NSRect closeRect = [close convertRect:close.bounds toView:nil];
        NSPoint closePoint = NSMakePoint(NSMidX(closeRect),NSMidY(closeRect));
        mouse_event(preview,window,NSEventTypeLeftMouseDown,closePoint);
        assert(!window.visible);
        assert(mac_preview_target(preview) == -2 && mac_preview_target(preview) == -1);
        mouse_event(preview,window,NSEventTypeLeftMouseUp,closePoint);
        assert(!mac_preview_frame(preview,&private_frame,&state,&cfg,error,sizeof error));
        assert(window.visible);

        cfg.preview = false;
        assert(!mac_preview_countdown_frame(preview,&private_frame,&cfg,error,sizeof error));
        assert(!mac_preview_countdown(preview,2100000000,error,sizeof error));
        assert(window.visible && [window.title containsString:@"Recording in 3"]);
        /* The film guide must be painted in the view, not merely its title. */
        image = snapshot(view);
        color_at(image,.3,.4,10,11,13); /* Recording composition dimmed to 30%. */
        NSUInteger bright = 0;
        for (NSInteger y=image.pixelsHigh/3; y<image.pixelsHigh*2/3; y++)
            for (NSInteger x=image.pixelsWide/3; x<image.pixelsWide*2/3; x++) {
                NSColor *pixel = [[image colorAtX:x y:y]
                    colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
                if (pixel.redComponent > .9) bright++;
            }
        assert(bright > 100); /* Large centered countdown numeral. */
        assert(!mac_preview_countdown(preview,1,error,sizeof error));
        assert([window.title containsString:@"Recording in 1"]);
        assert(!mac_preview_countdown(preview,0,error,sizeof error));
        assert(!window.visible);
        color_at(snapshot(view),.5,.5,20,20,20);
        assert(!mac_preview_countdown_frame(preview,&private_frame,&cfg,error,sizeof error));
        NSEvent *escape = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil
            characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53];
        [NSApp postEvent:escape atStart:YES];
        mac_preview_events(preview);
        assert(mac_preview_countdown(preview,1000000000,error,sizeof error) == 1);
        assert(mac_preview_target(preview) == -2 && mac_preview_target(preview) == -1 &&
               !window.visible);
        assert(!mac_preview_countdown(preview,0,error,sizeof error));
        color_at(snapshot(view),.5,.5,20,20,20);
        assert(selector(window).enabled);
        cfg.preview = true;
        assert(!mac_preview_countdown_frame(preview,&private_frame,&cfg,error,sizeof error));
        assert(!mac_preview_countdown(preview,0,error,sizeof error));
        assert(window.visible); /* Ending a guide preserves an explicitly enabled preview. */
        color_at(snapshot(view),.5,.5,20,20,20);
        mac_preview_close(preview);
        mac_preview_close(NULL);
        assert(mac_preview_target(NULL) == -1);
    }
    alarm(0);
    puts("native preview: owned RGBA, orientation, resize, privacy, selectors, close and countdown passed");
    return 0;
}
