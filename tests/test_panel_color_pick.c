#include "panel_color_pick.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef WITH_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void xdo(const char *verb, const char *first, const char *second)
{
    char *arguments[] = {"xdotool", (char *)verb, (char *)first, (char *)second, NULL};
    pid_t child;
    assert(!posix_spawnp(&child, arguments[0], NULL, NULL, arguments, environ));
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static unsigned long fixture_color(Display *display, unsigned r, unsigned g, unsigned b)
{
    XColor color = {.red = r * 257, .green = g * 257, .blue = b * 257};
    assert(XAllocColor(display, DefaultColormap(display, DefaultScreen(display)), &color));
    return color.pixel;
}

static Window find_swatch(Display *display)
{
    Window root, parent, *children;
    unsigned count;
    assert(XQueryTree(display, DefaultRootWindow(display), &root, &parent, &children, &count));
    Window result = None;
    Atom marker = XInternAtom(display, "_CAST_COLOR_PICKER", False);
    for (unsigned i = 0; i < count; i++) {
        Atom type;
        int format;
        unsigned long length, remaining;
        unsigned char *data = NULL;
        assert(XGetWindowProperty(display, children[i], marker, 0, 1, False, XA_CARDINAL,
                                  &type, &format, &length, &remaining, &data) == Success);
        if (type == XA_CARDINAL && format == 32 && length == 1 && *(unsigned long *)data == 1) {
            assert(result == None);
            result = children[i];
        }
        if (data) {
            XFree(data);
        }
    }
    XFree(children);
    return result;
}

static void assert_swatch(Display *display, Window swatch, int pointer_x, int pointer_y,
                           unsigned r, unsigned g, unsigned b)
{
    XWindowAttributes attributes;
    assert(XGetWindowAttributes(display, swatch, &attributes));
    assert(attributes.override_redirect && attributes.map_state == IsViewable);
    assert(attributes.width == attributes.height && attributes.width <= 32);
    assert(pointer_x < attributes.x || pointer_x >= attributes.x + attributes.width ||
           pointer_y < attributes.y || pointer_y >= attributes.y + attributes.height);
    XWMHints *hints = XGetWMHints(display, swatch);
    assert(hints && hints->flags & InputHint && !hints->input);
    XFree(hints);
    int shape_count, ordering;
    XRectangle *shape = XShapeGetRectangles(display, swatch, ShapeInput, &shape_count, &ordering);
    assert(shape_count == 0);
    XFree(shape);
    XImage *image = XGetImage(display, swatch, attributes.width / 2, attributes.height / 2,
                             1, 1, AllPlanes, ZPixmap);
    assert(image);
    XColor color = {.pixel = XGetPixel(image, 0, 0)};
    XQueryColor(display, attributes.colormap, &color);
    if ((color.red + 128u) / 257u != r || (color.green + 128u) / 257u != g ||
        (color.blue + 128u) / 257u != b) {
        fprintf(stderr, "swatch at pointer %d,%d: expected %u,%u,%u, got %u,%u,%u\n",
            pointer_x, pointer_y, r, g, b, (color.red + 128u) / 257u,
            (color.green + 128u) / 257u, (color.blue + 128u) / 257u);
    }
    assert((color.red + 128u) / 257u == r && (color.green + 128u) / 257u == g &&
           (color.blue + 128u) / 257u == b);
    XDestroyImage(image);
}

static void x11_tick(PanelColorPick *pick, Display *display)
{
    for (int i = 0; i < 4; i++) {
        assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_PENDING);
        XSync(display, False);
        usleep(1000);
    }
}

