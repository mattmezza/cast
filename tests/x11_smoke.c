/* Default invocation is read-only; --exercise MUST run in an isolated Xvfb display. */
#include "cast.h"
#define Cursor X11Cursor
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#undef Cursor
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t cast_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static Config configuration(void)
{
    Config c = {0};
    strcpy(c.backend, "xorg");
    strcpy(c.capture_kind, "monitor");
    strcpy(c.layout, "screen");
    strcpy(c.fit, "contain");
    strcpy(c.keys_mode, "shortcuts");
    strcpy(c.keys_navigation, "Left,Right,Home,End,Tab,Return,Escape");
    strcpy(c.keys_position, "bottom-left");
    strcpy(c.preview_target, "virtual");
    c.width = 256;
    c.height = 128;
    c.fps = 30;
    c.zoom_factor = 1;
    c.zoom_max = 8;
    c.zoom_smoothing = 1;
    c.keys_timeout_ms = 1000;
    c.keys_font_size = 14;
    c.keys_color = 0xffffff;
    c.pause_color = 0x253647;
    c.annotations_virtual_keys = c.annotations_record_keys = true;
    c.annotations_virtual_clicks = c.annotations_record_clicks = true;
    return c;
}

static uint32_t pixel(const Frame *f, int x, int y)
{
    const uint8_t *p = f->data + (size_t)y * f->stride + 4 * x;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static void command(Platform *p, Config *cfg, int argc, char **args)
{
    char error[CAST_ERR];
    int rc = platform_command(p, cfg, argc, args, error, sizeof(error));
    if (rc < 0) {
        fprintf(stderr, "command failed: %s\n", error);
    }
    assert(rc == 0);
}

static void pump(Display *injector, Platform *p, Config *cfg, Compositor *c, bool privacy)
{
    XSync(injector, False);
    usleep(10000);
    platform_events(p, c, cfg, privacy);
}

static void fill_source(Frame *f)
{
    assert(frame_alloc(f, 256, 128) == 0);
    for (int y = 0; y < f->height; y++) {
        for (int x = 0; x < f->width; x++) {
            uint8_t *p = f->data + y * f->stride + 4 * x;
            p[0] = 0x22;
            p[1] = 0x33;
            p[2] = 0x44;
            p[3] = 255;
        }
    }
    f->ts_ns = cast_now_ns();
}

static bool has_label(Compositor *c, Config *cfg, const char *label)
{
    Frame source = {0}, actual = {0}, expected = {0};
    fill_source(&source);
    char error[CAST_ERR];
    Compositor *reference = compositor_create();
    if (label) {
        compositor_key(reference, label, source.ts_ns, cfg->keys_timeout_ms);
    }
    assert(compositor_render(c, cfg, &source, NULL, NULL, false, &actual, error, sizeof(error)) ==
           0);
    assert(compositor_render(reference, cfg, &source, NULL, NULL, false, &expected, error,
                             sizeof(error)) == 0);
    bool same = memcmp(actual.data, expected.data, (size_t)actual.stride * actual.height) == 0;
    frame_free(&source);
    frame_free(&actual);
    frame_free(&expected);
    compositor_destroy(reference);
    return same;
}

static void press(Display *d, KeySym symbol, bool down)
{
    KeyCode code = XKeysymToKeycode(d, symbol);
    assert(code);
    assert(XTestFakeKeyEvent(d, code, down, CurrentTime));
}

static Window named_window(Display *d, const char *title)
{
    Window root, parent, *children = NULL, result = None;
    unsigned count = 0;
    assert(XQueryTree(d, DefaultRootWindow(d), &root, &parent, &children, &count));
    for (unsigned i = 0; i < count; i++) {
        char *name = NULL;
        if (XFetchName(d, children[i], &name) && name && !strcmp(name, title)) {
            result = children[i];
        }
        if (name) {
            XFree(name);
        }
    }
    if (children) {
        XFree(children);
    }
    return result;
}

static Window wait_preview(Display *d, Platform *p, Config *cfg, Compositor *c, Frame *frame,
                           State *state)
{
    char error[CAST_ERR];
    for (int i = 0; i < 100; i++) {
        pump(d, p, cfg, c, true);
        assert(platform_preview(p, frame, state, cfg, error, sizeof(error)) == 0);
        XSync(d, false);
        Window win = named_window(d, "cast output preview");
        XWindowAttributes attr;
        if (win && XGetWindowAttributes(d, win, &attr) && attr.map_state == IsViewable) {
            return win;
        }
        usleep(10000);
    }
    assert(!"preview worker did not paint/map within timeout");
    return None;
}

static void assert_preview_mask(const Frame *frame, const XWindowAttributes *attr, int source_x,
                                int source_y, uint32_t color)
{
    int left = attr->x - source_x, top = attr->y - source_y;
    int right = left + attr->width + 2 * attr->border_width;
    int bottom = top + attr->height + 2 * attr->border_width;
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            bool covered = x >= left && x < right && y >= top && y < bottom;
            assert(pixel(frame, x, y) == (covered ? color : 0));
            assert(frame->data[(size_t)y * frame->stride + x * 4 + 3] == 255);
        }
    }
}

