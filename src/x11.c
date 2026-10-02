/* Xlib-only platform implementation. All desktop coordinates stop at this boundary. */
#include "platform_backend.h"
#define Cursor X11Cursor
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#undef Cursor
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define MONITOR_MAX 64
typedef struct {
    char name[128];
    int x, y, w, h;
    bool primary;
} Monitor;
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stop, pending;
    Frame frame;
    Window window;
    int width, height;
    char label[256], error[CAST_ERR];
    unsigned long color;
    uint64_t epoch, ready_epoch;
    Pixmap ready_pixmap;
} Preview;
typedef struct {
    Display *d;
    Window root, selected, preview, selection, panel;
    int panel_pid;
    Atom wm_delete, wm_state, active_window;
    int screen, xi_opcode, randr_base, xkb_base;
    bool xi, shm, composite, xkb, redirected, shm_attached, privacy, preview_disabled;
    bool window_unmapped;
    XImage *image;
    XShmSegmentInfo segment;
    int image_depth;
    Visual *image_visual;
    Pixmap window_pixmap;
    int pixmap_w, pixmap_h;
    uint64_t generation, seen_generation;
    char monitor[128], kind[16], source_error[CAST_ERR];
    int rx, ry, rw, rh, sx, sy, sw, sh;
    unsigned mods, group, alt_mask, super_mask;
    int last_keycode;
    Time last_key_time;
    int selecting, drag_x, drag_y, last_x, last_y;
    bool dragging;
    Config *owner; /* Runtime config object provided by command; daemon owns its lifetime. */
    X11Cursor crosshair;
    int preview_w, preview_h, preview_border;
    uint64_t last_preview;
    Preview *preview_worker;
    uint64_t preview_epoch, preview_source;
    unsigned preview_state;
    bool preview_enabled;
    uint32_t preview_mask_color;
    bool preview_dragging;
    int preview_drag_x, preview_drag_y;
} Xorg;
static _Thread_local int xerror;
static pthread_once_t xlib_once = PTHREAD_ONCE_INIT;
static void initialize_xlib(void)
{
    XInitThreads();
}
static void preview_stop(Xorg *p);
static void preview_neutral(Xorg *p);
static int on_error(Display *d, XErrorEvent *ev)
{
    (void)d;
    xerror = ev->error_code;
    return 0;
}
static void begin(Xorg *p)
{
    XSync(p->d, False);
    xerror = 0;
}
static bool end(Xorg *p)
{
    XSync(p->d, False);
    return xerror == 0;
}
static int fail(char *e, size_t n, const char *s)
{
    snprintf(e, n, "%s", s);
    return -1;
}
static void append(char *e, size_t n, const char *s)
{
    size_t used = strlen(e);
    if (used < n) {
        snprintf(e + used, n - used, "%s", s);
    }
}
static bool integer(const char *s, int *out)
{
    char *endp;
    errno = 0;
    long v = strtol(s, &endp, 10);
    if (errno || !s[0] || *endp || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}
static int monitors(Xorg *p, Monitor *list, int capacity)
{
    int count = 0, n = 0;
    int major = 1, minor = 5;
    if (XRRQueryVersion(p->d, &major, &minor) && ((major > 1) || (major == 1 && minor >= 5))) {
        XRRMonitorInfo *info = XRRGetMonitors(p->d, p->root, True, &n);
        for (int i = 0; info && i < n && count < capacity; i++) {
            if (info[i].width > 0 && info[i].height > 0) {
                char *name = XGetAtomName(p->d, info[i].name);
                Monitor *m = &list[count++];
                memset(m, 0, sizeof(*m));
                snprintf(m->name, sizeof(m->name), "%s", name ? name : "unnamed");
                if (name) {
                    XFree(name);
                }
                m->x = info[i].x;
                m->y = info[i].y;
                m->w = info[i].width;
                m->h = info[i].height;
                m->primary = info[i].primary;
            }
        }
        if (info) {
            XRRFreeMonitors(info);
        }
    } else {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(p->d, p->root);
        RROutput primary = XRRGetOutputPrimary(p->d, p->root);
        for (int i = 0; res && i < res->noutput && count < capacity; i++) {
            XRROutputInfo *o = XRRGetOutputInfo(p->d, res, res->outputs[i]);
            if (o && o->connection == RR_Connected && o->crtc) {
                XRRCrtcInfo *c = XRRGetCrtcInfo(p->d, res, o->crtc);
                if (c && c->width && c->height) {
                    Monitor *m = &list[count++];
                    memset(m, 0, sizeof(*m));
                    snprintf(m->name, sizeof(m->name), "%.*s", o->nameLen, o->name);
                    m->x = c->x;
                    m->y = c->y;
                    m->w = (int)c->width;
                    m->h = (int)c->height;
                    m->primary = res->outputs[i] == primary;
                }
                if (c) {
                    XRRFreeCrtcInfo(c);
                }
            }
            if (o) {
                XRRFreeOutputInfo(o);
            }
        }
        if (res) {
            XRRFreeScreenResources(res);
        }
    }
    return count;
}
static int find_monitor(Xorg *p, const char *name, Monitor *out)
{
    Monitor list[MONITOR_MAX];
    int count = monitors(p, list, MONITOR_MAX);
    if (!count) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if ((name[0] && !strcmp(name, list[i].name)) || (!name[0] && list[i].primary)) {
            *out = list[i];
            return 0;
        }
    }
    if (!name[0]) {
        *out = list[0];
        return 0;
    }
    return -1;
}
static void image_free(Xorg *p)
{
    if (p->shm_attached) {
        begin(p);
        XShmDetach(p->d, &p->segment);
        end(p);
        p->shm_attached = false;
    }
    if (p->image) {
        if (p->segment.shmaddr && p->segment.shmaddr != (char *)-1) {
            p->image->data = NULL;
        }
        XDestroyImage(p->image);
        p->image = NULL;
    }
    if (p->segment.shmaddr && p->segment.shmaddr != (char *)-1) {
        shmdt(p->segment.shmaddr);
    }
    if (p->segment.shmid >= 0) {
        shmctl(p->segment.shmid, IPC_RMID, NULL);
    }
    memset(&p->segment, 0, sizeof(p->segment));
    p->segment.shmid = -1;
}
static int image_prepare(Xorg *p, Visual *visual, int depth, int w, int h)
{
    if (p->image && p->image->width == w && p->image->height == h && p->image_depth == depth &&
        p->image_visual == visual) {
        return 0;
    }
    image_free(p);
    if (!p->shm) {
        return 0;
    }
    p->image = XShmCreateImage(p->d, visual, (unsigned)depth, ZPixmap, NULL, &p->segment,
                               (unsigned)w, (unsigned)h);
    if (!p->image) {
        p->shm = false;
        return 0;
    }
    p->segment.shmid = shmget(IPC_PRIVATE, (size_t)p->image->bytes_per_line * h, IPC_CREAT | 0600);
    if (p->segment.shmid < 0) {
        image_free(p);
        p->shm = false;
        return 0;
    }
    p->segment.shmaddr = shmat(p->segment.shmid, NULL, 0);
    if (p->segment.shmaddr == (char *)-1) {
        image_free(p);
        p->shm = false;
        return 0;
    }
    p->image->data = p->segment.shmaddr;
    p->segment.readOnly = False;
    begin(p);
    Status attached = XShmAttach(p->d, &p->segment);
    bool ok = end(p);
    if (!attached || !ok) {
        image_free(p);
        p->shm = false;
        return 0;
    }
    p->shm_attached = true;
    shmctl(p->segment.shmid, IPC_RMID, NULL);
    p->segment.shmid = -1;
    p->image_depth = depth;
    p->image_visual = visual;
    return 0;
}
static void window_release(Xorg *p)
{
    if (p->window_pixmap) {
        begin(p);
        XFreePixmap(p->d, p->window_pixmap);
        end(p);
        p->window_pixmap = 0;
    }
    if (p->redirected && p->selected) {
        begin(p);
        XCompositeUnredirectWindow(p->d, p->selected, CompositeRedirectAutomatic);
        end(p);
    }
    p->redirected = false;
    p->window_unmapped = false;
    p->selected = 0;
    p->pixmap_w = p->pixmap_h = 0;
}
static bool client_window(Xorg *p, Window win)
{
    Atom type;
    int format;
    unsigned long n, remaining;
    unsigned char *value = NULL;
    int rc = XGetWindowProperty(p->d, win, p->wm_state, 0, 1, False, AnyPropertyType, &type,
                                &format, &n, &remaining, &value);
    if (value) {
        XFree(value);
    }
    return rc == Success && type != None;
}
static bool panel_window(Xorg *p, Window candidate)
{
    if (!p->panel || candidate == p->root) {
        return false;
    }
    bool found = false;
    begin(p);
    Window window = p->panel;
    for (int level = 0; window && window != p->root && level < 32; level++) {
        if (candidate == window) {
            found = true;
            break;
        }
        Window root, parent, *children = NULL;
        unsigned count;
        Status queried = XQueryTree(p->d, window, &root, &parent, &children, &count);
        if (children) {
            XFree(children);
        }
        if (!queried || root != p->root) {
            break;
        }
        window = parent;
    }
    return end(p) && found;
}

