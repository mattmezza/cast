#include "panel_color_pick.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WITH_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#endif
#ifdef WITH_WAYLAND
#include <gio/gio.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define REQUEST "org.freedesktop.portal.Request"

/* Async callbacks own references to an operation, never to the panel. This
 * permits cancel/free while a bus connection or method reply is outstanding. */
typedef struct {
    unsigned refs;
    PanelColorPickStatus status;
    uint8_t rgb[3];
    char error[256];
    char *parent, *request;
    GDBusConnection *bus;
    GCancellable *cancel;
    guint response_sub, owner_sub;
} PortalPick;
#endif

struct PanelColorPick {
    PanelColorPickStatus status;
    uint8_t rgb[3];
    char error[256];
#ifdef WITH_X11
    Display *display;
    Cursor cursor;
    bool pointer_grabbed, keyboard_grabbed, x11_dead, x11_selected;
#endif
#ifdef WITH_WAYLAND
    PortalPick *portal;
#endif
};

static int fail(PanelColorPick *pick, const char *message)
{
    snprintf(pick->error, sizeof pick->error, "%s", message);
    pick->status = PANEL_COLOR_PICK_ERROR;
    return -1;
}

#ifdef WITH_X11
/* Xlib handlers are process-wide. Install them only around this connection's
 * calls and forward errors belonging to SDL or another Xlib client. */
static PanelColorPick *x11_guard;
static XErrorHandler previous_error;
static XIOErrorHandler previous_io_error;
static int x11_error_code;

static int x11_error(Display *display, XErrorEvent *event)
{
    if (x11_guard && display == x11_guard->display) {
        x11_error_code = event->error_code;
        return 0;
    }
    return previous_error ? previous_error(display, event) : 0;
}

static int x11_io_error(Display *display)
{
    if (x11_guard && display == x11_guard->display) {
        x11_guard->x11_dead = true;
        return 0;
    }
    return previous_io_error ? previous_io_error(display) : 0;
}

static void x11_io_exit(Display *display, void *data)
{
    (void)display;
    ((PanelColorPick *)data)->x11_dead = true;
}

static void x11_enter(PanelColorPick *pick)
{
    x11_guard = pick;
    x11_error_code = 0;
    previous_error = XSetErrorHandler(x11_error);
    previous_io_error = XSetIOErrorHandler(x11_io_error);
}

static void x11_leave(void)
{
    XSetErrorHandler(previous_error);
    XSetIOErrorHandler(previous_io_error);
    x11_guard = NULL;
}

static void x11_close(PanelColorPick *pick)
{
    if (!pick->display) {
        return;
    }
    x11_enter(pick);
    if (!pick->x11_dead) {
        if (pick->pointer_grabbed) {
            XUngrabPointer(pick->display, CurrentTime);
        }
        if (pick->keyboard_grabbed) {
            XUngrabKeyboard(pick->display, CurrentTime);
        }
        if (pick->cursor) {
            XFreeCursor(pick->display, pick->cursor);
        }
    }
    /* Closing the connection also releases every grab after a partial start. */
    XCloseDisplay(pick->display);
    x11_leave();
    pick->display = NULL;
    pick->cursor = None;
    pick->pointer_grabbed = pick->keyboard_grabbed = false;
    pick->x11_selected = false;
}

static int x11_begin(PanelColorPick *pick)
{
    pick->display = XOpenDisplay(NULL);
    if (!pick->display) {
        return fail(pick, "Cannot connect to the X11 display for screen color selection.");
    }
    pick->x11_dead = false;
    XSetIOErrorExitHandler(pick->display, x11_io_exit, pick);
    x11_enter(pick);
    Window root = DefaultRootWindow(pick->display);
    pick->cursor = XCreateFontCursor(pick->display, XC_crosshair);
    int pointer = XGrabPointer(pick->display, root, False,
                               ButtonPressMask | ButtonReleaseMask,
                               GrabModeAsync, GrabModeAsync, None, pick->cursor, CurrentTime);
    pick->pointer_grabbed = pointer == GrabSuccess;
    int keyboard = pick->pointer_grabbed
                       ? XGrabKeyboard(pick->display, root, False, GrabModeAsync,
                                       GrabModeAsync, CurrentTime)
                       : AlreadyGrabbed;
    pick->keyboard_grabbed = keyboard == GrabSuccess;
    XFlush(pick->display);
    bool success = pick->pointer_grabbed && pick->keyboard_grabbed && !pick->x11_dead;
    x11_leave();
    if (!success) {
        x11_close(pick);
        return fail(pick, "Cannot select a screen color while another application holds the pointer or keyboard.");
    }
    return 0;
}