static void assert_utility(Display *d, Window window)
{
    Atom property = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False), type;
    unsigned char *value = NULL;
    int format;
    unsigned long count, left;
    assert(XGetWindowProperty(d, window, property, 0, 1, False, XA_ATOM, &type, &format, &count,
                              &left, &value) == Success);
    assert(type == XA_ATOM && format == 32 && count == 1 && !left);
    assert(*(Atom *)value == XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", False));
    XFree(value);
}

static void preview_tests(Display *d, Platform *p, Config *cfg, Compositor *c)
{
    Frame source = {0}, capture = {0};
    fill_source(&source);
    Cursor cursor;
    char error[CAST_ERR];
    State state = {.virtual_paused = true};
    cfg->preview = true;
    Window preview = wait_preview(d, p, cfg, c, &source, &state);
    XWindowAttributes attr;
    assert(XGetWindowAttributes(d, preview, &attr) && !attr.override_redirect);
    assert_utility(d, preview);
    assert(attr.x == (DisplayWidth(d, DefaultScreen(d)) - attr.width) / 2);
    assert(attr.y == (DisplayHeight(d, DefaultScreen(d)) - attr.height) / 2);
    XClassHint hint;
    assert(XGetClassHint(d, preview, &hint));
    assert(!strcmp(hint.res_name, "cast-preview") && !strcmp(hint.res_class, "CastPreview"));
    XFree(hint.res_name);
    XFree(hint.res_class);
    XImage *header = XGetImage(d, preview, 0, attr.height - 32, 32, 32, AllPlanes, ZPixmap);
    assert(header && XGetPixel(header, 1, 1) == 0x111111);
    XDestroyImage(header);
    /* Native target controls select lanes without exposing UI pixels in output. */
    XEvent choose = {0};
    choose.xbutton.type = ButtonPress;
    choose.xbutton.display = d;
    choose.xbutton.window = preview;
    choose.xbutton.button = Button1;
    choose.xbutton.x = attr.width * 5 / 6;
    choose.xbutton.y = attr.height - 48;
    assert(XSendEvent(d, preview, False, ButtonPressMask, &choose));
    pump(d, p, cfg, c, true);
    assert(platform_preview_target(p) == 2);
    assert(platform_preview_target(p) == -1);
    choose.xkey.type = KeyPress;
    choose.xkey.keycode = XKeysymToKeycode(d, XK_1);
    choose.xkey.state = 0;
    assert(XSendEvent(d, preview, False, KeyPressMask, &choose));
    pump(d, p, cfg, c, true);
    assert(platform_preview_target(p) == 0);
    XSetWindowBorderWidth(d, preview, 3);
    XSetWindowBorder(d, preview, 0xff0000);
    XMoveResizeWindow(d, preview, 70, 90, 210, 140);
    pump(d, p, cfg, c, true);
    XSelectInput(d, preview, StructureNotifyMask);
    XSelectInput(d, DefaultRootWindow(d), SubstructureNotifyMask);
    XSync(d, false);
    while (XPending(d)) {
        XEvent event;
        XNextEvent(d, &event);
    }
    for (int phase = 0; phase < 5; phase++) {
        state.virtual_paused = phase == 0;
        state.virtual_frozen = phase == 2;
        state.recording = phase >= 3;
        state.record_paused = phase == 4;
        strcpy(cfg->preview_target, phase >= 3 ? "record" : "virtual");
        for (int i = 0; i < 4; i++) {
            pump(d, p, cfg, c, state.virtual_paused);
            assert(platform_preview(p, &source, &state, cfg, error, sizeof(error)) == 0);
            assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
            assert(XGetWindowAttributes(d, preview, &attr) && attr.map_state == IsViewable);
            assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
            XSync(d, false);
            while (XPending(d)) {
                XEvent event;
                XNextEvent(d, &event);
                assert(event.type != MapNotify || event.xmap.window != preview);
                assert(event.type != UnmapNotify || event.xunmap.window != preview);
            }
        }
    }
    /* An in-flight old epoch cannot repaint after the synchronous privacy barrier. */
    pump(d, p, cfg, c, false);
    for (int i = 0; i < 8; i++) {
        assert(platform_preview(p, &source, &state, cfg, error, sizeof error) == 0);
    }
    pump(d, p, cfg, c, true);
    for (int i = 0; i < 4; i++) {
        XImage *neutral = XGetImage(d, preview, 20, 40, 1, 1, AllPlanes, ZPixmap);
        assert(neutral && XGetPixel(neutral, 0, 0) == 0x111111);
        XDestroyImage(neutral);
        usleep(10000);
    }
    /* Managed decorations are masked along with the preview client. */
    Window decoration =
        XCreateSimpleWindow(d, DefaultRootWindow(d), 35, 45, 240, 180, 3, 0x112233, 0x556677);
    XReparentWindow(d, preview, decoration, 10, 25);
    XMapWindow(d, decoration);
    XSync(d, false);
    XWindowAttributes decorated;
    assert(XGetWindowAttributes(d, decoration, &decorated));
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    assert_preview_mask(&capture, &decorated, 0, 0, cfg->pause_color);
    XReparentWindow(d, preview, DefaultRootWindow(d), attr.x, attr.y);
    XDestroyWindow(d, decoration);
    XSync(d, false);
    /* Region clipping uses current geometry rather than creation coordinates. */
    char *region[] = {"capture", "region", "100", "110", "140", "100"};
    command(p, cfg, 6, region);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 100, 110, cfg->pause_color);
    char *monitor[] = {"capture", "monitor"};
    command(p, cfg, 2, monitor);
    XMoveResizeWindow(d, preview, 270, 210, 240, 150);
    pump(d, p, cfg, c, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(XGetWindowAttributes(d, preview, &attr));
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    /* Dragging the local header works without involving a tiling manager. */
    XTestFakeMotionEvent(d, DefaultScreen(d), attr.x + 13, attr.y + 13, CurrentTime);
    XTestFakeButtonEvent(d, 1, true, CurrentTime);
    pump(d, p, cfg, c, false);
    XTestFakeMotionEvent(d, DefaultScreen(d), attr.x + 53, attr.y + 43, CurrentTime);
    pump(d, p, cfg, c, false);
    XTestFakeButtonEvent(d, 1, false, CurrentTime);
    pump(d, p, cfg, c, false);
    assert(XGetWindowAttributes(d, preview, &attr) && attr.x == 310 && attr.y == 240);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    /* Application pixmap capture needs no footprint mask even when overlapped. */
    Window app = XCreateSimpleWindow(d, DefaultRootWindow(d), 300, 230, 140, 90, 0, 0, 0x778899);
    Atom wm_state = XInternAtom(d, "WM_STATE", false);
    unsigned long app_state[2] = {1, 0};
    XChangeProperty(d, app, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)app_state, 2);
    Atom active = XInternAtom(d, "_NET_ACTIVE_WINDOW", false);
    XChangeProperty(d, DefaultRootWindow(d), active, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&app, 1);
    XMapWindow(d, app);
    XSync(d, false);
    char *select[] = {"capture", "window", "active"};
    command(p, cfg, 3, select);
    XClearWindow(d, app);
    XRaiseWindow(d, preview);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    for (int y = 0; y < capture.height; y++) {
        for (int x = 0; x < capture.width; x++) {
            assert(pixel(&capture, x, y) == 0x778899);
        }
    }
    command(p, cfg, 2, monitor);
    XDestroyWindow(d, app);
    XSync(d, false);
    cfg->preview = false;
    assert(platform_preview(p, &source, &state, cfg, error, sizeof(error)) == 0);
    XSync(d, false);
    assert(XGetWindowAttributes(d, preview, &attr) && attr.map_state == IsUnmapped);
    cfg->preview = true;
    wait_preview(d, p, cfg, c, &source, &state);
    cfg->preview = false;
    assert(platform_preview(p, &source, &state, cfg, error, sizeof(error)) == 0);
    frame_free(&source);
    frame_free(&capture);
}

