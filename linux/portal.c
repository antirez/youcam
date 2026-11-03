/* xdg-desktop-portal: app registration and ScreenCast (the ScreenCaptureKit
 * part of RecordingController.swift).
 *
 * On Wayland the only way to capture the screen is to ask the portal. It
 * shows a dialog the first time; with persist_mode=2 it hands back a restore
 * token so later recordings start without asking again. */

#include <string.h>
#include <gio/gunixfdlist.h>
#include "youcam.h"

#define BUS_NAME "org.freedesktop.portal.Desktop"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define SCREENCAST "org.freedesktop.portal.ScreenCast"
#define SOURCE_MONITOR 1
#define CURSOR_EMBEDDED 2
#define PERSIST_UNTIL_REVOKED 2

static guint TokenCounter;

GDBusConnection *portal_bus(void) {
    static GDBusConnection *conn;
    if (!conn) conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    return conn;
}

/* Give the portal an app id for a non-sandboxed app. GtkApplication may have
 * done it already, which is fine. */
void portal_register_app(void) {
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(portal_bus(), BUS_NAME, OBJECT_PATH,
        "org.freedesktop.host.portal.Registry", "Register",
        g_variant_new("(sa{sv})", APP_ID, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (r) g_variant_unref(r);
    if (err && !strstr(err->message, "already associated"))
        g_printerr("youcam: portal registry unavailable: %s\n", err->message);
    g_clear_error(&err);
}

void portal_close_session(const char *handle) {
    if (!handle) return;
    g_dbus_connection_call(portal_bus(), BUS_NAME, handle, "org.freedesktop.portal.Session", "Close",
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* ---------------------------------------------------------------- requests
 * Portal methods answer through a Request object's Response signal. We
 * subscribe to the predictable request path before calling. */

typedef struct {
    guint subscription;
    request_done done;
    void *ud;
} request;

static void on_response(GDBusConnection *c, const char *sender, const char *path,
                        const char *iface, const char *signal, GVariant *params, gpointer data) {
    (void)sender; (void)path; (void)iface; (void)signal;
    request *r = data;
    g_dbus_connection_signal_unsubscribe(c, r->subscription);
    guint32 code;
    GVariant *results;
    g_variant_get(params, "(u@a{sv})", &code, &results);
    r->done(code == 0 ? results : NULL, r->ud);
    g_variant_unref(results);
    g_free(r);
}

static char *new_token(void) { return g_strdup_printf("youcam%u", ++TokenCounter); }

/* Returns 0 and sets 'err' when the call itself fails. */
static int portal_request(const char *iface, const char *method, GVariant *args, const char *token,
                          request_done done, void *ud, GError **err) {
    char *sender = g_strdup(g_dbus_connection_get_unique_name(portal_bus()) + 1);
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    request *r = g_new0(request, 1);
    r->done = done;
    r->ud = ud;
    r->subscription = g_dbus_connection_signal_subscribe(portal_bus(), BUS_NAME,
        "org.freedesktop.portal.Request", "Response", path, NULL,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, r, NULL);
    GVariant *reply = g_dbus_connection_call_sync(portal_bus(), BUS_NAME, OBJECT_PATH, iface, method,
                                                  args, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, err);
    g_free(sender);
    g_free(path);
    if (!reply) {
        g_dbus_connection_signal_unsubscribe(portal_bus(), r->subscription);
        g_free(r);
        return 0;
    }
    g_variant_unref(reply);
    return 1;
}

/* Call 'method' with the given leading arguments plus an options dict that
 * gets the request handle token. Shared with hotkeys.c. */
int portal_call(const char *iface, const char *method, GVariant **prefix_args, int n_prefix,
                GVariantBuilder *options, request_done done, void *ud, GError **err) {
    char *token = new_token();
    g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token));
    GVariant *children[8];
    for (int i = 0; i < n_prefix; i++) children[i] = prefix_args[i];
    children[n_prefix] = g_variant_builder_end(options);
    int ok = portal_request(iface, method, g_variant_new_tuple(children, n_prefix + 1), token, done, ud, err);
    g_free(token);
    return ok;
}

/* -------------------------------------------------------------- screencast */

typedef struct {
    char *restore_token;
    char *session;
    screencast_ready ready;
    portal_error error;
    void *ud;
} screencast;

static void screencast_free(screencast *s) {
    g_free(s->restore_token);
    g_free(s->session);
    g_free(s);
}

static void screencast_fail(screencast *s, const char *message) {
    portal_close_session(s->session);
    s->error(message, s->ud);
    screencast_free(s);
}

static void screencast_call(screencast *s, const char *method, GVariant **args, int n,
                            GVariantBuilder *options, request_done done) {
    GError *err = NULL;
    if (!portal_call(SCREENCAST, method, args, n, options, done, s, &err)) {
        char *msg = g_strdup_printf("The screen capture portal failed: %s", err->message);
        g_clear_error(&err);
        screencast_fail(s, msg);
        g_free(msg);
    }
}

static void started(GVariant *results, void *ud) {
    screencast *s = ud;
    GVariant *streams = results ? g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})")) : NULL;
    if (!streams || g_variant_n_children(streams) == 0) {
        if (streams) g_variant_unref(streams);
        return screencast_fail(s, "Screen sharing was cancelled.");
    }
    guint32 node;
    GVariant *props;
    g_variant_get_child(streams, 0, "(u@a{sv})", &node, &props);
    int has_pos = 0, x = 0, y = 0;
    if (g_variant_lookup(props, "position", "(ii)", &x, &y)) has_pos = 1;
    g_variant_unref(props);
    g_variant_unref(streams);
    const char *token = NULL;
    g_variant_lookup(results, "restore_token", "&s", &token);

    GError *err = NULL;
    GUnixFDList *fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(portal_bus(), BUS_NAME, OBJECT_PATH,
        SCREENCAST, "OpenPipeWireRemote", g_variant_new("(oa{sv})", s->session, NULL),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL, &err);
    if (!reply) {
        char *msg = g_strdup_printf("PipeWire is not available: %s", err->message);
        g_clear_error(&err);
        screencast_fail(s, msg);
        g_free(msg);
        return;
    }
    gint32 index;
    g_variant_get(reply, "(h)", &index);
    int fd = g_unix_fd_list_get(fds, index, NULL);
    g_variant_unref(reply);
    g_object_unref(fds);
    s->ready(fd, node, token ? token : s->restore_token, s->session, has_pos, x, y, s->ud);
    screencast_free(s);
}

