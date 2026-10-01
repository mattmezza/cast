/* Exercise private portal buffer handling without a compositor or permissions. */
#include "../src/wayland.c"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

typedef struct {
    GDBusConnection *connection;
    char *sender, *request;
} MockResponse;
static gboolean mock_cancel(gpointer data)
{
    MockResponse *response = data;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    GError *error = NULL;
    assert(g_dbus_connection_emit_signal(response->connection, response->sender, response->request,
                                         "org.freedesktop.portal.Request", "Response",
                                         g_variant_new("(ua{sv})", 1, &b), &error));
    g_object_unref(response->connection);
    g_free(response->sender);
    g_free(response->request);
    g_free(response);
    return G_SOURCE_REMOVE;
}

static const char introspection[] =
    "<node><interface name='org.freedesktop.portal.ScreenCast'>"
    "<method name='CreateSession'><arg direction='in' type='a{sv}'/><arg direction='out' "
    "type='o'/></method>"
    "<method name='SelectSources'><arg direction='in' type='o'/><arg direction='in' "
    "type='a{sv}'/><arg direction='out' type='o'/></method>"
    "<method name='Start'><arg direction='in' type='o'/><arg direction='in' type='s'/><arg "
    "direction='in' type='a{sv}'/><arg direction='out' type='o'/></method>"
    "<property name='AvailableCursorModes' type='u' access='read'/>"
    "<property name='AvailableSourceTypes' type='u' access='read'/>"
    "<property name='version' type='u' access='read'/></interface></node>";
static GVariant *mock_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
                               const gchar *interface, const gchar *property, GError **error,
                               gpointer data)
{
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)error;
    (void)data;
    return g_variant_new_uint32(!strcmp(property, "AvailableCursorModes")   ? 4
                                : !strcmp(property, "AvailableSourceTypes") ? 1
                                                                            : 6);
}
static void mock_method(GDBusConnection *connection, const gchar *sender, const gchar *path,
                        const gchar *interface, const gchar *method, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer data)
{
    (void)path;
    (void)interface;
    (void)data;
    unsigned index = !strcmp(method, "CreateSession")   ? 0
                     : !strcmp(method, "SelectSources") ? 1
                                                        : 2;
    GVariant *dict = g_variant_get_child_value(parameters, index);
    const char *token = NULL;
    assert(g_variant_lookup(dict, "handle_token", "&s", &token));
    char *unique = g_strdup(sender + 1);
    for (char *p = unique; *p; p++) {
        if (*p == '.') {
            *p = '_';
        }
    }
    char *request = g_strdup_printf(PORTAL_PATH "/request/%s/%s", unique, token);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request));
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    if (index == 0) {
        g_variant_builder_add(&b, "{sv}", "session_handle",
                              g_variant_new_string(PORTAL_PATH "/session/mock"));
    }
    /* Keep Start consent pending after its method returns, then cancel it. */
    GError *error = NULL;
    if (index == 2) {
        MockResponse *response = g_new0(MockResponse, 1);
        response->connection = g_object_ref(connection);
        response->sender = g_strdup(sender);
        response->request = g_strdup(request);
        g_timeout_add(150, mock_cancel, response);
    } else {
        assert(g_dbus_connection_emit_signal(connection, sender, request,
                                             "org.freedesktop.portal.Request", "Response",
                                             g_variant_new("(ua{sv})", 0, &b), &error));
    }
    g_free(request);
    g_free(unique);
    g_variant_unref(dict);
}
static const GDBusInterfaceVTable mock_vtable = {.method_call = mock_method,
                                                 .get_property = mock_property};