static uint8_t x11_component(unsigned long pixel, unsigned long mask)
{
    if (!mask) {
        return 0;
    }
    while (!(mask & 1)) {
        mask >>= 1;
        pixel >>= 1;
    }
    return (uint8_t)lround((double)(pixel & mask) * 255.0 / (double)mask);
}

static void x11_poll(PanelColorPick *pick)
{
    x11_enter(pick);
    /* Check queued events without blocking; a click performs one pixel read. */
    for (int count = 0; count < 32 && !pick->x11_dead && XPending(pick->display); count++) {
        if (pick->x11_dead) {
            break;
        }
        XEvent event;
        XNextEvent(pick->display, &event);
        if ((event.type == KeyPress && XLookupKeysym(&event.xkey, 0) == XK_Escape) ||
            (event.type == ButtonPress && event.xbutton.button == Button3)) {
            pick->status = PANEL_COLOR_PICK_CANCELLED;
            break;
        }
        if (event.type == ButtonRelease && event.xbutton.button == Button1 && pick->x11_selected) {
            pick->status = PANEL_COLOR_PICK_SUCCESS;
            break;
        }
        if (event.type != ButtonPress || event.xbutton.button != Button1 || pick->x11_selected) {
            continue;
        }
        XImage *image = XGetImage(pick->display, event.xbutton.root,
                                 event.xbutton.x_root, event.xbutton.y_root,
                                 1, 1, AllPlanes, ZPixmap);
        if (image && !pick->x11_dead && !x11_error_code) {
            unsigned long pixel = XGetPixel(image, 0, 0);
            if (image->red_mask && image->green_mask && image->blue_mask) {
                pick->rgb[0] = x11_component(pixel, image->red_mask);
                pick->rgb[1] = x11_component(pixel, image->green_mask);
                pick->rgb[2] = x11_component(pixel, image->blue_mask);
            } else {
                XColor color = {.pixel = pixel};
                XQueryColor(pick->display, DefaultColormap(pick->display, DefaultScreen(pick->display)), &color);
                pick->rgb[0] = (uint8_t)((color.red + 128u) / 257u);
                pick->rgb[1] = (uint8_t)((color.green + 128u) / 257u);
                pick->rgb[2] = (uint8_t)((color.blue + 128u) / 257u);
            }
            pick->x11_selected = true;
        } else {
            fail(pick, "Cannot read that screen pixel. Try selecting another point.");
        }
        if (image) {
            XDestroyImage(image);
        }
        break;
    }
    if (pick->x11_dead) {
        fail(pick, "The X11 display disconnected during screen color selection.");
    }
    x11_leave();
    if (pick->status != PANEL_COLOR_PICK_PENDING) {
        x11_close(pick);
    }
}
#endif

#ifdef WITH_WAYLAND
static PortalPick *portal_ref(PortalPick *portal)
{
    portal->refs++;
    return portal;
}

static void portal_unref(gpointer data)
{
    PortalPick *portal = data;
    if (--portal->refs) {
        return;
    }
    g_clear_object(&portal->bus);
    g_clear_object(&portal->cancel);
    g_free(portal->parent);
    g_free(portal->request);
    free(portal);
}

static void portal_unsubscribe(PortalPick *portal)
{
    if (portal->response_sub) {
        g_dbus_connection_signal_unsubscribe(portal->bus, portal->response_sub);
        portal->response_sub = 0;
    }
    if (portal->owner_sub) {
        g_dbus_connection_signal_unsubscribe(portal->bus, portal->owner_sub);
        portal->owner_sub = 0;
    }
}