static void selected(GVariant *results, void *ud) {
    screencast *s = ud;
    if (!results) return screencast_fail(s, "Screen sharing was cancelled.");
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GVariant *args[] = {g_variant_new_object_path(s->session), g_variant_new_string("")};
    screencast_call(s, "Start", args, 2, &opts, started);
}

static void created(GVariant *results, void *ud) {
    screencast *s = ud;
    const char *handle = NULL;
    if (!results || !g_variant_lookup(results, "session_handle", "&s", &handle))
        return screencast_fail(s, "Screen sharing was cancelled.");
    s->session = g_strdup(handle);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(SOURCE_MONITOR));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(CURSOR_EMBEDDED));
    g_variant_builder_add(&opts, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
    if (s->restore_token)
        g_variant_builder_add(&opts, "{sv}", "restore_token", g_variant_new_string(s->restore_token));
    GVariant *args[] = {g_variant_new_object_path(s->session)};
    screencast_call(s, "SelectSources", args, 1, &opts, selected);
}

void portal_screencast_start(const char *restore_token, screencast_ready ready,
                             portal_error error, void *ud) {
    screencast *s = g_new0(screencast, 1);
    s->restore_token = restore_token ? g_strdup(restore_token) : NULL;
    s->ready = ready;
    s->error = error;
    s->ud = ud;
    char *session_token = new_token();
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    g_free(session_token);
    screencast_call(s, "CreateSession", NULL, 0, &opts, created);
}