/* A fake WM frame exercises decoration exclusion without a desktop WM. */
static void panel_tests(Display *d, Platform *p, Config *cfg, Compositor *comp)
{
    char error[CAST_ERR], status[CAST_ERR];
    Frame capture = {0}, virtual = {0}, recording = {0};
    Cursor cursor;
    Window root = DefaultRootWindow(d);
    Window frame = XCreateSimpleWindow(d, root, 60, 70, 230, 150, 3, 0x123456, 0x456789);
    Window panel = XCreateSimpleWindow(d, frame, 10, 25, 210, 115, 0, 0, 0xff3377);
    XClassHint hint = {.res_name = "cast-panel", .res_class = "CastPanel"};
    XSetClassHint(d, panel, &hint);
    Atom pid_atom = XInternAtom(d, "_NET_WM_PID", false);
    Atom type_atom = XInternAtom(d, "_NET_WM_WINDOW_TYPE", false);
    Atom normal = XInternAtom(d, "_NET_WM_WINDOW_TYPE_NORMAL", false);
    unsigned long pid = (unsigned long)getpid();
    XChangeProperty(d, panel, pid_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    XChangeProperty(d, panel, type_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)&normal, 1);
    XMapWindow(d, panel);
    XMapWindow(d, frame);
    XSync(d, false);
    assert(platform_capabilities(p).panel_exclusion);
    assert(platform_panel_register(p, panel, (int)pid + 1, error, sizeof(error)) < 0);
    assert(platform_panel_register(p, root, (int)pid, error, sizeof(error)) < 0);
    hint.res_class = "SomeOtherApplication";
    XSetClassHint(d, panel, &hint);
    XSync(d, false);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) < 0);
    hint.res_class = "CastPanel";
    XSetClassHint(d, panel, &hint);
    Atom dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", false);
    XChangeProperty(d, panel, type_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);
    XSync(d, false);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) < 0);
    XChangeProperty(d, panel, type_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)&normal, 1);
    XSync(d, false);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) == 0);
    Atom utility = XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", false);
    XChangeProperty(d, panel, type_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)&utility,
                    1);
    XSync(d, false);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) == 0);
    assert(platform_panel_register(p, panel, (int)pid + 1, error, sizeof(error)) < 0);

    XWindowAttributes attr;
    assert(XGetWindowAttributes(d, frame, &attr));
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "overlap") && strstr(status, "neutral-masked"));
    /* Both output lanes receive already-excluded pixels through zoom and layout. */
    Config output = *cfg;
    output.width = capture.width;
    output.height = capture.height;
    output.zoom_factor = 2;
    output.zoom_follow = false;
    output.keys = output.clicks = output.cursor = false;
    assert(compositor_render(comp, &output, &capture, NULL, NULL, false, &virtual, error,
                             sizeof(error)) == 0);
    assert(compositor_render(comp, &output, &capture, NULL, NULL, true, &recording, error,
                             sizeof(error)) == 0);
    assert(!memcmp(virtual.data, recording.data, (size_t)virtual.stride * virtual.height));
    for (int y = 0; y < virtual.height; y++) {
        for (int x = 0; x < virtual.width; x++) {
            assert(pixel(&virtual, x, y) == 0 || pixel(&virtual, x, y) == cfg->pause_color);
        }
    }
    XSelectInput(d, panel, StructureNotifyMask);
    XSelectInput(d, frame, StructureNotifyMask);
    XSync(d, false);
    while (XPending(d)) {
        XEvent event;
        XNextEvent(d, &event);
    }
    for (int i = 0; i < 5; i++) {
        assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    }
    XSync(d, false);
    while (XPending(d)) {
        XEvent event;
        XNextEvent(d, &event);
        assert(event.type != MapNotify && event.type != UnmapNotify);
    }
    XMoveResizeWindow(d, frame, 120, 130, 260, 190);
    XMoveResizeWindow(d, panel, 10, 25, 240, 155);
    XSync(d, false);
    assert(XGetWindowAttributes(d, frame, &attr));
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    Window new_frame = XCreateSimpleWindow(d, root, 200, 170, 300, 220, 5, 0x123456, 0x456789);
    XReparentWindow(d, panel, new_frame, 10, 25);
    XMapWindow(d, new_frame);
    XUnmapWindow(d, frame);
    XSync(d, false);
    assert(XGetWindowAttributes(d, new_frame, &attr));
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    XReparentWindow(d, panel, frame, 10, 25);
    XMapWindow(d, frame);
    XDestroyWindow(d, new_frame);
    XSync(d, false);
    assert(XGetWindowAttributes(d, frame, &attr));
    char *region[] = {"capture", "region", "150", "150", "140", "110"};
    command(p, cfg, 6, region);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 150, 150, cfg->pause_color);
    char *clipped[] = {"capture", "region", "350", "290", "140", "110"};
    command(p, cfg, 6, clipped);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert_preview_mask(&capture, &attr, 350, 290, cfg->pause_color);
    XMoveWindow(d, frame, 600, 450);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(XGetWindowAttributes(d, frame, &attr));
    assert_preview_mask(&capture, &attr, 350, 290, cfg->pause_color);
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "outside source"));
    char *monitor[] = {"capture", "monitor"};
    command(p, cfg, 2, monitor);
    XMoveWindow(d, frame, 450, 350);
    XSync(d, false);
    /* Selected app pixels remain intact when the separate panel overlaps. */
    Window app = XCreateSimpleWindow(d, root, 430, 330, 180, 120, 0, 0, 0x778899);
    Atom wm_state = XInternAtom(d, "WM_STATE", false);
    unsigned long app_state[2] = {1, 0};
    XChangeProperty(d, app, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)app_state, 2);
    Atom active = XInternAtom(d, "_NET_ACTIVE_WINDOW", false);
    XChangeProperty(d, root, active, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&app, 1);
    XMapWindow(d, app);
    XSync(d, false);
    char *select[] = {"capture", "window", "active"};
    command(p, cfg, 3, select);
    XClearWindow(d, app);
    XRaiseWindow(d, frame);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    for (int y = 0; y < capture.height; y++) {
        for (int x = 0; x < capture.width; x++) {
            assert(pixel(&capture, x, y) == 0x778899);
        }
    }
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "XComposite"));
    XChangeProperty(d, root, active, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&panel, 1);
    XSync(d, false);
    assert(platform_command(p, cfg, 3, select, error, sizeof(error)) < 0);
    XChangeProperty(d, root, active, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&frame, 1);
    XSync(d, false);
    assert(platform_command(p, cfg, 3, select, error, sizeof(error)) < 0);
    command(p, cfg, 2, monitor);
    XDestroyWindow(d, app);
    /* Property identity changes revoke registration instead of claiming another window. */
    pid++;
    XChangeProperty(d, panel, pid_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    pump(d, p, cfg, comp, false);
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "no panel window registered"));
    pid--;
    XChangeProperty(d, panel, pid_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    XSync(d, false);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) == 0);
    XUnmapWindow(d, frame);
    pump(d, p, cfg, comp, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    for (int y = 0; y < capture.height; y++) {
        for (int x = 0; x < capture.width; x++) {
            assert(pixel(&capture, x, y) == 0);
        }
    }
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "hidden"));
    XMapWindow(d, frame);
    XSync(d, false);
    platform_panel_unregister(p);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(pixel(&capture, 470, 390) == 0xff3377);
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) == 0);
    XDestroyWindow(d, frame);
    pump(d, p, cfg, comp, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    platform_panel_status(p, status, sizeof(status));
    assert(strstr(status, "no panel window registered"));
    assert(platform_panel_register(p, panel, (int)pid, error, sizeof(error)) < 0);
    frame_free(&capture);
    frame_free(&virtual);
    frame_free(&recording);
}

