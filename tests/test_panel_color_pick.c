#include "panel_color_pick.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
#ifdef WITH_WAYLAND
    test_portal();
#endif
    puts("panel color picker tests passed");
    return 0;
}
