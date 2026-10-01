#include "platform_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Backend {
    SYNTHETIC,
    XORG,
    WAYLAND
};
struct Platform {
    enum Backend backend;
    Platform *impl;
    int width, height;
};
static int error(char *e, size_t n, const char *s)
{
    snprintf(e, n, "%s", s);
    return -1;
}
Platform *platform_open(const Config *cfg, char *e, size_t n)
{
    Platform *p = calloc(1, sizeof(*p));
    if (!p) {
        error(e, n, "out of memory opening platform");
        return NULL;
    }
    p->width = cfg->width;
    p->height = cfg->height;
    const char *backend = cfg->backend;
    if (!strcmp(backend, "auto")) {
        backend = getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY") ? "wayland" : "x11";
    }
    if (!strcmp(backend, "synthetic")) {
        p->backend = SYNTHETIC;
        return p;
    }
    if (!strcmp(backend, "x11") || !strcmp(backend, "xorg")) {
#ifdef WITH_X11
        p->backend = XORG;
        p->impl = x11_open(cfg, e, n);
#else
        error(e, n, "Xorg backend was not built; rebuild with X11=1 or choose --backend wayland");
#endif
    } else if (!strcmp(backend, "wayland")) {
#ifdef WITH_WAYLAND
        p->backend = WAYLAND;
        p->impl = wayland_open(cfg, e, n);
#else
        error(e, n, "Wayland backend was not built; rebuild with WAYLAND=1");
#endif
    } else {
        error(e, n, "unknown capture backend; choose auto, x11, wayland or synthetic (tests)");
    }
    if (!p->impl) {
        free(p);
        return NULL;
    }
    return p;
}
void platform_close(Platform *p)
{
    if (!p) {
        return;
    }
#ifdef WITH_X11
    if (p->backend == XORG) {
        x11_close(p->impl);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        wayland_close(p->impl);
    }
#endif
    free(p);
}
Capabilities platform_capabilities(Platform *p)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_capabilities(p->impl);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        return wayland_capabilities(p->impl);
    }
#endif
    Capabilities cap = {.capture = true, .cursor_metadata = true};
    snprintf(cap.description, sizeof(cap.description),
             "synthetic test frames; no desktop, global input or presentation UI");
    return cap;
}
uint64_t platform_source_generation(Platform *p)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_source_generation(p->impl);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        return wayland_source_generation(p->impl);
    }
#endif
    return 1;
}
int platform_capture(Platform *p, Frame *f, Cursor *c, char *e, size_t n)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_capture(p->impl, f, c, e, n);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        return wayland_capture(p->impl, f, c, e, n);
    }
#endif
    if (frame_alloc(f, p->width, p->height) < 0) {
        return error(e, n, "cannot allocate synthetic test frame");
    }
    uint64_t now = cast_now_ns();
    unsigned phase = (unsigned)(now / 100000000);
    for (int y = 0; y < f->height; y++) {
        for (int x = 0; x < f->width; x++) {
            uint8_t *q = f->data + (size_t)y * f->stride + x * 4;
            q[0] = (uint8_t)(x * 255 / f->width);
            q[1] = (uint8_t)(y * 255 / f->height);
            q[2] = (uint8_t)((x / 32 + y / 32 + phase) % 2 ? 100 : 30);
            q[3] = 255;
        }
    }
    f->ts_ns = now;
    *c = (Cursor){f->width / 2, f->height / 2, true, now};
    return 0;
}
int platform_command(Platform *p, Config *cfg, int argc, char **argv, char *e, size_t n)
{
    if (argc == 3 && !strcmp(argv[0], "capture") && !strcmp(argv[1], "fit")) {
        if (strcmp(argv[2], "contain") && strcmp(argv[2], "cover")) {
            return error(e, n, "usage: cast capture fit contain|cover");
        }
        snprintf(cfg->fit, sizeof(cfg->fit), "%s", argv[2]);
        snprintf(e, n, "capture fit: %s", cfg->fit);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[0], "preview")) {
        if (argc == 3 && !strcmp(argv[1], "target")) {
            if (strcmp(argv[2], "live") && strcmp(argv[2], "record")) {
                return error(e, n, "usage: cast preview target live|record");
            }
            snprintf(cfg->preview_target, sizeof(cfg->preview_target), "%s", argv[2]);
            snprintf(e, n, "preview target: %s", cfg->preview_target);
            return 0;
        }
        if (argc != 2 ||
            (strcmp(argv[1], "on") && strcmp(argv[1], "off") && strcmp(argv[1], "toggle"))) {
            return error(e, n,
                         "usage: cast preview on|off|toggle; cast preview target live|record");
        }
        bool on = !strcmp(argv[1], "toggle") ? !cfg->preview : !strcmp(argv[1], "on");
        if (on && !platform_capabilities(p).preview) {
            return error(e, n,
                         "preview is unsupported by this backend; view the virtual camera with an "
                         "external application");
        }
        cfg->preview = on;
        snprintf(e, n, "preview: %s", on ? "on" : "off");
        return 0;
    }
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_command(p->impl, cfg, argc, argv, e, n);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        return wayland_command(p->impl, cfg, argc, argv, e, n);
    }
#endif
    if (argc == 2 && !strcmp(argv[0], "screen") && !strcmp(argv[1], "list")) {
        snprintf(e, n, "synthetic: %dx%d (test source)", p->width, p->height);
        return 0;
    }
    return error(e, n,
                 "synthetic test backend does not support monitor/window selection or preview");
}
int platform_reconfigure(Platform *p, const Config *cfg, char *e, size_t n)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_reconfigure(p->impl, cfg, e, n);
    }
#endif
#ifdef WITH_WAYLAND
    if (p->backend == WAYLAND) {
        return wayland_reconfigure(p->impl, cfg, e, n);
    }
#endif
    p->width = cfg->width;
    p->height = cfg->height;
    return 0;
}
void platform_events(Platform *p, Compositor *comp, const Config *cfg, bool privacy)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        x11_events(p->impl, comp, cfg, privacy);
    }
#else
    (void)p;
    (void)comp;
    (void)cfg;
    (void)privacy;
#endif
}
int platform_preview(Platform *p, const Frame *f, const State *s, const Config *cfg, char *e,
                     size_t n)
{
#ifdef WITH_X11
    if (p->backend == XORG) {
        return x11_preview(p->impl, f, s, cfg, e, n);
    }
#else
    (void)f;
    (void)s;
#endif
    if (!cfg->preview) {
        return 0;
    }
    return error(e, n,
                 "preview is unavailable for this backend; use preview off or an external "
                 "virtual-camera viewer");
}
void platform_doctor(const Config *cfg, char *e, size_t n)
{
    const char *backend = cfg->backend;
    if (!strcmp(backend, "auto")) {
        backend = getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY") ? "wayland" : "x11";
    }
#ifdef WITH_X11
    if (!strcmp(backend, "x11") || !strcmp(backend, "xorg")) {
        x11_doctor(cfg, e, n);
        return;
    }
#endif
#ifdef WITH_WAYLAND
    if (!strcmp(backend, "wayland")) {
        wayland_doctor(cfg, e, n);
        return;
    }
#endif
    snprintf(e, n,
             !strcmp(backend, "synthetic") ? "capture: synthetic test source (hardware not checked)"
                                           : "capture: requested backend unavailable in this "
                                             "build; rebuild with X11=1 and/or WAYLAND=1");
}