static void countdown_tests(Display *d, Platform *p, Config *cfg, Compositor *comp)
{
    char error[CAST_ERR];
    Frame capture = {0};
    Cursor cursor;
    Window existing_preview = named_window(d, "cast output preview");
    assert(platform_countdown(p, 3000000000ULL, error, sizeof error) == 0);
    XSync(d, false);
    Window window = named_window(d, "cast output preview");
    XWindowAttributes attr;
    assert(window && XGetWindowAttributes(d, window, &attr) && attr.map_state == IsViewable);
    assert(!attr.override_redirect && attr.width == 640 && attr.height > 300);
    if (existing_preview) {
        assert(window == existing_preview); /* No second, retiring WM frame. */
    } else {
        assert(attr.x == (DisplayWidth(d, DefaultScreen(d)) - attr.width) / 2);
    }
    assert_utility(d, window);
    XClassHint hint;
    assert(XGetClassHint(d, window, &hint));
    assert(!strcmp(hint.res_class, "CastPreview"));
    XFree(hint.res_name);
    XFree(hint.res_class);
    XImage *image =
        XGetImage(d, window, 0, 0, (unsigned)attr.width, (unsigned)attr.height, AllPlanes, ZPixmap);
    assert(image);
    int radius = (attr.height - 64) / 3;
    assert(XGetPixel(image, attr.width / 2 - radius, attr.height / 2) == 0xbbc1ca);
    XDestroyImage(image);
    usleep(40000);
    assert(platform_countdown(p, 2000000000ULL, error, sizeof error) == 0);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    Atom active = XInternAtom(d, "_NET_ACTIVE_WINDOW", false);
    XChangeProperty(d, DefaultRootWindow(d), active, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&window, 1);
    XSync(d, false);
    char *select[] = {"capture", "window", "active"};
    assert(platform_command(p, cfg, 3, select, error, sizeof error) < 0);
    Window frame =
        XCreateSimpleWindow(d, DefaultRootWindow(d), 40, 60, 260, 275, 3, 0xaa2233, 0x332244);
    XReparentWindow(d, window, frame, 10, 25);
    XMapWindow(d, frame);
    XSync(d, false);
    assert(XGetWindowAttributes(d, frame, &attr));
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    XChangeProperty(d, DefaultRootWindow(d), active, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&frame, 1);
    XSync(d, false);
    assert(platform_command(p, cfg, 3, select, error, sizeof error) < 0);
    XEvent cancel = {0};
    cancel.xclient.type = ClientMessage;
    cancel.xclient.window = window;
    cancel.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", false);
    cancel.xclient.format = 32;
    cancel.xclient.data.l[0] = (long)XInternAtom(d, "WM_DELETE_WINDOW", false);
    XSendEvent(d, window, false, NoEventMask, &cancel);
    XSync(d, false);
    /* The final deadline poll sees close even without the regular event pump. */
    assert(platform_countdown(p, 1, error, sizeof error) == 1);
    assert(platform_countdown(p, 0, error, sizeof error) < 0);
    assert(strstr(error, "window manager"));
    XSync(d, false);
    /* The fake WM deliberately leaves decorations mapped after client teardown. */
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    assert_preview_mask(&capture, &attr, 0, 0, cfg->pause_color);
    XDestroyWindow(d, frame);
    XSync(d, false);
    assert(platform_countdown(p, 0, error, sizeof error) == 0);
    assert(!named_window(d, "cast output preview"));
    assert(platform_capture(p, &capture, &cursor, error, sizeof error) == 0);
    for (int y = 0; y < capture.height; y++) {
        for (int x = 0; x < capture.width; x++) {
            assert(pixel(&capture, x, y) == 0);
        }
    }
    assert(platform_countdown(p, 1000000000ULL, error, sizeof error) == 0);
    XSync(d, false);
    window = named_window(d, "cast output preview");
    memset(&cancel, 0, sizeof cancel);
    cancel.xkey.type = KeyPress;
    cancel.xkey.window = window;
    cancel.xkey.keycode = XKeysymToKeycode(d, XK_Escape);
    XSendEvent(d, window, false, KeyPressMask, &cancel);
    pump(d, p, cfg, comp, false);
    assert(platform_countdown(p, 1, error, sizeof error) == 1);
    assert(platform_countdown(p, 0, error, sizeof error) == 1);
    /* The visible Cancel control also wins at the final deadline poll. */
    assert(platform_countdown(p, 1000000000ULL, error, sizeof error) == 0);
    XSync(d, false);
    window = named_window(d, "cast output preview");
    assert(XGetWindowAttributes(d, window, &attr));
    memset(&cancel, 0, sizeof cancel);
    cancel.xbutton.type = ButtonPress;
    cancel.xbutton.window = window;
    cancel.xbutton.button = Button1;
    cancel.xbutton.x = attr.width - 40;
    cancel.xbutton.y = 16;
    assert(XSendEvent(d, window, False, ButtonPressMask, &cancel));
    XSync(d, false);
    assert(platform_countdown(p, 1, error, sizeof error) == 1);
    assert(platform_countdown(p, 0, error, sizeof error) == 1);
    memset(&cancel, 0, sizeof cancel);
    cancel.xkey.type = KeyPress;
    cancel.xkey.keycode = XKeysymToKeycode(d, XK_Escape);
    /* A panel attachment hides directly: queued cancellation must still win. */
    assert(platform_countdown(p, 1000000000ULL, error, sizeof error) == 0);
    XSync(d, false);
    window = named_window(d, "cast output preview");
    cancel.xkey.window = window;
    XSendEvent(d, window, false, KeyPressMask, &cancel);
    XSync(d, false);
    assert(platform_countdown(p, 0, error, sizeof error) == 1);
    assert(platform_countdown(p, 0, error, sizeof error) == 0);
    frame_free(&capture);
}