static void portal_close(PortalPick *portal)
{
    if (portal->bus && portal->request && !g_dbus_connection_is_closed(portal->bus)) {
        g_dbus_connection_call(portal->bus, PORTAL, portal->request, REQUEST, "Close",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
    }
}

static void portal_cancel(PortalPick *portal)
{
    portal->status = PANEL_COLOR_PICK_CANCELLED;
    portal_close(portal);
    portal_unsubscribe(portal);
    g_cancellable_cancel(portal->cancel);
}

static void portal_fail(PortalPick *portal, const char *message)
{
    snprintf(portal->error, sizeof portal->error, "Screen color portal: %s", message);
    portal->status = PANEL_COLOR_PICK_ERROR;
    portal_close(portal);
    portal_unsubscribe(portal);
}

static void portal_response(GDBusConnection *bus, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal, GVariant *parameters,
                            gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    PortalPick *portal = data;
    if (portal->status != PANEL_COLOR_PICK_PENDING) {
        return;
    }
    guint code;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &code, &results);
    if (code == 1) {
        portal->status = PANEL_COLOR_PICK_CANCELLED;
    } else if (code != 0) {
        portal_fail(portal, "the desktop could not select a color.");
    } else {
        double r, g, b;
        if (!g_variant_lookup(results, "color", "(ddd)", &r, &g, &b) ||
            !isfinite(r) || !isfinite(g) || !isfinite(b) ||
            r < 0 || r > 1 || g < 0 || g > 1 || b < 0 || b > 1) {
            portal_fail(portal, "the desktop returned an invalid color.");
        } else {
            portal->rgb[0] = (uint8_t)lround(r * 255);
            portal->rgb[1] = (uint8_t)lround(g * 255);
            portal->rgb[2] = (uint8_t)lround(b * 255);
            portal->status = PANEL_COLOR_PICK_SUCCESS;
        }
    }
    g_variant_unref(results);
    portal_unsubscribe(portal);
}

static void portal_subscribe(PortalPick *portal)
{
    portal->response_sub = g_dbus_connection_signal_subscribe(
        portal->bus, PORTAL, REQUEST, "Response", portal->request, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, portal_response, portal_ref(portal), portal_unref);
}

static void portal_owner_changed(GDBusConnection *bus, const gchar *sender, const gchar *path,
                                 const gchar *interface, const gchar *signal, GVariant *parameters,
                                 gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    PortalPick *portal = data;
    const char *name, *old_owner, *new_owner;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    (void)name;
    (void)old_owner;
    if (!new_owner[0] && portal->status == PANEL_COLOR_PICK_PENDING) {
        portal_fail(portal, "the desktop color portal disconnected.");
    }
}

static void portal_called(GObject *object, GAsyncResult *result, gpointer data)
{
    PortalPick *portal = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
    if (reply) {
        const char *path;
        g_variant_get(reply, "(&o)", &path);
        if (strcmp(path, portal->request)) {
            portal_unsubscribe(portal);
            g_free(portal->request);
            portal->request = g_strdup(path);
            if (portal->status == PANEL_COLOR_PICK_PENDING) {
                portal_subscribe(portal);
            }
        }
        if (portal->status == PANEL_COLOR_PICK_CANCELLED) {
            portal_close(portal);
        }
        g_variant_unref(reply);
    } else if (portal->status == PANEL_COLOR_PICK_PENDING) {
        portal_fail(portal, error ? error->message : "color selection is unavailable on this desktop.");
    }
    g_clear_error(&error);
    portal_unref(portal);
}

static void portal_connected(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    PortalPick *portal = data;
    GError *error = NULL;
    portal->bus = g_bus_get_finish(result, &error);
    if (portal->status != PANEL_COLOR_PICK_PENDING) {
        /* Cancellation before the connection completes never launches a picker. */
    } else if (!portal->bus) {
        portal_fail(portal, error ? error->message : "cannot connect to the session bus.");
    } else {
        char *sender = g_strdup(g_dbus_connection_get_unique_name(portal->bus) + 1);
        for (char *c = sender; *c; c++) {
            if (*c == '.') {
                *c = '_';
            }
        }
        char token[80];
        snprintf(token, sizeof token, "cast_color_%08x_%08x", g_random_int(), g_random_int());
        portal->request = g_strdup_printf(PORTAL_PATH "/request/%s/%s", sender, token);
        g_free(sender);
        portal_subscribe(portal);
        portal->owner_sub = g_dbus_connection_signal_subscribe(
            portal->bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
            "/org/freedesktop/DBus", PORTAL, G_DBUS_SIGNAL_FLAGS_NONE,
            portal_owner_changed, portal_ref(portal), portal_unref);
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
        g_dbus_connection_call(portal->bus, PORTAL, PORTAL_PATH,
                               "org.freedesktop.portal.Screenshot", "PickColor",
                               g_variant_new("(sa{sv})", portal->parent, &options),
                               G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 10000,
                               portal->cancel, portal_called, portal_ref(portal));
    }
    g_clear_error(&error);
    portal_unref(portal);
}
#endif

