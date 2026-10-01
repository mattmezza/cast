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
    strcpy(c.preview_target, "live");
    c.width = 256;
    c.height = 128;
    c.fps = 30;
    c.zoom_factor = 1;
    c.zoom_max = 8;
    c.zoom_smoothing = 1;
    c.keys_timeout_ms = 1000;
    c.keys_font_size = 14;
    c.keys_color = 0xffffff;
    c.annotations_live_keys = c.annotations_record_keys = true;
    c.annotations_live_clicks = c.annotations_record_clicks = true;
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
        compositor_key(reference, label, source.ts_ns);
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

static Window preview_window(Display *d)
{
    Window root, parent, *children = NULL, result = None;
    unsigned count = 0;
    assert(XQueryTree(d, DefaultRootWindow(d), &root, &parent, &children, &count));
    for (unsigned i = 0; i < count; i++) {
        char *name = NULL;
        if (XFetchName(d, children[i], &name) && name && !strcmp(name, "cast output preview")) {
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
        Window win = preview_window(d);
        XWindowAttributes attr;
        if (win && XGetWindowAttributes(d, win, &attr) && attr.map_state == IsViewable) {
            return win;
        }
        usleep(10000);
    }
    assert(!"preview worker did not paint/map within timeout");
    return None;
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

    /* Repeated presses while held must not extend the annotation timeout. */
    compositor_clear(c);
    cfg->keys_timeout_ms = 120;
    assert(XkbSetAutoRepeatRate(d, XkbUseCoreKbd, 40, 20));
    press(d, XK_a, true);
    pump(d, p, cfg, c, false);
    assert(has_label(c, cfg, "A"));
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
    assert(has_label(c, cfg, "Z"));
    cfg->keys = false;
    compositor_clear(c);
}

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
    State s = {.live_paused = true};
    cfg->preview = true;
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    Window preview = wait_preview(d, p, cfg, c, &capture, &s);
    XImage *header = XGetImage(d, preview, 0, 0, 32, 32, AllPlanes, ZPixmap);
    assert(header && XGetPixel(header, 10, 10) == 0x111111);
    XDestroyImage(header); /* Local header was really painted before exclusion check. */
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(pixel(&capture, 10, 10) == 0); /* Preview unmapped from root readback. */
    cfg->preview = false;
    assert(platform_preview(p, &capture, &s, cfg, error, sizeof(error)) == 0);
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    XWindowAttributes attr;
    assert(XGetWindowAttributes(d, preview, &attr) && attr.map_state == IsUnmapped);
    cfg->preview = true;
    preview = wait_preview(d, p, cfg, c, &capture, &s);
    s.live_paused = false;
    assert(platform_preview(p, &capture, &s, cfg, error, sizeof(error)) == 0);
    /* A state boundary hides already painted content until a fresh epoch is ready. */
    assert(platform_capture(p, &capture, &cursor, error, sizeof(error)) == 0);
    assert(XGetWindowAttributes(d, preview, &attr) && attr.map_state == IsUnmapped);
    cfg->preview = false;
    assert(platform_preview(p, &capture, &s, cfg, error, sizeof(error)) == 0);
    frame_free(&capture);
    compositor_destroy(c);
    puts("Xvfb: root/window pixels, resize, remap/minimize/destroy, atomic regions, selection "
         "cancellation, XI2/XKB mapping/modifiers/repeat, input privacy and preview exclusion "
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