static void keyboard_tests(Display *d, Platform *p, Config *cfg, Compositor *c)
{
    cfg->keys = true;
    pump(d, p, cfg, c, false); /* Discard initial source-generation observation batch. */
    press(d, XK_Control_L, true);
    press(d, XK_c, true);
    press(d, XK_c, false);
    press(d, XK_Control_L, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "Ctrl+C"));

    compositor_clear(c);
    press(d, XK_a, true);
    press(d, XK_a, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL)); /* Shortcuts mode excludes printable typing. */
    strcpy(cfg->keys_mode, "all");
    press(d, XK_Shift_L, true);
    press(d, XK_a, true);
    press(d, XK_a, false);
    press(d, XK_Shift_L, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "Shift+A"));

    compositor_clear(c);
    strcpy(cfg->keys_filter, "Ctrl+C");
    press(d, XK_Control_L, true);
    press(d, XK_c, true);
    press(d, XK_c, false);
    press(d, XK_Control_L, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    cfg->keys_filter[0] = 0;

    compositor_clear(c);
    strcpy(cfg->keys_filter, "Super+Shift+Space");
    press(d, XK_Super_L, true);
    press(d, XK_Shift_L, true);
    press(d, XK_space, true);
    press(d, XK_space, false);
    press(d, XK_Shift_L, false);
    press(d, XK_Super_L, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    cfg->keys_filter[0] = 0;

    /* Paused input, including pending events on the resume boundary, never reappears. */
    pump(d, p, cfg, c, true);
    press(d, XK_a, true);
    press(d, XK_a, false);
    pump(d, p, cfg, c, true);
    press(d, XK_b, true);
    press(d, XK_b, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    press(d, XK_Left, true);
    press(d, XK_Left, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "Left"));

    /* Rapid separate presses must all count, even inside the old 100ms guard. */
    compositor_clear(c);
    press(d, XK_j, true);
    press(d, XK_j, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "j"));
    press(d, XK_j, true);
    press(d, XK_j, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "jx2"));
    press(d, XK_j, true);
    press(d, XK_j, false);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "jx3"));

    /* Without another observed raw press, a held key still expires normally.
     * XKB software repeat does not necessarily emit XI_RawKeyPress events. */
    compositor_clear(c);
    cfg->keys_timeout_ms = 120;
    press(d, XK_a, true);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "a"));
    usleep(200000);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    press(d, XK_a, false);
    pump(d, p, cfg, c, false);
    cfg->keys_timeout_ms = 1000;

    /* Remap a key on this disposable server, and verify XKB map-change translation. */
    compositor_clear(c);
    KeyCode code = XKeysymToKeycode(d, XK_a);
    KeySym mapping[2] = {XK_z, XK_Z};
    XChangeKeyboardMapping(d, code, 2, mapping, 1);
    pump(d, p, cfg, c, false);
    assert(XTestFakeKeyEvent(d, code, true, CurrentTime));
    assert(XTestFakeKeyEvent(d, code, false, CurrentTime));
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "z"));
    cfg->keys = false;
    compositor_clear(c);
}