PanelColorPick *panel_color_pick_new(void)
{
    return calloc(1, sizeof(PanelColorPick));
}

void panel_color_pick_cancel(PanelColorPick *pick)
{
    if (!pick) {
        return;
    }
#ifdef WITH_X11
    x11_close(pick);
#endif
#ifdef WITH_WAYLAND
    if (pick->portal) {
        portal_cancel(pick->portal);
        portal_unref(pick->portal);
        pick->portal = NULL;
    }
#endif
    pick->status = PANEL_COLOR_PICK_IDLE;
}

void panel_color_pick_free(PanelColorPick *pick)
{
    panel_color_pick_cancel(pick);
    free(pick);
}

int panel_color_pick_begin(PanelColorPick *pick, const char *driver, const char *parent_window)
{
    if (!pick) {
        return -1;
    }
    panel_color_pick_cancel(pick);
    pick->error[0] = '\0';
    pick->status = PANEL_COLOR_PICK_PENDING;
    if (driver && !strcmp(driver, "x11")) {
#ifdef WITH_X11
        (void)parent_window;
        return x11_begin(pick);
#else
        return fail(pick, "Screen color selection requires an X11-enabled build on this display.");
#endif
    }
    if (driver && !strcmp(driver, "wayland")) {
#ifdef WITH_WAYLAND
        PortalPick *portal = calloc(1, sizeof *portal);
        if (!portal) {
            return fail(pick, "Cannot allocate screen color selection.");
        }
        portal->refs = 1;
        portal->status = PANEL_COLOR_PICK_PENDING;
        portal->parent = g_strdup(parent_window ? parent_window : "");
        portal->cancel = g_cancellable_new();
        pick->portal = portal;
        g_bus_get(G_BUS_TYPE_SESSION, portal->cancel, portal_connected, portal_ref(portal));
        return 0;
#else
        return fail(pick, "Screen color selection on Wayland requires a Wayland-enabled build and a desktop color portal.");
#endif
    }
    (void)parent_window;
    return fail(pick, "Screen color selection is unavailable on this display driver.");
}

PanelColorPickStatus panel_color_pick_poll(PanelColorPick *pick, uint8_t rgb[3])
{
    if (!pick) {
        return PANEL_COLOR_PICK_IDLE;
    }
#ifdef WITH_X11
    if (pick->display && pick->status == PANEL_COLOR_PICK_PENDING) {
        x11_poll(pick);
    }
#endif
#ifdef WITH_WAYLAND
    /* GLib async replies run on the panel thread. Bound work per SDL frame. */
    for (int i = 0; i < 16 && g_main_context_pending(NULL); i++) {
        g_main_context_iteration(NULL, FALSE);
    }
    if (pick->portal) {
        PortalPick *portal = pick->portal;
        if (portal->status == PANEL_COLOR_PICK_PENDING && portal->bus &&
            g_dbus_connection_is_closed(portal->bus)) {
            portal_fail(portal, "the session bus disconnected.");
        }
        pick->status = portal->status;
        memcpy(pick->rgb, portal->rgb, sizeof pick->rgb);
        snprintf(pick->error, sizeof pick->error, "%s", portal->error);
    }
#endif
    if (rgb && pick->status == PANEL_COLOR_PICK_SUCCESS) {
        memcpy(rgb, pick->rgb, sizeof pick->rgb);
    }
    return pick->status;
}

const char *panel_color_pick_error(const PanelColorPick *pick)
{
    return pick ? pick->error : "Cannot allocate screen color selection.";
}
