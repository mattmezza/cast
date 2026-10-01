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
    Display *d;
    Window root, selected, preview, selection;
    GC preview_gc;
    Atom wm_delete, wm_state, active_window;
    int screen, xi_opcode, randr_base, xkb_base;
    bool xi, shm, composite, xkb, redirected, shm_attached, privacy, preview_disabled;
    XImage *image, *preview_image;
    XShmSegmentInfo segment;
    int image_depth;
    Visual *image_visual;
    Pixmap window_pixmap;
    int pixmap_w, pixmap_h;
    unsigned generation, seen_generation;
    char monitor[128], kind[16], source_error[CAST_ERR];
    int rx, ry, rw, rh, sx, sy, sw, sh;
    unsigned mods, group, alt_mask, super_mask;
    int last_keycode;
    Time last_key_time;
    int selecting, drag_x, drag_y, last_x, last_y;
    bool dragging;
    Config *owner; /* Runtime config object provided by command; daemon owns its lifetime. */
    X11Cursor crosshair;
    int preview_w, preview_h;
    uint64_t last_preview;
} Xorg;
static int xerror;
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
static Window find_client(Xorg *p, Window win, int depth, int *remaining)
{
    if (!win || win == p->preview || win == p->selection || depth > 12 || --*remaining < 0) {
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
    if (!win || win == p->root || win == p->preview || win == p->selection) {
        return fail(e, n, "select a normal application window, not the desktop or cast preview");
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
    int xi_event, xi_error, major = 2, minor = 0;
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
        image_free(p);
        window_release(p);
        if (p->preview_image) {
            XDestroyImage(p->preview_image);
        }
        if (p->preview_gc) {
            XFreeGC(p->d, p->preview_gc);
        }
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
                      .preview = true};
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
static unsigned channel(unsigned long pixel, unsigned long mask)
{
    if (!mask) {
        return 0;
    }
    unsigned shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    unsigned long value = (pixel >> shift) & mask;
    return (unsigned)((value * 255 + mask / 2) / mask);
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
            return fail(e, n,
                        "selected window is minimized or unmapped; restore it or explicitly select "
                        "another source");
        }
        if (a.width <= 0 || a.height <= 0 || a.width > 16384 || a.height > 16384) {
            return fail(e, n, "selected window dimensions are unsupported");
        }
        if (a.width != p->pixmap_w || a.height != p->pixmap_h || !p->window_pixmap) {
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
    bool hide_preview = false;
    if (drawable == p->root && p->preview && !p->preview_disabled) {
        XWindowAttributes attr;
        Window child;
        int px, py;
        begin(p);
        Status exists = XGetWindowAttributes(p->d, p->preview, &attr);
        Status position =
            exists ? XTranslateCoordinates(p->d, p->preview, p->root, 0, 0, &px, &py, &child) : 0;
        bool checked = end(p);
        hide_preview = checked && exists && position && attr.map_state == IsViewable &&
                       px < p->sx + p->sw && p->sx < px + attr.width && py < p->sy + p->sh &&
                       p->sy < py + attr.height;
    }
    if (outline) {
        XUnmapWindow(p->d, p->selection);
    }
    if (hide_preview) {
        XUnmapWindow(p->d, p->preview);
    }
    if (outline || hide_preview) {
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
    if (hide_preview) {
        XMapWindow(p->d, p->preview);
    }
    if (!ok) {
        if (image && image != p->image) {
            XDestroyImage(image);
        }
        return fail(e, n,
                    "Xorg source acquisition failed (window/monitor changed); no alternate source "
                    "was selected");
    }
    for (int j = 0; j < out->height; j++) {
        uint8_t *d = out->data + (size_t)j * out->stride;
        for (int i = 0; i < out->width; i++, d += 4) {
            unsigned long value;
            if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst) {
                uint32_t v;
                memcpy(&v, image->data + (size_t)j * image->bytes_per_line + i * 4, 4);
                value = v;
            } else {
                value = XGetPixel(image, i, j);
            }
            d[0] = (uint8_t)channel(value, image->red_mask);
            d[1] = (uint8_t)channel(value, image->green_mask);
            d[2] = (uint8_t)channel(value, image->blue_mask);
            d[3] = 255;
        }
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
                snprintf(row, sizeof(row), "%s%s %dx%d%+d%+d%s\n",
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
                snprintf(p->source_error, sizeof(p->source_error), "window selection cancelled: %s",
                         error);
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
            snprintf(p->source_error, sizeof(p->source_error), "region selection cancelled: %s",
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
    if (sym >= XK_a && sym <= XK_z) {
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
        compositor_clear(comp);
        p->last_keycode = 0;
        p->seen_generation = p->generation;
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
                XkbRefreshKeyboardMapping(&k->map);
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
        if (p->selecting) {
            selection_event(p, &ev);
        }
        if (ev.type == ClientMessage && ev.xclient.window == p->preview &&
            (Atom)ev.xclient.data.l[0] == p->wm_delete) {
            XUnmapWindow(p->d, p->preview);
            p->preview_disabled = true;
        }
        if (ev.type == DestroyNotify && ev.xdestroywindow.window == p->preview) {
            p->preview = 0;
            if (p->preview_gc) {
                XFreeGC(p->d, p->preview_gc);
            }
            p->preview_gc = 0;
            p->preview_disabled = true;
        }
        if (ev.type == ConfigureNotify && ev.xconfigure.window == p->preview) {
            p->preview_w = ev.xconfigure.width;
            p->preview_h = ev.xconfigure.height;
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
    int x = 0, y = 0;
    Monitor list[MONITOR_MAX];
    int count = monitors(p, list, MONITOR_MAX);
    /* Prefer a monitor outside the root-captured rectangle. Otherwise unmap around readback. */
    for (int i = 0; i < count; i++) {
        if (!rectangles_overlap(list[i].x, list[i].y, list[i].w, list[i].h, p->sx, p->sy, p->sw,
                                p->sh)) {
            x = list[i].x + 24;
            y = list[i].y + 24;
            break;
        }
    }
    p->preview = XCreateSimpleWindow(p->d, p->root, x, y, (unsigned)w, (unsigned)h, 0, 0, 0x111111);
    p->preview_w = w;
    p->preview_h = h;
    XStoreName(p->d, p->preview, "cast output preview");
    XSetWMProtocols(p->d, p->preview, &p->wm_delete, 1);
    XSelectInput(p->d, p->preview, ExposureMask | StructureNotifyMask);
    p->preview_gc = XCreateGC(p->d, p->preview, 0, NULL);
    XMapWindow(p->d, p->preview);
}
int x11_preview(Platform *platform, const Frame *frame, const State *state, const Config *cfg,
                char *e, size_t n)
{
    Xorg *p = (Xorg *)platform;
    if (!cfg->preview) {
        if (p->preview) {
            XUnmapWindow(p->d, p->preview);
        }
        p->preview_disabled = false;
        return 0;
    }
    if (p->preview_disabled) {
        return 0;
    }
    uint64_t now = cast_now_ns();
    if (p->last_preview && now - p->last_preview < 66666666) {
        return 0;
    }
    p->last_preview = now;
    if (!frame || !frame->data) {
        return 0;
    }
    if (!p->preview) {
        preview_create(p, cfg);
    } else {
        XMapWindow(p->d, p->preview);
    }
    int w = p->preview_w, h = p->preview_h - 32;
    if (w < 1 || h < 1 || w > 4096 || h > 4096) {
        return fail(e, n, "preview window size is unsupported; resize it below 4096 pixels");
    }
    if (!p->preview_image || p->preview_image->width != w || p->preview_image->height != h) {
        if (p->preview_image) {
            XDestroyImage(p->preview_image);
        }
        p->preview_image = XCreateImage(p->d, DefaultVisual(p->d, p->screen),
                                        (unsigned)DefaultDepth(p->d, p->screen), ZPixmap, 0, NULL,
                                        (unsigned)w, (unsigned)h, 32, 0);
        if (!p->preview_image) {
            return fail(e, n, "cannot allocate preview XImage");
        }
        p->preview_image->data = calloc((size_t)p->preview_image->bytes_per_line, h);
        if (!p->preview_image->data) {
            XDestroyImage(p->preview_image);
            p->preview_image = NULL;
            return fail(e, n, "cannot allocate preview image pixels");
        }
    }
    XImage *im = p->preview_image;
    double scale = (double)w / frame->width;
    if ((double)h / frame->height < scale) {
        scale = (double)h / frame->height;
    }
    int iw = (int)(frame->width * scale), ih = (int)(frame->height * scale), ox = (w - iw) / 2,
        oy = (h - ih) / 2;
    memset(im->data, 0, (size_t)im->bytes_per_line * h);
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
    begin(p);
    XPutImage(p->d, p->preview, p->preview_gc, im, 0, 0, 0, 32, (unsigned)w, (unsigned)h);
    XSetForeground(p->d, p->preview_gc, 0x111111);
    XFillRectangle(p->d, p->preview, p->preview_gc, 0, 0, (unsigned)w, 32);
    XSetForeground(p->d, p->preview_gc,
                   state->live_paused   ? 0xffc65c
                   : state->live_frozen ? 0x7ec8ff
                                        : 0x64e69f);
    const char *live = state->live_paused ? "PAUSED" : state->live_frozen ? "FROZEN" : "LIVE";
    char label[256];
    snprintf(label, sizeof(label), "%s | %s | preview: %s", live,
             state->recording ? (state->record_paused ? "RECORDING-PAUSED" : "RECORDING")
                              : "RECORD OFF",
             cfg->preview_target);
    XDrawString(p->d, p->preview, p->preview_gc, 10, 21, label, (int)strlen(label));
    if (!end(p)) {
        return fail(e, n, "preview window disappeared; toggle preview off/on to recreate it");
    }
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
             "Preview uses out-of-source placement or explicit unmap during root readback.",
             source, shm ? "available" : "missing",
             xi ? "available" : "missing: keys/clicks unsupported",
             comp ? "available" : "missing: window capture unsupported");
    XCloseDisplay(d);
}