static void preview_benchmark(Display *, Platform *, Config *);

static void exercise(Display *d, Platform *p, Config *cfg)
{
    Window root = DefaultRootWindow(d);
    Window window = XCreateSimpleWindow(d, root, 80, 60, 200, 100, 0, 0, 0x44aa66);
    XStoreName(d, window, "cast isolated capture acceptance");
    Atom wm_state = XInternAtom(d, "WM_STATE", false);
    unsigned long state[2] = {1, 0};
    XChangeProperty(d, window, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)state, 2);
    Atom active = XInternAtom(d, "_NET_ACTIVE_WINDOW", false);
    XChangeProperty(d, root, active, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&window, 1);
    XMapWindow(d, window);
    XSync(d, false);
    Compositor *c = compositor_create();
    Frame capture = {0};
    Cursor cursor;
    char error[CAST_ERR];
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(pixel(&capture, 90, 70) == 0x44aa66);
    char *select[] = {"capture", "window", "active"};
    command(p, cfg, 3, select);
    /* XComposite redirection exposes the redirected drawable; the application repaints it. */
    XClearWindow(d, window);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(capture.width == 200 && capture.height == 100 && pixel(&capture, 10, 10) == 0x44aa66);
    XMoveResizeWindow(d, window, 120, 100, 240, 120);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(capture.width == 240 && capture.height == 120);
    XUnmapWindow(d, window);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) < 0);
    assert(strstr(error, "minimized"));
    XMapWindow(d, window);
    XSetWindowBackground(d, window, 0xaa4466);
    XClearWindow(d, window);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(pixel(&capture, 10, 10) == 0xaa4466); /* Newly mapped backing pixmap. */
    char *monitor[] = {"capture", "monitor"};
    command(p, cfg, 2, monitor);
    char selected_monitor[sizeof cfg->monitor];
    char *next[] = {"screen", "next"}, *previous[] = {"screen", "prev"};
    command(p, cfg, 2, next);
    snprintf(selected_monitor, sizeof selected_monitor, "%s", cfg->monitor);
    command(p, cfg, 2, next);
    command(p, cfg, 2, previous);
    assert(!strcmp(cfg->monitor, selected_monitor));
    command(p, cfg, 2, previous);
    command(p, cfg, 2, next);
    assert(!strcmp(cfg->monitor, selected_monitor));
    char *region[] = {"capture", "region", "80", "60", "200", "100"};
    command(p, cfg, 6, region);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(capture.width == 200 && capture.height == 100);
    uint64_t before = platform_source_generation(p);
    char *invalid[] = {"capture", "region", "-1", "0", "200", "100"};
    assert(platform_command(p, cfg, 6, invalid, error, sizeof(error)) < 0);
    assert(platform_source_generation(p) == before && cfg->region_x == 80);

    char *interactive[] = {"capture", "region", "select"};
    command(p, cfg, 3, interactive);
    press(d, XK_Escape, true);
    press(d, XK_Escape, false);
    pump(d, p, cfg, c, false);
    assert(platform_source_generation(p) == before && cfg->region_x == 80);
    command(p, cfg, 3, interactive);
    XTestFakeMotionEvent(d, DefaultScreen(d), 10, 20, CurrentTime);
    XTestFakeButtonEvent(d, 1, true, CurrentTime);
    XTestFakeMotionEvent(d, DefaultScreen(d), 110, 100, CurrentTime);
    XTestFakeButtonEvent(d, 1, false, CurrentTime);
    pump(d, p, cfg, c, false);
    assert(cfg->region_x == 10 && cfg->region_y == 20 && cfg->region_w == 100 &&
           cfg->region_h == 80);
    assert(platform_source_generation(p) > before);

    /* Selection cancellation also retains an existing window, including zoom. */
    command(p, cfg, 3, select);
    cfg->zoom_factor = 2;
    before = platform_source_generation(p);
    char *window_interactive[] = {"capture", "window", "select"};
    command(p, cfg, 3, window_interactive);
    press(d, XK_Escape, true);
    press(d, XK_Escape, false);
    pump(d, p, cfg, c, false);
    assert(platform_source_generation(p) == before);
    assert(!strcmp(cfg->capture_kind, "window") && cfg->zoom_factor == 2);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(capture.width == 240 && capture.height == 120 && pixel(&capture, 10, 10) == 0xaa4466);

    /* Passive click observation ignores desktop coordinates outside the source. */
    cfg->clicks = true;
    cfg->click_radius = 12;
    cfg->click_duration_ms = 500;
    cfg->click_left_color = 0xff0000;
    cfg->zoom_factor = 1;
    compositor_clear(c);
    XTestFakeMotionEvent(d, DefaultScreen(d), 10, 10, CurrentTime);
    XTestFakeButtonEvent(d, 1, true, CurrentTime);
    XTestFakeButtonEvent(d, 1, false, CurrentTime);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    XTestFakeMotionEvent(d, DefaultScreen(d), 150, 130, CurrentTime);
    XTestFakeButtonEvent(d, 1, true, CurrentTime);
    XTestFakeButtonEvent(d, 1, false, CurrentTime);
    pump(d, p, cfg, c, false);
    assert(!has_label(c, cfg, NULL));
    pump(d, p, cfg, c, true);
    assert(has_label(c, cfg, NULL));
    XTestFakeButtonEvent(d, 1, true, CurrentTime);
    XTestFakeButtonEvent(d, 1, false, CurrentTime);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, NULL));
    cfg->clicks = false;

    command(p, cfg, 2, monitor);
    keyboard_tests(d, p, cfg, c);
    command(p, cfg, 3, select);
    XDestroyWindow(d, window);
    XSync(d, false);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) < 0);
    assert(strstr(error, "destroyed"));
    command(p, cfg, 2, monitor);
    preview_tests(d, p, cfg, c);
    panel_tests(d, p, cfg, c);
    countdown_tests(d, p, cfg, c);
    preview_benchmark(d, p, cfg);
    frame_free(&capture);
    compositor_destroy(c);
    puts("Xvfb: root/window pixels, resize, remap/minimize/destroy, atomic regions, selection "
         "cancellation, XI2/XKB mapping/modifiers/repeat, input privacy, stable floating preview "
         "mapping/drag, authenticated panel/frame geometry and output exclusion "
         "passed");
}