static Window find_client(Xorg *p, Window win, int depth, int *remaining)
{
    if (!win || win == p->preview || win == p->selection || panel_window(p, win) || depth > 12 ||
        --*remaining < 0) {
        return 0;
    }
    if (client_window(p, win)) {
        return win;
    }
    Window root, parent, *children = NULL;
    unsigned count = 0;
    if (!XQueryTree(p->d, win, &root, &parent, &children, &count)) {
        return 0;
    }
    Window result = 0;
    for (unsigned i = 0; i < count && !result; i++) {
        result = find_client(p, children[i], depth + 1, remaining);
    }
    if (children) {
        XFree(children);
    }
    return result;
}
static int window_select(Xorg *p, Window win, char *e, size_t n)
{
    if (!win || win == p->root || win == p->preview || win == p->selection ||
        panel_window(p, win)) {
        return fail(e, n,
                    "select a normal application window, not the desktop or cast preview/panel");
    }
    XWindowAttributes a;
    begin(p);
    Status ok = XGetWindowAttributes(p->d, win, &a);
    bool checked = end(p);
    if (!ok || !checked || a.map_state != IsViewable) {
        return fail(
            e, n,
            "selected window is unavailable or minimized; select a visible application window");
    }
    if (win == p->selected && p->window_pixmap) {
        snprintf(p->kind, sizeof(p->kind), "window");
        p->generation++;
        p->source_error[0] = 0;
        return 0;
    }
    if (!p->composite) {
        return fail(e, n,
                    "window capture requires the XComposite extension; use monitor/region capture "
                    "on this X server");
    }
    begin(p);
    XCompositeRedirectWindow(p->d, win, CompositeRedirectAutomatic);
    checked = end(p);
    if (!checked) {
        return fail(
            e, n,
            "XComposite cannot redirect this window; choose another window or monitor capture");
    }
    begin(p);
    Pixmap pix = XCompositeNameWindowPixmap(p->d, win);
    checked = end(p);
    if (!checked || !pix) {
        begin(p);
        XCompositeUnredirectWindow(p->d, win, CompositeRedirectAutomatic);
        end(p);
        return fail(e, n, "window pixmap is unavailable; choose another visible window");
    }
    window_release(p);
    p->selected = win;
    p->window_pixmap = pix;
    p->redirected = true;
    p->pixmap_w = a.width;
    p->pixmap_h = a.height;
    snprintf(p->kind, sizeof(p->kind), "window");
    p->generation++;
    p->source_error[0] = 0;
    image_free(p);
    return 0;
}
static Window active_once(Xorg *p)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *value = NULL;
    Window win = 0;
    if (XGetWindowProperty(p->d, p->root, p->active_window, 0, 1, False, XA_WINDOW, &type, &format,
                           &count, &after, &value) == Success &&
        type == XA_WINDOW && format == 32 && count) {
        win = *(Window *)value;
    }
    if (value) {
        XFree(value);
    }
    if (!win) {
        int revert;
        XGetInputFocus(p->d, &win, &revert);
        if (win == PointerRoot) {
            win = 0;
        }
    }
    return win;
}
static void modifier_masks(Xorg *p)
{
    p->alt_mask = Mod1Mask;
    p->super_mask = Mod4Mask;
    XModifierKeymap *map = XGetModifierMapping(p->d);
    if (!map) {
        return;
    }
    unsigned alt = 0, super = 0;
    for (int mod = 0; mod < 8; mod++) {
        for (int i = 0; i < map->max_keypermod; i++) {
            KeyCode key = map->modifiermap[mod * map->max_keypermod + i];
            if (!key) {
                continue;
            }
            KeySym sym = XkbKeycodeToKeysym(p->d, key, 0, 0);
            if (sym == XK_Alt_L || sym == XK_Alt_R) {
                alt |= 1u << mod;
            }
            if (sym == XK_Super_L || sym == XK_Super_R || sym == XK_Meta_L || sym == XK_Meta_R) {
                super |= 1u << mod;
            }
        }
    }
    if (alt) {
        p->alt_mask = alt;
    }
    if (super) {
        p->super_mask = super;
    }
    XFreeModifiermap(map);
}
Platform *x11_open(const Config *cfg, char *e, size_t n)
{
    pthread_once(&xlib_once, initialize_xlib);
    Xorg *p = calloc(1, sizeof(*p));
    if (!p) {
        fail(e, n, "out of memory opening Xorg");
        return NULL;
    }
    p->segment.shmid = -1;
    p->d = XOpenDisplay(NULL);
    if (!p->d) {
        free(p);
        fail(e, n,
             "cannot open DISPLAY; run cast in your Xorg session and check DISPLAY/XAUTHORITY");
        return NULL;
    }
    XSetErrorHandler(on_error);
    p->screen = DefaultScreen(p->d);
    p->root = RootWindow(p->d, p->screen);
    int randr_error;
    if (!XRRQueryExtension(p->d, &p->randr_base, &randr_error)) {
        fail(e, n,
             "Xorg display lacks XRandR monitor discovery; enable XRandR or choose the portal "
             "backend");
        x11_close((Platform *)p);
        return NULL;
    }
    p->shm = XShmQueryExtension(p->d);
    int composite_event, composite_error;
    p->composite = XCompositeQueryExtension(p->d, &composite_event, &composite_error);
    p->wm_state = XInternAtom(p->d, "WM_STATE", False);
    p->active_window = XInternAtom(p->d, "_NET_ACTIVE_WINDOW", False);
    p->wm_delete = XInternAtom(p->d, "WM_DELETE_WINDOW", False);
    int xi_event, xi_error, major = 2, minor = 2;
    if (XQueryExtension(p->d, "XInputExtension", &p->xi_opcode, &xi_event, &xi_error) &&
        XIQueryVersion(p->d, &major, &minor) == Success) {
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
        XISetMask(bits, XI_RawKeyPress);
        XISetMask(bits, XI_RawKeyRelease);
        XISetMask(bits, XI_RawButtonPress);
        XIEventMask mask = {XIAllMasterDevices, sizeof(bits), bits};
        begin(p);
        XISelectEvents(p->d, p->root, &mask, 1);
        p->xi = end(p);
    }
    int xkb_opcode, xkb_error;
    major = XkbMajorVersion;
    minor = XkbMinorVersion;
    p->xkb = XkbQueryExtension(p->d, &xkb_opcode, &p->xkb_base, &xkb_error, &major, &minor);
    if (p->xkb) {
        XkbSelectEvents(p->d, XkbUseCoreKbd,
                        XkbStateNotifyMask | XkbMapNotifyMask | XkbNewKeyboardNotifyMask,
                        XkbStateNotifyMask | XkbMapNotifyMask | XkbNewKeyboardNotifyMask);
        XkbStateRec state;
        if (XkbGetState(p->d, XkbUseCoreKbd, &state) == Success) {
            p->mods = state.mods;
            p->group = state.group;
        }
        modifier_masks(p);
    }
    XRRSelectInput(p->d, p->root,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    XSelectInput(p->d, p->root, StructureNotifyMask | SubstructureNotifyMask);
    p->crosshair = XCreateFontCursor(p->d, XC_crosshair);
    if (!strcmp(cfg->capture_kind, "window") && window_select(p, active_once(p), e, n) < 0) {
        x11_close((Platform *)p);
        return NULL;
    }
    if (x11_reconfigure((Platform *)p, cfg, e, n) < 0) {
        x11_close((Platform *)p);
        return NULL;
    }
    return (Platform *)p;
}
static void selection_stop(Xorg *p)
{
    if (p->selecting) {
        XUngrabPointer(p->d, CurrentTime);
        XUngrabKeyboard(p->d, CurrentTime);
    }
    if (p->selection) {
        XUnmapWindow(p->d, p->selection);
    }
    p->selecting = 0;
    p->dragging = false;
    p->owner = NULL;
    XFlush(p->d);
}
void x11_close(Platform *platform)
{
    Xorg *p = (Xorg *)platform;
    if (!p) {
        return;
    }
    if (p->d) {
        selection_stop(p);
        preview_stop(p);
        image_free(p);
        window_release(p);
        if (p->preview) {
            XDestroyWindow(p->d, p->preview);
        }
        if (p->selection) {
            XDestroyWindow(p->d, p->selection);
        }
        if (p->crosshair) {
            XFreeCursor(p->d, p->crosshair);
        }
        XCloseDisplay(p->d);
    }
    free(p);
}
Capabilities x11_capabilities(Platform *platform)
{
    Xorg *p = (Xorg *)platform;
    Capabilities c = {.capture = true,
                      .cursor_metadata = true,
                      .input = p->xi && p->xkb,
                      .region_selection = true,
                      .window_selection = p->composite,
                      .preview = true,
                      .panel_exclusion = true};
    snprintf(
        c.description, sizeof(c.description),
        "Xorg: %s capture, XRandR monitors, %s input, %s window pixmaps; cursor drawn separately",
        p->shm ? "MIT-SHM" : "XGetImage", c.input ? "XI2/XKB" : "no global",
        p->composite ? "XComposite" : "no");
    return c;
}
uint64_t x11_source_generation(Platform *platform)
{
    return ((Xorg *)platform)->generation;
}
typedef struct {
    unsigned shift;
    unsigned long mask;
} ColorChannel;
static ColorChannel color_channel(unsigned long mask)
{
    ColorChannel c = {0, mask};
    while (mask && !(mask & 1)) {
        mask >>= 1;
        c.shift++;
    }
    c.mask = mask;
    return c;
}
static unsigned channel(unsigned long pixel, ColorChannel c)
{
    if (!c.mask) {
        return 0;
    }
    unsigned long value = (pixel >> c.shift) & c.mask;
    return (unsigned)((value * 255 + c.mask / 2) / c.mask);
}
static void image_to_frame(XImage *image, Visual *visual, Frame *out)
{
    /* Most Xorg visuals have one byte per color in a 32-bit pixel. Choose the
     * byte positions once; handle XImage byte order independently of the CPU. */
    bool packed = image->format == ZPixmap && image->bits_per_pixel == 32 && image->xoffset == 0 &&
                  visual->green_mask == 0xff00 &&
                  ((visual->red_mask == 0xff0000 && visual->blue_mask == 0xff) ||
                   (visual->red_mask == 0xff && visual->blue_mask == 0xff0000));
    if (packed) {
        int red = visual->red_mask == 0xff0000 ? 2 : 0;
        int green = 1, blue = 2 - red;
        if (image->byte_order == MSBFirst) {
            red = 3 - red;
            green = 3 - green;
            blue = 3 - blue;
        }
        for (int y = 0; y < out->height; y++) {
            const uint8_t *s = (const uint8_t *)image->data + (size_t)y * image->bytes_per_line;
            uint8_t *d = out->data + (size_t)y * out->stride;
            for (int x = 0; x < out->width; x++, s += 4, d += 4) {
                d[0] = s[red];
                d[1] = s[green];
                d[2] = s[blue];
                d[3] = 255;
            }
        }
        return;
    }
    ColorChannel red = color_channel(visual->red_mask);
    ColorChannel green = color_channel(visual->green_mask);
    ColorChannel blue = color_channel(visual->blue_mask);
    for (int y = 0; y < out->height; y++) {
        uint8_t *d = out->data + (size_t)y * out->stride;
        for (int x = 0; x < out->width; x++, d += 4) {
            unsigned long value = XGetPixel(image, x, y);
            d[0] = (uint8_t)channel(value, red);
            d[1] = (uint8_t)channel(value, green);
            d[2] = (uint8_t)channel(value, blue);
            d[3] = 255;
        }
    }
}
static int source_rect(Xorg *p, Drawable *drawable, Visual **visual, int *depth, char *e, size_t n)
{
    if (!strcmp(p->kind, "window")) {
        XWindowAttributes a;
        Window child;
        int x, y;
        begin(p);
        Status ok = p->selected ? XGetWindowAttributes(p->d, p->selected, &a) : 0;
        bool pos = ok && XTranslateCoordinates(p->d, p->selected, p->root, 0, 0, &x, &y, &child);
        bool checked = end(p);
        if (!ok || !checked || !pos) {
            return fail(e, n,
                        "selected window was destroyed; choose capture window active/select or "
                        "capture monitor");
        }
        if (a.map_state != IsViewable) {
            /* Remapping replaces the window backing pixmap, even without a resize. */
            p->window_unmapped = true;
            return fail(e, n,
                        "selected window is minimized or unmapped; restore it or explicitly select "
                        "another source");
        }
        if (a.width <= 0 || a.height <= 0 || a.width > 16384 || a.height > 16384) {
            return fail(e, n, "selected window dimensions are unsupported");
        }
        if (p->window_unmapped || a.width != p->pixmap_w || a.height != p->pixmap_h ||
            !p->window_pixmap) {
            if (p->window_pixmap) {
                begin(p);
                XFreePixmap(p->d, p->window_pixmap);
                end(p);
            }
            begin(p);
            p->window_pixmap = XCompositeNameWindowPixmap(p->d, p->selected);
            checked = end(p);
            if (!checked || !p->window_pixmap) {
                p->window_pixmap = 0;
                return fail(e, n,
                            "resized window pixmap is unavailable; restore or reselect the window");
            }
            p->pixmap_w = a.width;
            p->pixmap_h = a.height;
            p->window_unmapped = false;
            image_free(p);
        }
        p->sx = x;
        p->sy = y;
        p->sw = a.width;
        p->sh = a.height;
        *drawable = p->window_pixmap;
        *visual = a.visual;
        *depth = a.depth;
        return 0;
    }
    Monitor m;
    if (find_monitor(p, p->monitor, &m) < 0) {
        snprintf(e, n,
                 "selected monitor '%s' disappeared; cast screen list then cast screen select NAME",
                 p->monitor);
        return -1;
    }
    if (!strcmp(p->kind, "region")) {
        if (p->rx < 0 || p->ry < 0 || p->rw < 1 || p->rh < 1 || p->rx > m.w - p->rw ||
            p->ry > m.h - p->rh) {
            return fail(
                e, n, "selected region no longer fits the monitor; explicitly select a new region");
        }
        p->sx = m.x + p->rx;
        p->sy = m.y + p->ry;
        p->sw = p->rw;
        p->sh = p->rh;
    } else {
        p->sx = m.x;
        p->sy = m.y;
        p->sw = m.w;
        p->sh = m.h;
    }
    *drawable = p->root;
    *visual = DefaultVisual(p->d, p->screen);
    *depth = DefaultDepth(p->d, p->screen);
    return 0;
}
typedef struct {
    int x, y, width, height;
    bool visible;
} WindowRect;

/* The root child enclosing a managed client includes its WM decorations. */
static bool window_rect(Xorg *p, Window window, bool include_frame, WindowRect *rect)
{
    memset(rect, 0, sizeof(*rect));
    XWindowAttributes attr;
    Window child;
    begin(p);
    Status exists = XGetWindowAttributes(p->d, window, &attr);
    bool visible = exists && attr.map_state == IsViewable;
    Window outer = window;
    bool tree_ok = true;
    if (exists && include_frame) {
        bool reached_root = false;
        for (int level = 0; level < 32; level++) {
            Window root, parent, *children = NULL;
            unsigned count;
            Status queried = XQueryTree(p->d, outer, &root, &parent, &children, &count);
            if (children) {
                XFree(children);
            }
            if (!queried || root != p->root || !parent) {
                tree_ok = false;
                break;
            }
            if (parent == p->root) {
                reached_root = true;
                break;
            }
            outer = parent;
        }
        tree_ok = tree_ok && reached_root;
        if (tree_ok) {
            exists = XGetWindowAttributes(p->d, outer, &attr);
        }
    }
    Status positioned = exists && tree_ok ? XTranslateCoordinates(p->d, outer, p->root, 0, 0,
                                                                  &rect->x, &rect->y, &child)
                                          : 0;
    bool checked = end(p);
    if (!checked || !exists || !tree_ok || !positioned) {
        return false;
    }
    rect->x -= attr.border_width;
    rect->y -= attr.border_width;
    rect->width = attr.width + 2 * attr.border_width;
    rect->height = attr.height + 2 * attr.border_width;
    rect->visible = visible && attr.map_state == IsViewable;
    return true;
}

static bool panel_identity(Xorg *p, Window window, int peer_pid)
{
    XWindowAttributes attr;
    XClassHint hint = {0};
    Atom pid_atom = XInternAtom(p->d, "_NET_WM_PID", False);
    Atom type_atom = XInternAtom(p->d, "_NET_WM_WINDOW_TYPE", False);
    Atom normal_atom = XInternAtom(p->d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *value = NULL;
    begin(p);
    Status exists = XGetWindowAttributes(p->d, window, &attr);
    bool normal = exists && attr.class == InputOutput && !attr.override_redirect;
    bool class_ok = XGetClassHint(p->d, window, &hint) && hint.res_class &&
                    !strcmp(hint.res_class, "CastPanel");
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    bool pid_ok = XGetWindowProperty(p->d, window, pid_atom, 0, 1, False, XA_CARDINAL, &type,
                                     &format, &count, &remaining, &value) == Success &&
                  type == XA_CARDINAL && format == 32 && count == 1 && !remaining && value &&
                  *(unsigned long *)value == (unsigned long)peer_pid;
    if (value) {
        XFree(value);
        value = NULL;
    }
    if (XGetWindowProperty(p->d, window, type_atom, 0, 16, False, XA_ATOM, &type, &format, &count,
                           &remaining, &value) != Success) {
        normal = false;
    } else if (type != None) {
        bool found = false;
        if (type == XA_ATOM && format == 32 && value && !remaining) {
            for (unsigned long i = 0; i < count; i++) {
                found = found || ((Atom *)value)[i] == normal_atom;
            }
        }
        normal = normal && found;
    }
    if (value) {
        XFree(value);
    }
    return end(p) && normal && class_ok && pid_ok;
}

int x11_panel_register(Platform *platform, uint64_t window, int peer_pid, char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    if (!window || window > UINT32_MAX || peer_pid <= 0 || window == p->root ||
        window == p->selected || window == p->preview || window == p->selection ||
        !panel_identity(p, (Window)window, peer_pid)) {
        return fail(
            e, n,
            "panel window must be a normal CastPanel window owned by the authenticated peer PID");
    }
    WindowRect rect;
    if (!window_rect(p, (Window)window, true, &rect)) {
        return fail(e, n, "panel window geometry is unavailable");
    }
    begin(p);
    XSelectInput(p->d, (Window)window, StructureNotifyMask | PropertyChangeMask);
    if (!end(p)) {
        return fail(e, n, "panel window disappeared during registration");
    }
    p->panel = (Window)window;
    p->panel_pid = peer_pid;
    snprintf(e, n, "panel registered: overlapping monitor/region pixels will be neutral-masked");
    return 0;
}

void x11_panel_unregister(Platform *platform)
{
    Xorg *p = (Xorg *)platform;
    p->panel = None;
    p->panel_pid = 0;
}

static bool rect_overlaps_source(const Xorg *p, const WindowRect *rect)
{
    return rect->visible && rect->x < p->sx + p->sw && p->sx < rect->x + rect->width &&
           rect->y < p->sy + p->sh && p->sy < rect->y + rect->height;
}

void x11_panel_status(Platform *platform, char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    if (!p->panel) {
        snprintf(e, n, "available: Xorg neutral masking; no panel window registered");
        return;
    }
    WindowRect rect;
    if (!window_rect(p, p->panel, true, &rect)) {
        x11_panel_unregister(platform);
        snprintf(e, n, "available: registered panel window disappeared");
    } else if (!rect.visible) {
        snprintf(e, n, "hidden: panel is outside visible captured pixels");
    } else if (!strcmp(p->kind, "window")) {
        snprintf(e, n,
                 "excluded: separate panel is absent from the application's XComposite pixmap");
    } else {
        Drawable drawable;
        Visual *visual;
        int depth;
        char error[CAST_ERR];
        if (source_rect(p, &drawable, &visual, &depth, error, sizeof(error)) < 0) {
            snprintf(e, n, "unavailable: capture source geometry is unavailable");
        } else {
            snprintf(e, n,
                     rect_overlaps_source(p, &rect)
                         ? "overlap: panel and WM frame are neutral-masked; covered content is lost"
                         : "outside source: panel does not overlap the captured rectangle");
        }
    }
}

static void mask_window_rect(Xorg *p, Frame *out, const WindowRect *rect)
{
    if (!rect_overlaps_source(p, rect)) {
        return;
    }
    int left = rect->x - p->sx, top = rect->y - p->sy;
    int right = left + rect->width, bottom = top + rect->height;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > out->width) {
        right = out->width;
    }
    if (bottom > out->height) {
        bottom = out->height;
    }
    for (int y = top; y < bottom; y++) {
        uint8_t *d = out->data + (size_t)y * out->stride + 4 * left;
        for (int x = left; x < right; x++, d += 4) {
            d[0] = (uint8_t)(p->preview_mask_color >> 16);
            d[1] = (uint8_t)(p->preview_mask_color >> 8);
            d[2] = (uint8_t)p->preview_mask_color;
            d[3] = 255;
        }
    }
}

int x11_capture(Platform *platform, Frame *out, Cursor *cursor, char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    memset(cursor, 0, sizeof(*cursor));
    if (p->source_error[0]) {
        snprintf(e, n, "%s", p->source_error);
        p->source_error[0] = 0;
        return -1;
    }
    Drawable drawable;
    Visual *visual;
    int depth;
    if (source_rect(p, &drawable, &visual, &depth, e, n) < 0) {
        return -1;
    }
    if (frame_alloc(out, p->sw, p->sh) < 0) {
        return fail(e, n, "cannot allocate Xorg capture frame");
    }
    bool outline = p->selecting && p->dragging && p->selection;
    WindowRect preview = {0}, panel = {0};
    if (drawable == p->root) {
        if (p->preview && !p->preview_disabled) {
            window_rect(p, p->preview, false, &preview);
        }
        if (p->panel && !window_rect(p, p->panel, true, &panel)) {
            x11_panel_unregister(platform);
        }
    }
    if (outline) {
        XUnmapWindow(p->d, p->selection);
    }
    if (outline) {
        XSync(p->d, False);
    }
    image_prepare(p, visual, depth, p->sw, p->sh);
    int x = drawable == p->root ? p->sx : 0, y = drawable == p->root ? p->sy : 0;
    XImage *image = p->image;
    begin(p);
    bool ok;
    if (image) {
        ok = XShmGetImage(p->d, drawable, image, x, y, AllPlanes);
    } else {
        image =
            XGetImage(p->d, drawable, x, y, (unsigned)p->sw, (unsigned)p->sh, AllPlanes, ZPixmap);
        ok = image != NULL;
    }
    ok = end(p) && ok;
    if (outline) {
        XMapRaised(p->d, p->selection);
    }
    if (!ok) {
        if (image && image != p->image) {
            XDestroyImage(image);
        }
        return fail(e, n,
                    "Xorg source acquisition failed (window/monitor changed); no alternate source "
                    "was selected");
    }
    /* Pixmap readback has no associated visual: XGetImage/XShmGetImage may return
     * zero channel masks. The selected window visual describes its backing pixmap. */
    image_to_frame(image, visual, out);
    /* Neutral masking happens in source pixels, before output zoom/layout composition. */
    if (drawable == p->root) {
        mask_window_rect(p, out, &preview);
        mask_window_rect(p, out, &panel);
    }
    if (image != p->image) {
        XDestroyImage(image);
    }
    Window root, child;
    int rx, ry, wx, wy;
    unsigned mask;
    if (XQueryPointer(p->d, p->root, &root, &child, &rx, &ry, &wx, &wy, &mask)) {
        cursor->x = rx - p->sx;
        cursor->y = ry - p->sy;
        cursor->valid = cursor->x >= 0 && cursor->y >= 0 && cursor->x < p->sw && cursor->y < p->sh;
    }
    out->ts_ns = cast_now_ns();
    cursor->ts_ns = out->ts_ns;
    return 0;
}
int x11_reconfigure(Platform *platform, const Config *cfg, char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    Monitor m;
    if (strcmp(cfg->capture_kind, "window")) {
        if (find_monitor(p, cfg->monitor, &m) < 0) {
            snprintf(
                e, n,
                "monitor '%s' is unavailable; use cast screen list to choose an attached monitor",
                cfg->monitor);
            return -1;
        }
        if (!strcmp(cfg->capture_kind, "region") &&
            (cfg->region_x < 0 || cfg->region_y < 0 || cfg->region_w < 1 || cfg->region_h < 1 ||
             cfg->region_x > m.w - cfg->region_w || cfg->region_y > m.h - cfg->region_h)) {
            return fail(e, n, "configured region must fit entirely inside the selected monitor");
        }
    } else if (!p->selected) {
        return fail(e, n,
                    "a window cannot be selected by configuration; start with monitor then use "
                    "cast capture window active/select");
    }
    if (cfg->preview == true && !x11_capabilities(platform).preview) {
        return fail(e, n, "preview is unavailable on this display");
    }
    selection_stop(p);
    bool changed =
        strcmp(p->kind, cfg->capture_kind) ||
        (!strcmp(cfg->capture_kind, "window") ? false : strcmp(p->monitor, m.name) != 0) ||
        p->rx != cfg->region_x || p->ry != cfg->region_y || p->rw != cfg->region_w ||
        p->rh != cfg->region_h;
    if (strcmp(cfg->capture_kind, "window")) {
        snprintf(p->monitor, sizeof(p->monitor), "%s", m.name);
    }
    snprintf(p->kind, sizeof(p->kind), "%s", cfg->capture_kind);
    p->rx = cfg->region_x;
    p->ry = cfg->region_y;
    p->rw = cfg->region_w;
    p->rh = cfg->region_h;
    if (changed) {
        p->generation++;
        image_free(p);
    }
    p->source_error[0] = 0;
    p->preview_disabled = false;
    p->preview_mask_color = cfg->pause_color;
    return 0;
}
static int selection_start(Xorg *p, Config *cfg, int kind, char *e, size_t n)
{
    if (p->selecting) {
        return fail(e, n, "selection already in progress; finish it or press Escape");
    }
    if (kind == 1) {
        Monitor m;
        if (find_monitor(p, p->monitor, &m) < 0) {
            return fail(e, n, "selected monitor is unavailable; select an attached monitor first");
        }
    } else if (!p->composite) {
        return fail(e, n, "window selection needs XComposite; use capture monitor/region");
    }
    int ptr =
        XGrabPointer(p->d, p->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, p->crosshair, CurrentTime);
    if (ptr != GrabSuccess) {
        return fail(e, n,
                    "cannot start selection: another application holds the pointer grab; close its "
                    "menu/dialog and retry");
    }
    int key = XGrabKeyboard(p->d, p->root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (key != GrabSuccess) {
        XUngrabPointer(p->d, CurrentTime);
        return fail(e, n,
                    "cannot start selection: another application holds the keyboard grab; retry "
                    "after closing its menu/dialog");
    }
    p->selecting = kind;
    p->owner = cfg;
    p->dragging = false;
    p->last_keycode = 0;
    snprintf(e, n, "%s selection started; %s; Escape cancels and retains the previous source",
             kind == 1 ? "region" : "window",
             kind == 1 ? "drag with the left button inside the selected monitor"
                       : "click an application window");
    XFlush(p->d);
    return 0;
}
int x11_command(Platform *platform, Config *cfg, int argc, char **argv, char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    if (argc >= 2 && !strcmp(argv[0], "screen")) {
        Monitor list[MONITOR_MAX];
        int count = monitors(p, list, MONITOR_MAX);
        if (argc == 2 && !strcmp(argv[1], "list")) {
            e[0] = 0;
            char row[320];
            for (int i = 0; i < count; i++) {
                snprintf(row, sizeof(row), "%s%.127s %dx%d%+d%+d%s\n",
                         !strcmp(list[i].name, p->monitor) ? "* " : "  ", list[i].name, list[i].w,
                         list[i].h, list[i].x, list[i].y, list[i].primary ? " primary" : "");
                append(e, n, row);
            }
            if (!count) {
                return fail(e, n, "no active XRandR monitors; attach/enable a display");
            }
            return 0;
        }
        const char *name = NULL;
        if (argc == 3 && !strcmp(argv[1], "select")) {
            name = argv[2];
        } else if (argc == 2 && !strcmp(argv[1], "next")) {
            if (!count) {
                return fail(e, n, "no monitors available");
            }
            int index = -1;
            for (int i = 0; i < count; i++) {
                if (!strcmp(list[i].name, p->monitor)) {
                    index = i;
                }
            }
            name = list[(index + 1) % count].name;
        } else {
            return fail(e, n, "usage: cast screen list|next; cast screen select MONITOR");
        }
        Monitor m;
        if (find_monitor(p, name, &m) < 0) {
            snprintf(e, n, "unknown monitor '%s'; cast screen list shows attached names", name);
            return -1;
        }
        Config candidate = *cfg;
        snprintf(candidate.monitor, sizeof(candidate.monitor), "%s", m.name);
        snprintf(candidate.capture_kind, sizeof(candidate.capture_kind), "monitor");
        candidate.zoom_factor = 1;
        if (x11_reconfigure(platform, &candidate, e, n) < 0) {
            return -1;
        }
        *cfg = candidate;
        snprintf(e, n, "capture monitor: %s; zoom reset", m.name);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[0], "capture")) {
        if (argc == 2 && !strcmp(argv[1], "monitor")) {
            Config candidate = *cfg;
            snprintf(candidate.monitor, sizeof(candidate.monitor), "%s", p->monitor);
            snprintf(candidate.capture_kind, sizeof(candidate.capture_kind), "monitor");
            candidate.zoom_factor = 1;
            if (x11_reconfigure(platform, &candidate, e, n) < 0) {
                return -1;
            }
            *cfg = candidate;
            snprintf(e, n, "capture monitor: %s; zoom reset", p->monitor);
            return 0;
        }
        if (!strcmp(argv[1], "region")) {
            if (argc == 3 && !strcmp(argv[2], "select")) {
                return selection_start(p, cfg, 1, e, n);
            }
            if (argc == 6) {
                Config candidate = *cfg;
                if (!integer(argv[2], &candidate.region_x) ||
                    !integer(argv[3], &candidate.region_y) ||
                    !integer(argv[4], &candidate.region_w) ||
                    !integer(argv[5], &candidate.region_h)) {
                    return fail(e, n,
                                "region requires integer X Y WIDTH HEIGHT relative to the selected "
                                "monitor");
                }
                snprintf(candidate.monitor, sizeof(candidate.monitor), "%s", p->monitor);
                snprintf(candidate.capture_kind, sizeof(candidate.capture_kind), "region");
                candidate.zoom_factor = 1;
                if (x11_reconfigure(platform, &candidate, e, n) < 0) {
                    return -1;
                }
                *cfg = candidate;
                snprintf(e, n, "capture region: %d,%d %dx%d on %s; zoom reset", p->rx, p->ry, p->rw,
                         p->rh, p->monitor);
                return 0;
            }
            return fail(e, n,
                        "usage: cast capture region select; cast capture region X Y WIDTH HEIGHT");
        }
        if (!strcmp(argv[1], "window")) {
            if (argc == 3 && !strcmp(argv[2], "select")) {
                return selection_start(p, cfg, 2, e, n);
            }
            if (argc == 3 && !strcmp(argv[2], "active")) {
                Window win = active_once(p);
                selection_stop(p);
                if (window_select(p, win, e, n) < 0) {
                    return -1;
                }
                snprintf(cfg->capture_kind, sizeof(cfg->capture_kind), "window");
                cfg->zoom_factor = 1;
                snprintf(e, n, "capture window: 0x%lx (active once); zoom reset", win);
                return 0;
            }
            return fail(e, n, "usage: cast capture window select|active");
        }
        return fail(e, n,
                    "usage: cast capture monitor|region|window; cast capture fit contain|cover");
    }
    return fail(e, n, "unsupported Xorg platform command");
}
static void selection_outline(Xorg *p, int x, int y)
{
    int left = x < p->drag_x ? x : p->drag_x, top = y < p->drag_y ? y : p->drag_y,
        w = abs(x - p->drag_x), h = abs(y - p->drag_y);
    if (w < 2 || h < 2) {
        return;
    }
    if (!p->selection) {
        XSetWindowAttributes a = {
            .override_redirect = True, .background_pixel = 0x00ff99, .save_under = False};
        p->selection = XCreateWindow(p->d, p->root, left, top, (unsigned)w, (unsigned)h, 0,
                                     CopyFromParent, InputOutput, CopyFromParent,
                                     CWOverrideRedirect | CWBackPixel | CWSaveUnder, &a);
        XStoreName(p->d, p->selection, "cast region selection");
    }
    XMoveResizeWindow(p->d, p->selection, left, top, (unsigned)w, (unsigned)h);
    XRectangle rects[4] = {{0, 0, (unsigned short)w, 2},
                           {0, (short)(h - 2), (unsigned short)w, 2},
                           {0, 0, 2, (unsigned short)h},
                           {(short)(w - 2), 0, 2, (unsigned short)h}};
    XShapeCombineRectangles(p->d, p->selection, ShapeBounding, 0, 0, rects, 4, ShapeSet, Unsorted);
    XMapRaised(p->d, p->selection);
    XFlush(p->d);
}
static void selection_event(Xorg *p, XEvent *event)
{
    if (event->type == KeyPress && XLookupKeysym(&event->xkey, 0) == XK_Escape) {
        selection_stop(p);
        return;
    }
    if (event->type == MotionNotify && p->selecting == 1 && p->dragging) {
        p->last_x = event->xmotion.x_root;
        p->last_y = event->xmotion.y_root;
        selection_outline(p, p->last_x, p->last_y);
        return;
    }
    if (event->type == ButtonPress && event->xbutton.button == Button1) {
        if (p->selecting == 1) {
            p->drag_x = p->last_x = event->xbutton.x_root;
            p->drag_y = p->last_y = event->xbutton.y_root;
            p->dragging = true;
        } else {
            Window root, child;
            int rx, ry, wx, wy;
            unsigned mask;
            XQueryPointer(p->d, p->root, &root, &child, &rx, &ry, &wx, &wy, &mask);
            Config *cfg = p->owner;
            selection_stop(p);
            int remaining = 1024;
            Window client = find_client(p, child, 0, &remaining);
            char error[CAST_ERR];
            if (window_select(p, client ? client : child, error, sizeof(error)) < 0) {
                snprintf(p->source_error, sizeof(p->source_error),
                         "window selection cancelled: %.900s", error);
            } else if (cfg) {
                snprintf(cfg->capture_kind, sizeof(cfg->capture_kind), "window");
                cfg->zoom_factor = 1;
            }
        }
        return;
    }
    if (event->type == ButtonRelease && event->xbutton.button == Button1 && p->selecting == 1 &&
        p->dragging) {
        int x = event->xbutton.x_root, y = event->xbutton.y_root,
            left = x < p->drag_x ? x : p->drag_x, top = y < p->drag_y ? y : p->drag_y,
            w = abs(x - p->drag_x), h = abs(y - p->drag_y);
        Config *cfg = p->owner;
        Monitor m;
        bool available = find_monitor(p, p->monitor, &m) == 0;
        selection_stop(p);
        if (!available || left < m.x || top < m.y || w < 1 || h < 1 || left > m.x + m.w - w ||
            top > m.y + m.h - h) {
            snprintf(p->source_error, sizeof(p->source_error),
                     "region selection cancelled: drag a nonempty region entirely inside the "
                     "selected monitor");
            return;
        }
        Config candidate = *cfg;
        snprintf(candidate.capture_kind, sizeof(candidate.capture_kind), "region");
        snprintf(candidate.monitor, sizeof(candidate.monitor), "%s", p->monitor);
        candidate.region_x = left - m.x;
        candidate.region_y = top - m.y;
        candidate.region_w = w;
        candidate.region_h = h;
        candidate.zoom_factor = 1;
        char error[CAST_ERR];
        if (x11_reconfigure((Platform *)p, &candidate, error, sizeof(error)) < 0) {
            snprintf(p->source_error, sizeof(p->source_error), "region selection cancelled: %.900s",
                     error);
        } else {
            *cfg = candidate;
        }
    }
}
static bool list_contains(const char *list, const char *value)
{
    const char *p = list;
    size_t wanted = strlen(value);
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *endp = strchr(p, ',');
        if (!endp) {
            endp = p + strlen(p);
        }
        const char *trim = endp;
        while (trim > p && trim[-1] == ' ') {
            trim--;
        }
        if ((size_t)(trim - p) == wanted && !strncmp(p, value, wanted)) {
            return true;
        }
        p = *endp ? endp + 1 : endp;
    }
    return false;
}
static void key_event(Xorg *p, const Config *cfg, Compositor *comp, const XIRawEvent *event)
{
    if (event->evtype == XI_RawKeyRelease) {
        if (p->last_keycode == event->detail) {
            p->last_keycode = 0;
        }
        return;
    }
    if (!cfg->keys || !p->xkb || p->privacy || p->selecting || (event->flags & XIKeyRepeat)) {
        return;
    }
    KeySym sym = NoSymbol;
    unsigned consumed = 0;
    if (!XkbLookupKeySym(p->d, (KeyCode)event->detail, XkbBuildCoreState(p->mods, p->group),
                         &consumed, &sym)) {
        return;
    }
    if (IsModifierKey(sym) || sym == XK_ISO_Level3_Shift || sym == XK_ISO_Level5_Shift) {
        return;
    }
    const char *name = XKeysymToString(sym);
    char atom[80];
    if (!name) {
        snprintf(atom, sizeof(atom), "U+%04lX", sym >= 0x01000000 ? sym - 0x01000000 : sym);
        name = atom;
    }
    if (sym == XK_space) {
        name = "Space";
    } else if (sym >= XK_a && sym <= XK_z) {
        atom[0] = (char)('A' + sym - XK_a);
        atom[1] = 0;
        name = atom;
    } else if (sym >= XK_A && sym <= XK_Z) {
        atom[0] = (char)sym;
        atom[1] = 0;
        name = atom;
    } else if (sym >= 0x20 && sym <= 0x7e) {
        atom[0] = (char)sym;
        atom[1] = 0;
        name = atom;
    }
    unsigned mods = p->mods;
    bool shortcut = mods & (ControlMask | p->alt_mask | p->super_mask);
    if (!strcmp(cfg->keys_mode, "shortcuts") && !shortcut &&
        !list_contains(cfg->keys_navigation, name)) {
        return;
    }
    char combo[CAST_TEXT] = "";
    if (mods & ControlMask) {
        append(combo, sizeof(combo), "Ctrl+");
    }
    if (mods & p->alt_mask) {
        append(combo, sizeof(combo), "Alt+");
    }
    if (mods & p->super_mask) {
        append(combo, sizeof(combo), "Super+");
    }
    if (mods & ShiftMask) {
        append(combo, sizeof(combo), "Shift+");
    }
    append(combo, sizeof(combo), name);
    if (list_contains(cfg->keys_filter, combo) || list_contains(cfg->keys_filter, name)) {
        return;
    }
    if (p->last_keycode == event->detail && event->time - p->last_key_time < 100) {
        return;
    }
    p->last_keycode = event->detail;
    p->last_key_time = event->time;
    compositor_key(comp, combo, cast_now_ns());
}
void x11_events(Platform *platform, Compositor *comp, const Config *cfg, bool privacy)
{
    Xorg *p = (Xorg *)platform;
    bool drop_pending = privacy != p->privacy || p->generation != p->seen_generation;
    if (drop_pending) {
        /* Round-trip before draining so input already queued at the server cannot
         * cross a pause/resume or source boundary on the next iteration. */
        XSync(p->d, False);
        compositor_clear(comp);
        p->last_keycode = 0;
        p->seen_generation = p->generation;
        preview_neutral(p);
        p->preview_epoch++;
    }
    p->privacy = privacy;
    while (XPending(p->d)) {
        XEvent ev;
        XNextEvent(p->d, &ev);
        if (ev.type == GenericEvent && ev.xcookie.extension == p->xi_opcode &&
            XGetEventData(p->d, &ev.xcookie)) {
            XIRawEvent *raw = ev.xcookie.data;
            if (raw && !drop_pending &&
                (ev.xcookie.evtype == XI_RawKeyPress || ev.xcookie.evtype == XI_RawKeyRelease)) {
                key_event(p, cfg, comp, raw);
            } else if (raw && !drop_pending && ev.xcookie.evtype == XI_RawButtonPress && !privacy &&
                       !p->selecting && cfg->clicks && raw->detail >= 1 && raw->detail <= 3) {
                Window root, child;
                int x, y, wx, wy;
                unsigned mask;
                if (XQueryPointer(p->d, p->root, &root, &child, &x, &y, &wx, &wy, &mask)) {
                    x -= p->sx;
                    y -= p->sy;
                    if (x >= 0 && y >= 0 && x < p->sw && y < p->sh) {
                        compositor_click(comp, x, y, raw->detail, cast_now_ns());
                    }
                }
            }
            XFreeEventData(p->d, &ev.xcookie);
            continue;
        }
        if (p->xkb && ev.type == p->xkb_base) {
            XkbEvent *k = (XkbEvent *)&ev;
            if (k->any.xkb_type == XkbStateNotify) {
                p->mods = k->state.mods;
                p->group = (unsigned)k->state.group;
            } else {
                if (k->any.xkb_type == XkbMapNotify) {
                    XkbRefreshKeyboardMapping(&k->map);
                }
                modifier_masks(p);
                p->last_keycode = 0;
            }
            continue;
        }
        if (ev.type == MappingNotify) {
            XRefreshKeyboardMapping(&ev.xmapping);
            modifier_masks(p);
            p->last_keycode = 0;
            continue;
        }
        if (ev.type == p->randr_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration(&ev);
            continue;
        }
        if (ev.type == PropertyNotify && ev.xproperty.window == p->panel &&
            !panel_identity(p, p->panel, p->panel_pid)) {
            x11_panel_unregister(platform);
        }
        if (ev.type == DestroyNotify && ev.xdestroywindow.window == p->panel) {
            x11_panel_unregister(platform);
        }
        if (ev.type == UnmapNotify && ev.xunmap.window == p->selected) {
            p->window_unmapped = true;
        }
        if (p->selecting) {
            selection_event(p, &ev);
        }
        if (ev.type == ClientMessage && ev.xclient.window == p->preview &&
            (Atom)ev.xclient.data.l[0] == p->wm_delete) {
            XUnmapWindow(p->d, p->preview);
            p->preview_disabled = true;
            p->preview_dragging = false;
        }
        if (ev.type == DestroyNotify && ev.xdestroywindow.window == p->preview) {
            p->preview = 0;
            p->preview_disabled = true;
            p->preview_dragging = false;
        }
        if (ev.type == ConfigureNotify && ev.xconfigure.window == p->preview) {
            p->preview_w = ev.xconfigure.width;
            p->preview_h = ev.xconfigure.height;
            p->preview_border = ev.xconfigure.border_width;
        }
        if (ev.type == ButtonPress && ev.xbutton.window == p->preview &&
            ev.xbutton.button == Button1 && ev.xbutton.y < 32) {
            p->preview_dragging = true;
            p->preview_drag_x = ev.xbutton.x + p->preview_border;
            p->preview_drag_y = ev.xbutton.y + p->preview_border;
        }
        if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
            p->preview_dragging = false;
        }
        if (ev.type == MotionNotify && ev.xmotion.window == p->preview && p->preview_dragging) {
            int x = ev.xmotion.x_root - p->preview_drag_x;
            int y = ev.xmotion.y_root - p->preview_drag_y;
            int max_x = DisplayWidth(p->d, p->screen) - p->preview_w - 2 * p->preview_border;
            int max_y = DisplayHeight(p->d, p->screen) - p->preview_h - 2 * p->preview_border;
            if (max_x < 0) {
                max_x = 0;
            }
            if (max_y < 0) {
                max_y = 0;
            }
            x = x < 0 ? 0 : x > max_x ? max_x : x;
            y = y < 0 ? 0 : y > max_y ? max_y : y;
            XMoveWindow(p->d, p->preview, x, y);
            XFlush(p->d);
        }
    }
    /* A selection may have committed while draining. Drop the selection's own input. */
    if (p->seen_generation != p->generation) {
        compositor_clear(comp);
        p->seen_generation = p->generation;
        p->last_keycode = 0;
    }
}
static bool rectangles_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}
static void preview_neutral(Xorg *p)
{
    if (p->preview) {
        /* Keep the floating window mapped while a new epoch is prepared. Only the
         * main connection publishes pixmaps, so an old worker cannot undo this. */
        XSetWindowBackground(p->d, p->preview, 0x111111);
        XClearWindow(p->d, p->preview);
        XFlush(p->d);
    }
}
static void preview_create(Xorg *p, const Config *cfg)
{
    int w = 640, h = cfg->height * 640 / cfg->width + 32;
    if (h > 600) {
        h = 600;
        w = cfg->width * (h - 32) / cfg->height;
    }
    if (w < 160) {
        w = 160;
    }
    if (h < 100) {
        h = 100;
    }
    int x = 24, y = 24;
    Monitor list[MONITOR_MAX];
    int count = monitors(p, list, MONITOR_MAX);
    /* Prefer a monitor outside the root-captured rectangle. Overlap is neutral-masked. */
    for (int i = 0; i < count; i++) {
        if (!rectangles_overlap(list[i].x, list[i].y, list[i].w, list[i].h, p->sx, p->sy, p->sw,
                                p->sh)) {
            x = list[i].x + 24;
            y = list[i].y + 24;
            break;
        }
    }
    int root_w = DisplayWidth(p->d, p->screen), root_h = DisplayHeight(p->d, p->screen);
    if (w > root_w) {
        w = root_w;
    }
    if (h > root_h) {
        h = root_h;
    }
    if (x > root_w - w) {
        x = root_w - w;
    }
    if (y > root_h - h) {
        y = root_h - h;
    }
    XSetWindowAttributes attr = {.override_redirect = True, .background_pixel = 0x111111};
    p->preview =
        XCreateWindow(p->d, p->root, x, y, (unsigned)w, (unsigned)h, 0, CopyFromParent, InputOutput,
                      CopyFromParent, CWOverrideRedirect | CWBackPixel, &attr);
    p->preview_w = w;
    p->preview_h = h;
    p->preview_border = 0;
    p->preview_dragging = false;
    XStoreName(p->d, p->preview, "cast output preview");
    XClassHint hint = {.res_name = "cast-preview", .res_class = "CastPreview"};
    XSetClassHint(p->d, p->preview, &hint);
    XSetWMProtocols(p->d, p->preview, &p->wm_delete, 1);
    XSelectInput(p->d, p->preview,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                     Button1MotionMask);
    /* Map only after the worker has painted an accepted current-epoch frame. */
    /* Its second connection must not address a window before creation completes. */
    XSync(p->d, False);
}
/* The worker prepares complete pixmaps on its own connection. Only the main
 * connection can install an accepted epoch's pixmap in the visible window. */