static void test_x11_swatch(void)
{
    /* Opt in only on a private Xvfb: never paint or warp the user's desktop. */
    if (!getenv("CAST_PANEL_COLOR_PICK_TEST_X11")) {
        return;
    }
    Display *display = XOpenDisplay(NULL);
    assert(display);
    Window root = DefaultRootWindow(display);
    int width = DisplayWidth(display, DefaultScreen(display));
    int height = DisplayHeight(display, DefaultScreen(display));
    assert(width >= 400 && height >= 200);
    XSetWindowAttributes attributes = {.override_redirect = True, .backing_store = Always};
    Window fixture = XCreateWindow(display, root, 0, 0, width, height, 0, CopyFromParent,
        InputOutput, CopyFromParent, CWOverrideRedirect | CWBackingStore, &attributes);
    GC gc = XCreateGC(display, fixture, 0, NULL);
    XMapRaised(display, fixture);
    XSetForeground(display, gc, fixture_color(display, 17, 34, 51));
    XFillRectangle(display, fixture, gc, 0, 0, width, height);
    XSetForeground(display, gc, fixture_color(display, 170, 187, 204));
    XFillRectangle(display, fixture, gc, 200, 0, width - 200, height);
    XSync(display, False);
    xdo("mousemove", "100", "100");
    PanelColorPick *pick = panel_color_pick_new();
    assert(pick && !panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    Window swatch = find_swatch(display);
    assert(swatch);
    assert_swatch(display, swatch, 100, 100, 17, 34, 51);
    /* The old square's white corner must be replaced by the underlying color. */
    XWindowAttributes old;
    assert(XGetWindowAttributes(display, swatch, &old));
    char x[32], y[32];
    snprintf(x, sizeof x, "%d", old.x);
    snprintf(y, sizeof y, "%d", old.y);
    xdo("mousemove", x, y);
    x11_tick(pick, display);
    assert_swatch(display, swatch, old.x, old.y, 17, 34, 51);
    xdo("mousemove", "300", "100");
    x11_tick(pick, display);
    assert_swatch(display, swatch, 300, 100, 170, 187, 204);
    /* A stationary pointer still previews changing desktop content. */
    XSetForeground(display, gc, fixture_color(display, 68, 187, 119));
    XFillRectangle(display, fixture, gc, 295, 95, 10, 10);
    XSync(display, False);
    x11_tick(pick, display);
    assert_swatch(display, swatch, 300, 100, 68, 187, 119);
    xdo("click", "1", NULL);
    uint8_t rgb[3];
    for (int i = 0; i < 100 && panel_color_pick_poll(pick, rgb) == PANEL_COLOR_PICK_PENDING; i++) {
        usleep(1000);
    }
    assert(panel_color_pick_poll(pick, rgb) == PANEL_COLOR_PICK_SUCCESS);
    assert(rgb[0] == 68 && rgb[1] == 187 && rgb[2] == 119);
    assert(!find_swatch(display));
    /* A click that jumps into the old square reads its underlying desktop,
     * even when no hover tick occurred between that move and the click. */
    assert(!panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    assert(XGetWindowAttributes(display, find_swatch(display), &old));
    snprintf(x, sizeof x, "%d", old.x + old.width / 2);
    snprintf(y, sizeof y, "%d", old.y + old.height / 2);
    xdo("mousemove", x, y);
    xdo("click", "1", NULL);
    for (int i = 0; i < 100 && panel_color_pick_poll(pick, rgb) == PANEL_COLOR_PICK_PENDING; i++) {
        usleep(1000);
    }
    assert(panel_color_pick_poll(pick, rgb) == PANEL_COLOR_PICK_SUCCESS);
    assert(rgb[0] == 170 && rgb[1] == 187 && rgb[2] == 204);
    assert(!find_swatch(display));
    assert(!panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    xdo("key", "Escape", NULL);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_CANCELLED);
    assert(!find_swatch(display));
    assert(!panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    xdo("click", "3", NULL);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_CANCELLED);
    assert(!find_swatch(display));
    /* Near screen edges the preview flips beside, never beneath, the pointer. */
    snprintf(x, sizeof x, "%d", width - 1);
    snprintf(y, sizeof y, "%d", height - 1);
    xdo("mousemove", x, y);
    assert(!panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    assert_swatch(display, find_swatch(display), width - 1, height - 1, 170, 187, 204);
    panel_color_pick_cancel(pick);
    assert(!find_swatch(display));
    assert(XGrabPointer(display, root, False, ButtonPressMask, GrabModeAsync, GrabModeAsync,
                         None, None, CurrentTime) == GrabSuccess);
    assert(XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess);
    XUngrabPointer(display, CurrentTime);
    XUngrabKeyboard(display, CurrentTime);
    XSync(display, False);
    assert(!panel_color_pick_begin(pick, "x11", NULL));
    x11_tick(pick, display);
    panel_color_pick_free(pick);
    assert(!find_swatch(display));
    XFreeGC(display, gc);
    XDestroyWindow(display, fixture);
    XCloseDisplay(display);
}
#endif

#ifdef WITH_WAYLAND
#include <gio/gio.h>

typedef enum { SUCCESS, EARLY_SUCCESS, CANCELLED, FAILED, INVALID, UNSUPPORTED, DEFERRED } Mode;
typedef struct {
    GDBusConnection *bus;
    Mode mode;
    char *request;
    unsigned calls, closes;
    guint registration;
} MockPortal;

static const char interfaces[] =
    "<node><interface name='org.freedesktop.portal.Screenshot'>"
    "<method name='PickColor'><arg type='s' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "</interface><interface name='org.freedesktop.portal.Request'>"
    "<method name='Close'/><signal name='Response'><arg type='u'/><arg type='a{sv}'/>"
    "</signal></interface></node>";

static GDBusNodeInfo *node;

static void emit_response(MockPortal *mock)
{
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    if (mock->mode == SUCCESS || mock->mode == EARLY_SUCCESS || mock->mode == INVALID) {
        g_variant_builder_add(&results, "{sv}", "color",
                              g_variant_new("(ddd)", mock->mode == INVALID ? 2.0 : 0.2, 0.4, 0.6));
    }
    guint code = mock->mode == CANCELLED ? 1 : mock->mode == FAILED ? 2 : 0;
    GError *error = NULL;
    assert(g_dbus_connection_emit_signal(mock->bus, NULL, mock->request,
                                         "org.freedesktop.portal.Request", "Response",
                                         g_variant_new("(ua{sv})", code, &results), &error));
    assert(!error);
}

static gboolean delayed_reply(gpointer data)
{
    GDBusMethodInvocation *invocation = data;
    const char *request = g_object_get_data(G_OBJECT(invocation), "request");
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request));
    g_object_unref(invocation);
    return G_SOURCE_REMOVE;
}

static void method_call(GDBusConnection *bus, const gchar *sender, const gchar *path,
                         const gchar *interface, const gchar *method, GVariant *parameters,
                         GDBusMethodInvocation *invocation, gpointer data);

static const GDBusInterfaceVTable vtable = {.method_call = method_call};

static void method_call(GDBusConnection *bus, const gchar *sender, const gchar *path,
                         const gchar *interface, const gchar *method, GVariant *parameters,
                         GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus;
    (void)path;
    (void)interface;
    MockPortal *mock = data;
    if (!strcmp(method, "Close")) {
        mock->closes++;
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    assert(!strcmp(method, "PickColor"));
    mock->calls++;
    if (mock->mode == UNSUPPORTED) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.DBus.Error.UnknownMethod", "PickColor is unavailable");
        return;
    }
    const char *parent;
    GVariant *options;
    g_variant_get(parameters, "(&s@a{sv})", &parent, &options);
    assert(!parent[0]);
    const char *token;
    assert(g_variant_lookup(options, "handle_token", "&s", &token));
    char *escaped_sender = g_strdup(sender + 1);
    for (char *c = escaped_sender; *c; c++) {
        if (*c == '.') {
            *c = '_';
        }
    }
    if (mock->registration) {
        g_dbus_connection_unregister_object(mock->bus, mock->registration);
    }
    g_free(mock->request);
    mock->request = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", escaped_sender, token);
    g_free(escaped_sender);
    g_variant_unref(options);
    GError *error = NULL;
    mock->registration = g_dbus_connection_register_object(mock->bus, mock->request,
        node->interfaces[1], &vtable, mock, NULL, &error);
    assert(mock->registration && !error);
    if (mock->mode == EARLY_SUCCESS) {
        emit_response(mock);
        g_object_set_data_full(G_OBJECT(invocation), "request", g_strdup(mock->request), g_free);
        g_timeout_add(30, delayed_reply, g_object_ref(invocation));
    } else {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", mock->request));
        if (mock->mode != DEFERRED) {
            emit_response(mock);
        }
    }
}

static void pump(PanelColorPick *pick, unsigned milliseconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do {
        panel_color_pick_poll(pick, NULL);
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

static PanelColorPickStatus await_result(PanelColorPick *pick, uint8_t rgb[3])
{
    gint64 end = g_get_monotonic_time() + 3000000;
    PanelColorPickStatus status;
    do {
        status = panel_color_pick_poll(pick, rgb);
        if (status != PANEL_COLOR_PICK_PENDING) {
            return status;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
    assert(!"color picker timed out");
    return status;
}

static void test_portal(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);
    GError *error = NULL;
    MockPortal mock = {0};
    mock.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    assert(mock.bus && !error);
    node = g_dbus_node_info_new_for_xml(interfaces, &error);
    assert(node && !error);
    guint screenshot = g_dbus_connection_register_object(mock.bus, "/org/freedesktop/portal/desktop",
        node->interfaces[0], &vtable, &mock, NULL, &error);
    assert(screenshot && !error);
    GVariant *name = g_dbus_connection_call_sync(mock.bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    assert(name && !error);
    g_variant_unref(name);
    PanelColorPick *pick = panel_color_pick_new();
    assert(pick);
    const Mode modes[] = {SUCCESS, EARLY_SUCCESS, CANCELLED, FAILED, INVALID, UNSUPPORTED};
    const PanelColorPickStatus expected[] = {PANEL_COLOR_PICK_SUCCESS, PANEL_COLOR_PICK_SUCCESS,
        PANEL_COLOR_PICK_CANCELLED, PANEL_COLOR_PICK_ERROR, PANEL_COLOR_PICK_ERROR, PANEL_COLOR_PICK_ERROR};
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
        mock.mode = modes[i];
        assert(!panel_color_pick_begin(pick, "wayland", NULL));
        uint8_t rgb[3] = {7, 8, 9};
        assert(await_result(pick, rgb) == expected[i]);
        if (expected[i] == PANEL_COLOR_PICK_SUCCESS) {
            assert(rgb[0] == 51 && rgb[1] == 102 && rgb[2] == 153);
        } else {
            assert(rgb[0] == 7 && rgb[1] == 8 && rgb[2] == 9);
        }
        if (expected[i] == PANEL_COLOR_PICK_ERROR) {
            assert(panel_color_pick_error(pick)[0]);
        }
        pump(pick, 60);
        assert(panel_color_pick_poll(pick, NULL) == expected[i]);
    }
    mock.mode = DEFERRED;
    unsigned calls = mock.calls;
    assert(!panel_color_pick_begin(pick, "wayland", NULL));
    pump(pick, 60);
    assert(mock.calls == calls + 1);
    unsigned closes = mock.closes;
    panel_color_pick_cancel(pick);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_IDLE);
    pump(pick, 60);
    assert(mock.closes == closes + 1);
    /* A late response after cancellation must not replace a new operation. */
    emit_response(&mock);
    mock.mode = SUCCESS;
    assert(!panel_color_pick_begin(pick, "wayland", ""));
    uint8_t rgb[3];
    assert(await_result(pick, rgb) == PANEL_COLOR_PICK_SUCCESS);
    assert(rgb[0] == 51 && rgb[1] == 102 && rgb[2] == 153);
    panel_color_pick_free(pick);
    /* Cancellation before the async bus reply must not launch a portal. */
    calls = mock.calls;
    for (int i = 0; i < 10; i++) {
        pick = panel_color_pick_new();
        assert(!panel_color_pick_begin(pick, "wayland", NULL));
        panel_color_pick_free(pick);
    }
    pump(NULL, 80);
    /* poll(NULL) has no context to service; explicitly dispatch late callbacks. */
    for (int i = 0; i < 64 && g_main_context_pending(NULL); i++) {
        g_main_context_iteration(NULL, FALSE);
    }
    assert(mock.calls == calls);
    if (mock.registration) {
        g_dbus_connection_unregister_object(mock.bus, mock.registration);
    }
    g_dbus_connection_unregister_object(mock.bus, screenshot);
    g_free(mock.request);
    g_dbus_node_info_unref(node);
    g_dbus_connection_close_sync(mock.bus, NULL, NULL);
    g_object_unref(mock.bus);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}
#endif

int main(void)
{
    PanelColorPick *pick = panel_color_pick_new();
    assert(pick);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_IDLE);
    assert(panel_color_pick_begin(pick, "dummy", NULL) == -1);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_ERROR);
    assert(panel_color_pick_error(pick)[0]);
    panel_color_pick_cancel(pick);
    assert(panel_color_pick_poll(pick, NULL) == PANEL_COLOR_PICK_IDLE);
    panel_color_pick_free(pick);
#ifdef WITH_X11
    test_x11_swatch();
#endif
#ifdef WITH_WAYLAND
    test_portal();
#endif
    puts("panel color picker tests passed");
    return 0;
}