static void capture_benchmark(Platform *p)
{
    Frame f = {0};
    Cursor cursor;
    char error[CAST_ERR];
    assert(platform_capture(p, &f, &cursor, error, sizeof(error)) == 0);
    const int frames = 40;
    uint64_t start = cast_now_ns();
    for (int i = 0; i < frames; i++) {
        assert(platform_capture(p, &f, &cursor, error, sizeof(error)) == 0);
    }
    double seconds = (double)(cast_now_ns() - start) / 1e9;
    printf("Xorg read-only benchmark: %dx%d, %d captures in %.3fs, %.3fms/capture, %.2ffps; "
           "no pixels saved\n",
           f.width, f.height, frames, seconds, seconds * 1000 / frames, frames / seconds);
    frame_free(&f);
}

static void preview_benchmark(Display *d, Platform *p, Config *cfg)
{
    Frame frame = {0};
    assert(frame_alloc(&frame, 1920, 1080) == 0);
    memset(frame.data, 0, (size_t)frame.stride * frame.height);
    cfg->preview = true;
    State state = {0};
    Compositor *comp = compositor_create();
    Window window = wait_preview(d, p, cfg, comp, &frame, &state);
    XResizeWindow(d, window, 640, 392);
    pump(d, p, cfg, comp, false);
    XWindowAttributes attr;
    assert(XGetWindowAttributes(d, window, &attr));
    uint64_t start = cast_now_ns(), calls = 0;
    int updates = 0, last = -1, samples = 0, total_lag = 0;
    char error[CAST_ERR];
    for (int seq = 1; seq <= 90; seq++) {
        for (int y = 0; y < frame.height; y++) {
            uint8_t *row = frame.data + (size_t)y * frame.stride;
            for (int x = 0; x < frame.width; x++) {
                row[4 * x] = 32;
                row[4 * x + 1] = (uint8_t)seq;
                row[4 * x + 2] = 64;
                row[4 * x + 3] = 255;
            }
        }
        frame.ts_ns = cast_now_ns();
        uint64_t before = cast_now_ns();
        assert(platform_preview(p, &frame, &state, cfg, error, sizeof error) == 0);
        calls += cast_now_ns() - before;
        XImage *image =
            XGetImage(d, window, attr.width / 2, (attr.height - 64) / 2, 1, 1, AllPlanes, ZPixmap);
        assert(image);
        unsigned long value = XGetPixel(image, 0, 0);
        XDestroyImage(image);
        int shown = (int)((value >> 8) & 255);
        if (shown > 0 && shown <= seq) {
            if (shown != last) {
                updates++;
            }
            total_lag += seq - shown;
            samples++;
            last = shown;
        }
        uint64_t deadline = start + (uint64_t)seq * 1000000000 / 60;
        struct timespec next = {(time_t)(deadline / 1000000000), (long)(deadline % 1000000000)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    printf("Preview 1080p->%dx%d: %d updates/90 at 60Hz, %.2fms mean age, %.3fms capture-thread "
           "call\n",
           attr.width, attr.height - 64, updates, samples ? total_lag * 1000. / 60 / samples : 0,
           calls / 90. / 1e6);
    assert(updates >= 40); /* Reject the former ~15fps gate, allow loaded CI runners. */
    assert(samples && total_lag < samples * 8); /* Less than eight producer ticks behind. */
    cfg->preview = false;
    assert(platform_preview(p, &frame, &state, cfg, error, sizeof error) == 0);
    frame_free(&frame);
    compositor_destroy(comp);
}

int main(int argc, char **argv)
{
    Config cfg = configuration();
    char error[CAST_ERR];
    Platform *p = platform_open(&cfg, error, sizeof(error));
    if (!p) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    Capabilities cap = platform_capabilities(p);
    assert(cap.capture && cap.cursor_metadata && cap.input);
    if (argc == 2 && !strcmp(argv[1], "--exercise")) {
        Display *d = XOpenDisplay(NULL);
        assert(d);
        exercise(d, p, &cfg);
        XCloseDisplay(d);
    } else if (argc == 2 && !strcmp(argv[1], "--preview-benchmark")) {
        Display *d = XOpenDisplay(NULL);
        assert(d);
        preview_benchmark(d, p, &cfg);
        XCloseDisplay(d);
    } else if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        capture_benchmark(p);
    } else {
        Frame f = {0};
        Cursor cursor;
        assert(platform_capture(p, &f, &cursor, error, sizeof(error)) == 0);
        Compositor *c = compositor_create();
        platform_events(p, c, &cfg, true);
        printf("Xorg read-only prototype: %dx%d RGBA stride %d; MIT-SHM/XRandR and passive XI2/XKB "
               "available; no pixels saved\n",
               f.width, f.height, f.stride);
        frame_free(&f);
        compositor_destroy(c);
    }
    platform_close(p);
    return 0;
}