static Pixmap preview_paint(Display *d, const Frame *frame, Window window, int w, int h,
                            const char *label, unsigned long color)
{
    int screen = DefaultScreen(d);
    XImage *im = XCreateImage(d, DefaultVisual(d, screen), (unsigned)DefaultDepth(d, screen),
                              ZPixmap, 0, NULL, (unsigned)w, (unsigned)h, 32, 0);
    if (!im) {
        return None;
    }
    im->data = calloc((size_t)im->bytes_per_line, h);
    if (!im->data) {
        XDestroyImage(im);
        return None;
    }
    double scale = fmin((double)w / frame->width, (double)h / frame->height);
    int iw = (int)(frame->width * scale), ih = (int)(frame->height * scale);
    int ox = (w - iw) / 2, oy = (h - ih) / 2;
    for (int y = 0; y < ih; y++) {
        for (int x = 0; x < iw; x++) {
            int sx = x * frame->width / iw, sy = y * frame->height / ih;
            const uint8_t *q = frame->data + (size_t)sy * frame->stride + sx * 4;
            unsigned long value = 0;
            unsigned rgb[3] = {q[0], q[1], q[2]};
            unsigned long masks[3] = {im->red_mask, im->green_mask, im->blue_mask};
            for (int c = 0; c < 3; c++) {
                unsigned shift = 0;
                unsigned long m = masks[c];
                if (!m) {
                    continue;
                }
                while (!(m & 1)) {
                    m >>= 1;
                    shift++;
                }
                value |= ((rgb[c] * m + 127) / 255) << shift;
            }
            XPutPixel(im, ox + x, oy + y, value);
        }
    }
    XSync(d, False);
    xerror = 0;
    /* Unmapped Windows clip direct painting. Paint a server Pixmap and install it
     * as the background, so a later safe map starts with complete current pixels. */
    Pixmap pixmap = XCreatePixmap(d, window, (unsigned)w, (unsigned)(h + 32),
                                  (unsigned)DefaultDepth(d, screen));
    GC gc = XCreateGC(d, pixmap, 0, NULL);
    XPutImage(d, pixmap, gc, im, 0, 0, 0, 32, (unsigned)w, (unsigned)h);
    XSetForeground(d, gc, 0x111111);
    XFillRectangle(d, pixmap, gc, 0, 0, (unsigned)w, 32);
    XSetForeground(d, gc, color);
    XDrawString(d, pixmap, gc, 10, 21, label, (int)strlen(label));
    XFreeGC(d, gc);
    XDestroyImage(im);
    XSync(d, False);
    if (xerror) {
        XFreePixmap(d, pixmap);
        return None;
    }
    return pixmap;
}
static void *preview_run(void *data)
{
    Preview *p = data;
    Display *d = NULL;
    pthread_mutex_lock(&p->mutex);
    while (!p->stop) {
        if (!p->pending) {
            pthread_cond_wait(&p->changed, &p->mutex);
            continue;
        }
        Frame frame = p->frame;
        memset(&p->frame, 0, sizeof(p->frame));
        Window window = p->window;
        int w = p->width, h = p->height;
        uint64_t epoch = p->epoch;
        unsigned long color = p->color;
        char label[sizeof(p->label)];
        snprintf(label, sizeof(label), "%s", p->label);
        p->pending = false;
        pthread_mutex_unlock(&p->mutex);
        if (!d) {
            d = XOpenDisplay(NULL);
        }
        Pixmap pixmap = d ? preview_paint(d, &frame, window, w, h, label, color) : None;
        frame_free(&frame);
        pthread_mutex_lock(&p->mutex);
        if (!pixmap) {
            snprintf(p->error, sizeof(p->error), "%s; toggle preview off/on",
                     d ? "preview painting failed" : "cannot open preview display connection");
        } else {
            if (p->ready_pixmap) {
                XFreePixmap(d, p->ready_pixmap);
            }
            p->ready_pixmap = pixmap;
            p->ready_epoch = epoch;
            p->error[0] = 0;
        }
    }
    pthread_mutex_unlock(&p->mutex);
    if (d) {
        if (p->ready_pixmap) {
            XFreePixmap(d, p->ready_pixmap);
            p->ready_pixmap = None;
        }
        XCloseDisplay(d);
    }
    return NULL;
}
static int preview_start(Xorg *p, char *e, size_t n)
{
    if (p->preview_worker) {
        return 0;
    }
    Preview *worker = calloc(1, sizeof(*worker));
    if (!worker) {
        return fail(e, n, "cannot allocate preview worker");
    }
    pthread_mutex_init(&worker->mutex, NULL);
    pthread_cond_init(&worker->changed, NULL);
    int rc = pthread_create(&worker->thread, NULL, preview_run, worker);
    if (rc != 0) {
        pthread_cond_destroy(&worker->changed);
        pthread_mutex_destroy(&worker->mutex);
        free(worker);
        snprintf(e, n, "cannot start preview worker: %s", strerror(rc));
        return -1;
    }
    p->preview_worker = worker;
    return 0;
}
static void preview_stop(Xorg *p)
{
    Preview *worker = p->preview_worker;
    if (!worker) {
        return;
    }
    pthread_mutex_lock(&worker->mutex);
    worker->stop = true;
    pthread_cond_signal(&worker->changed);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    frame_free(&worker->frame);
    pthread_cond_destroy(&worker->changed);
    pthread_mutex_destroy(&worker->mutex);
    free(worker);
    p->preview_worker = NULL;
}
int x11_preview(Platform *platform, const Frame *frame, const State *state, const Config *cfg,
                char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    if (!cfg->preview) {
        if (p->preview) {
            XUnmapWindow(p->d, p->preview);
            XFlush(p->d);
        }
        if (p->preview_enabled) {
            p->preview_epoch++;
            p->preview_enabled = false;
        }
        Preview *worker = p->preview_worker;
        if (worker && pthread_mutex_trylock(&worker->mutex) == 0) {
            frame_free(&worker->frame);
            worker->pending = false;
            worker->error[0] = 0;
            pthread_mutex_unlock(&worker->mutex);
        }
        p->preview_disabled = false;
        p->preview_dragging = false;
        return 0;
    }
    if (p->preview_disabled || !frame || !frame->data) {
        return 0;
    }
    bool record = !strcmp(cfg->preview_target, "record");
    unsigned flags = (unsigned)state->live_paused | ((unsigned)state->live_frozen << 1) |
                     ((unsigned)state->recording << 2) | ((unsigned)state->record_paused << 3) |
                     ((unsigned)record << 4);
    if (!p->preview_enabled || flags != p->preview_state || p->preview_source != p->generation) {
        p->preview_epoch++;
        p->preview_state = flags;
        p->preview_source = p->generation;
        p->preview_enabled = true;
        p->last_preview = 0;
        preview_neutral(p);
    }
    if (!p->preview) {
        preview_create(p, cfg);
    }
    if (preview_start(p, e, n) < 0) {
        return -1;
    }
    Preview *worker = p->preview_worker;
    if (pthread_mutex_trylock(&worker->mutex) != 0) {
        return 0; /* A busy preview loses this update, never the live capture cadence. */
    }
    if (worker->ready_pixmap && worker->ready_epoch == p->preview_epoch) {
        XSetWindowBackgroundPixmap(p->d, p->preview, worker->ready_pixmap);
        XClearWindow(p->d, p->preview);
        XMapWindow(p->d, p->preview);
        XFreePixmap(p->d, worker->ready_pixmap);
        worker->ready_pixmap = None;
        XFlush(p->d);
    }
    if (worker->error[0]) {
        snprintf(e, n, "%s", worker->error);
        pthread_mutex_unlock(&worker->mutex);
        return -1;
    }
    uint64_t now = cast_now_ns();
    if (p->last_preview && now - p->last_preview < 66666666) {
        pthread_mutex_unlock(&worker->mutex);
        return 0;
    }
    int w = p->preview_w, h = p->preview_h - 32;
    if (w < 1 || h < 1 || w > 4096 || h > 4096) {
        pthread_mutex_unlock(&worker->mutex);
        return fail(e, n, "preview window size is unsupported; resize it below 4096 pixels");
    }
    if (frame_copy(&worker->frame, frame) < 0) {
        pthread_mutex_unlock(&worker->mutex);
        return fail(e, n, "cannot allocate preview queue frame");
    }
    p->last_preview = now;
    worker->window = p->preview;
    worker->width = w;
    worker->height = h;
    worker->epoch = p->preview_epoch;
    worker->color = state->live_paused ? 0xffc65c : state->live_frozen ? 0x7ec8ff : 0x64e69f;
    const char *live = state->live_paused ? "PAUSED" : state->live_frozen ? "FROZEN" : "LIVE";
    snprintf(worker->label, sizeof(worker->label), "%s | %s | preview: %s", live,
             state->recording ? (state->record_paused ? "RECORDING-PAUSED" : "RECORDING")
                              : "RECORD OFF",
             cfg->preview_target);
    worker->pending = true;
    pthread_cond_signal(&worker->changed);
    pthread_mutex_unlock(&worker->mutex);
    return 0;
}
void x11_doctor(const Config *cfg, char *e, size_t n)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        snprintf(e, n,
                 "Xorg: cannot open DISPLAY. Run inside the graphical session; inspect DISPLAY and "
                 "XAUTHORITY. No privileges are needed.");
        return;
    }
    int major = 2, minor = 0, opcode, event, error;
    bool xi = XQueryExtension(d, "XInputExtension", &opcode, &event, &error) &&
              XIQueryVersion(d, &major, &minor) == Success;
    bool shm = XShmQueryExtension(d), rr = XRRQueryExtension(d, &event, &error),
         comp = XCompositeQueryExtension(d, &event, &error);
    Xorg p = {.d = d, .root = DefaultRootWindow(d)};
    Monitor m;
    char source[256];
    if (!rr) {
        snprintf(source, sizeof(source), "XRandR missing; monitor capture unavailable");
    } else if (find_monitor(&p, cfg->monitor, &m) < 0) {
        snprintf(source, sizeof(source), "requested monitor is unavailable; run cast screen list");
    } else {
        snprintf(source, sizeof(source), "monitor %s %dx%d%+d%+d", m.name, m.w, m.h, m.x, m.y);
    }
    snprintf(e, n,
             "Xorg: DISPLAY connected; %s; MIT-SHM %s (XGetImage fallback); XI2 %s; XComposite %s. "
             "Preview is an unmanaged floating window; overlapping root pixels are neutral-masked.",
             source, shm ? "available" : "missing",
             xi ? "available" : "missing: keys/clicks unsupported",
             comp ? "available" : "missing: window capture unsupported");
    XCloseDisplay(d);
}