static void mock_portal(int ready)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    assert(bus);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(introspection, &error);
    assert(node);
    assert(g_dbus_connection_register_object(bus, PORTAL_PATH, node->interfaces[0], &mock_vtable,
                                             NULL, NULL, &error));
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", PORTAL, 0), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
        &error);
    assert(r);
    g_variant_unref(r);
    assert(write(ready, "R", 1) == 1);
    close(ready);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    _exit(0);
}
static void test_consent_lifecycle(void)
{
    GTestDBus *testbus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(testbus);
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]);
        mock_portal(ready[1]);
    }
    close(ready[1]);
    struct pollfd ready_fd = {ready[0], POLLIN, 0};
    if (poll(&ready_fd, 1, 3000) <= 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        fprintf(stderr, "Mock portal failed to start within three seconds\n");
        abort();
    }
    char marker;
    assert(read(ready[0], &marker, 1) == 1);
    close(ready[0]);
    Config c = {.fps = 30};
    snprintf(c.capture_kind, sizeof c.capture_kind, "monitor");
    char err[1024];
    Platform *platform = wayland_open(&c, err, sizeof err);
    assert(platform);
    Wayland *w = (Wayland *)platform;
    Frame frame = {0};
    Cursor cursor = {0};
    uint64_t deadline = cast_now_ns() + UINT64_C(3000000000);
    bool checked_pending = false;
    char *args[] = {"capture", "monitor"};
    while (!w->failed && cast_now_ns() < deadline) {
        wayland_capture(platform, &frame, &cursor, err, sizeof err);
        if (w->stage == 3 && w->awaiting_response && !w->pending) {
            assert(wayland_command(platform, &c, 2, args, err, sizeof err) < 0);
            assert(strstr(err, "pending"));
            checked_pending = true;
        }
        g_usleep(1000);
    }
    assert(checked_pending && w->failed && strstr(w->error, "cancelled") && !w->latest.data);
    assert(w->generation == 0);
    while (w->pending) {
        g_main_context_iteration(NULL, TRUE);
    }
    assert(wayland_command(platform, &c, 2, args, err, sizeof err) == 0);
    assert(!w->failed);
    deadline = cast_now_ns() + UINT64_C(3000000000);
    while (!w->failed && cast_now_ns() < deadline) {
        wayland_capture(platform, &frame, &cursor, err, sizeof err);
        g_usleep(1000);
    }
    assert(w->failed && strstr(w->error, "cancelled"));
    /* Closing cast during a new request must drain callbacks without dangling userdata. */
    while (w->pending) {
        g_main_context_iteration(NULL, TRUE);
    }
    assert(wayland_command(platform, &c, 2, args, err, sizeof err) == 0);
    wayland_close(platform);
    frame_free(&frame);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    g_test_dbus_down(testbus);
    g_object_unref(testbus);
    puts("Mock portal asynchronous consent/cancellation/reselection passed");
}
static bool private_socket_available(void)
{
    char directory[] = "/tmp/cast-wayland-socket-XXXXXX";
    if (!mkdtemp(directory)) {
        return false;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof address.sun_path, "%s/probe", directory);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool available = fd >= 0 && bind(fd, (void *)&address, sizeof address) == 0;
    int saved = errno;
    if (fd >= 0) {
        close(fd);
    }
    unlink(address.sun_path);
    rmdir(directory);
    if (!available) {
        fprintf(stderr,
                "Private D-Bus test needs local Unix sockets: %s. Run --unit-only for "
                "CPU buffer/capability tests in this environment.\n",
                strerror(saved));
    }
    return available;
}
int main(int argc, char **argv)
{
    bool unit_only = argc == 2 && !strcmp(argv[1], "--unit-only");
    if (argc != 1 && !unit_only) {
        fprintf(stderr, "usage: %s [--unit-only]\n", argv[0]);
        return 1;
    }
    Wayland w = {0};
    w.format.size = SPA_RECTANGLE(2, 2);
    w.format.format = SPA_VIDEO_FORMAT_BGRx;
    uint8_t pixels[] = {3, 2, 1, 0, 6, 5, 4, 0, 9, 8, 7, 0, 12, 11, 10, 0};
    struct spa_chunk chunk = {.offset = 0, .size = 16, .stride = 8};
    struct spa_data d = {.type = SPA_DATA_MemPtr, .data = pixels, .maxsize = 16, .chunk = &chunk};
    assert(copy_pixels(&w, &d) == 0);
    assert(w.latest.data[0] == 1 && w.latest.data[1] == 2 && w.latest.data[2] == 3 &&
           w.latest.data[3] == 255);
    chunk.stride = -8;
    assert(copy_pixels(&w, &d) == 0);
    assert(w.latest.data[0] == 7);
    chunk.stride = 8;
    chunk.offset = 1;
    assert(copy_pixels(&w, &d) < 0);
    chunk.offset = 0;
    chunk.size = 15;
    assert(copy_pixels(&w, &d) < 0);
    chunk.size = 16;
    d.type = SPA_DATA_DmaBuf;
    assert(copy_pixels(&w, &d) < 0);
    d.type = SPA_DATA_MemPtr;
    chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
    assert(copy_pixels(&w, &d) < 0);
    chunk.flags = 0;
    w.cursor.valid = true;
    session_closed(NULL, NULL, NULL, NULL, NULL, NULL, &w);
    assert(w.failed && !w.latest.data && !w.cursor.valid);
    assert(strstr(w.error, "revoked"));
    w.failed = false;
    GVariant *owner = g_variant_ref_sink(g_variant_new("(sss)", PORTAL, ":1.2", ""));
    owner_changed(NULL, NULL, NULL, NULL, NULL, owner, &w);
    g_variant_unref(owner);
    assert(w.failed);
    w.cursor_mode = 4;
    Capabilities cap = wayland_capabilities((Platform *)&w);
    assert(cap.capture && cap.cursor_metadata && !cap.embedded_cursor && !cap.input &&
           !cap.preview);
    w.cursor_mode = 2;
    cap = wayland_capabilities((Platform *)&w);
    assert(cap.embedded_cursor && !cap.cursor_metadata);
    Config c = {0};
    char error[1024];
    c.zoom_follow = true;
    assert(wayland_reconfigure((Platform *)&w, &c, error, sizeof error) < 0);
    char *args[] = {"capture", "region", "select"};
    assert(wayland_command((Platform *)&w, &c, 3, args, error, sizeof error) < 0);
    puts("Wayland buffer, revocation and capability tests passed");
    if (!unit_only) {
        if (!private_socket_available()) {
            return 1;
        }
        test_consent_lifecycle();
    }
    return 0;
}
