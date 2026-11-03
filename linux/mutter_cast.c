/* Screen capture without YouCam's own windows (Linux addition).
 *
 * macOS excludes the panel and the area guides from recordings. GNOME's
 * portal cannot: for a monitor it copies the finished screen image. When
 * the YouCam Capture Exclusion extension (gnome-extension/) is active,
 * YouCam records through Mutter's RecordArea instead, which repaints the
 * stage for the stream, and the extension leaves out every window whose
 * role is CAPTURE_HIDDEN_ROLE. Without the extension the portal is used. */

#include <math.h>
#include <string.h>
#include "youcam.h"

#define EXTENSION_UUID "youcam-capture-exclusion@youcam.app"
#define EXTENSION_ACTIVE 1           /* org.gnome.Shell.Extensions state. */
#define LAYOUT_MODE_LOGICAL 1        /* DisplayConfig: monitors sized in logical pixels. */
#define MUTTER_CURSOR_EMBEDDED 1
#define STREAM_TIMEOUT_S 5
#define SCREEN_CAST "org.gnome.Mutter.ScreenCast"
#define CAST_SESSION SCREEN_CAST ".Session"

static GDBusConnection *bus(void) { return g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL); }

int mutter_cast_available(void) {
    const char *forced = g_getenv("YOUCAM_CAPTURE");   /* Tests: "portal" or "mutter". */
    if (forced) return strcmp(forced, "mutter") == 0;
    GDBusConnection *c = bus();
    if (!c) return 0;
    GVariant *r = g_dbus_connection_call_sync(c, "org.gnome.Shell", "/org/gnome/Shell",
        "org.gnome.Shell.Extensions", "GetExtensionInfo", g_variant_new("(s)", EXTENSION_UUID),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_object_unref(c);
    if (!r) return 0;
    GVariant *info = g_variant_get_child_value(r, 0);
    double state = 0;
    g_variant_lookup(info, "state", "d", &state);
    g_variant_unref(info);
    g_variant_unref(r);
    return state == EXTENSION_ACTIVE;
}

/* The stage rectangle of a monitor, from DisplayConfig.GetCurrentState:
 * the logical monitor gives the origin, scale and rotation, the current
 * mode the size in physical pixels. Returns 0 if the connector is unknown. */
int mutter_monitor_rect(GVariant *state, const char *connector, int out[4]) {
    GVariant *monitors = g_variant_get_child_value(state, 1);
    GVariant *logical = g_variant_get_child_value(state, 2);
    GVariant *props = g_variant_get_child_value(state, 3);
    guint32 layout = LAYOUT_MODE_LOGICAL;
    g_variant_lookup(props, "layout-mode", "u", &layout);
    int mode_w = 0, mode_h = 0, found = 0;
    for (gsize i = 0; i < g_variant_n_children(monitors) && !mode_w; i++) {
        GVariant *m = g_variant_get_child_value(monitors, i);
        const char *name;
        g_variant_get_child(m, 0, "(&ssss)", &name, NULL, NULL, NULL);
        GVariant *modes = strcmp(name, connector) == 0 ? g_variant_get_child_value(m, 1) : NULL;
        for (gsize k = 0; modes && k < g_variant_n_children(modes); k++) {
            GVariant *mode = g_variant_get_child_value(modes, k);
            GVariant *mode_props = g_variant_get_child_value(mode, 6);
            gboolean current = FALSE;
            g_variant_lookup(mode_props, "is-current", "b", &current);
            if (current) {
                g_variant_get_child(mode, 1, "i", &mode_w);
                g_variant_get_child(mode, 2, "i", &mode_h);
            }
            g_variant_unref(mode_props);
            g_variant_unref(mode);
        }
        if (modes) g_variant_unref(modes);
        g_variant_unref(m);
    }
    for (gsize i = 0; i < g_variant_n_children(logical) && mode_w && !found; i++) {
        GVariant *lm = g_variant_get_child_value(logical, i);
        int x, y;
        double scale;
        guint32 transform;
        g_variant_get_child(lm, 0, "i", &x);
        g_variant_get_child(lm, 1, "i", &y);
        g_variant_get_child(lm, 2, "d", &scale);
        g_variant_get_child(lm, 3, "u", &transform);
        GVariant *list = g_variant_get_child_value(lm, 5);
        for (gsize k = 0; k < g_variant_n_children(list) && !found; k++) {
            const char *name;
            g_variant_get_child(list, k, "(&ssss)", &name, NULL, NULL, NULL);
            found = strcmp(name, connector) == 0;
        }
        if (found) {
            int w = mode_w, h = mode_h;
            if (transform % 2) { w = mode_h; h = mode_w; }   /* 90 and 270 degrees, flipped or not. */
            if (layout == LAYOUT_MODE_LOGICAL && scale > 0) { w = (int)lround(w / scale); h = (int)lround(h / scale); }
            out[0] = x; out[1] = y; out[2] = w; out[3] = h;
        }
        g_variant_unref(list);
        g_variant_unref(lm);
    }
    g_variant_unref(props);
    g_variant_unref(logical);
    g_variant_unref(monitors);
    return found;
}

typedef struct {
    GDBusConnection *bus;
    char *session;
    guint subscription, timeout;
    screencast_ready ready;
    portal_error error;
    void *ud;
} cast;

static void cast_free(cast *c) {
    if (c->subscription) g_dbus_connection_signal_unsubscribe(c->bus, c->subscription);
    if (c->timeout) g_source_remove(c->timeout);
    g_object_unref(c->bus);
    g_free(c->session);
    g_free(c);
}

static void stream_added(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                         const char *signal, GVariant *params, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal;
    cast *c = data;
    guint32 node;
    g_variant_get(params, "(u)", &node);
    c->ready(-1, node, NULL, c->session, 0, 0, 0, c->ud);
    cast_free(c);
}

static gboolean stream_timeout(gpointer data) {
    cast *c = data;
    c->timeout = 0;
    mutter_cast_stop(c->session);
    c->error("The screen recording did not start. Try again, or disable the YouCam Capture Exclusion extension.",
             c->ud);
    cast_free(c);
    return G_SOURCE_REMOVE;
}

static GVariant *call(GDBusConnection *b, const char *path, const char *iface, const char *method,
                      GVariant *args, const char *type, GError **err) {
    return g_dbus_connection_call_sync(b, SCREEN_CAST, path, iface, method, args,
                                       type ? G_VARIANT_TYPE(type) : NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, err);
}

/* Records the monitor 'connector' (a GDK monitor model is its connector
 * name). Calls ready() with fd -1 and the PipeWire node, or error(). */
void mutter_cast_start(const char *connector, screencast_ready ready, portal_error error, void *ud) {
    GError *err = NULL;
    GDBusConnection *b = bus();
    GVariant *state = b ? g_dbus_connection_call_sync(b, "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL,
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err) : NULL;
    int r[4];
    if (!state || !mutter_monitor_rect(state, connector, r)) {
        g_printerr("youcam: no stage area for %s: %s\n", connector, err ? err->message : "unknown monitor");
        g_clear_error(&err);
        if (state) g_variant_unref(state);
        if (b) g_object_unref(b);
        error("The display to record was not found.", ud);
        return;
    }
    g_variant_unref(state);
    cast *c = g_new0(cast, 1);
    *c = (cast){b, NULL, 0, 0, ready, error, ud};
    GVariant *v = call(b, "/org/gnome/Mutter/ScreenCast", SCREEN_CAST, "CreateSession",
                       g_variant_new_parsed("(@a{sv} {},)"), "(o)", &err);
    if (v) {
        g_variant_get(v, "(o)", &c->session);
        g_variant_unref(v);
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(MUTTER_CURSOR_EMBEDDED));
        v = call(b, c->session, CAST_SESSION, "RecordArea",
                 g_variant_new("(iiiia{sv})", r[0], r[1], r[2], r[3], &props), "(o)", &err);
    }
    if (v) {
        char *stream;
        g_variant_get(v, "(o)", &stream);
        g_variant_unref(v);
        c->subscription = g_dbus_connection_signal_subscribe(b, NULL, SCREEN_CAST ".Stream",
            "PipeWireStreamAdded", stream, NULL, G_DBUS_SIGNAL_FLAGS_NONE, stream_added, c, NULL);
        g_free(stream);
        v = call(b, c->session, CAST_SESSION, "Start", NULL, NULL, &err);
    }
    if (!v) {
        g_printerr("youcam: Mutter screen cast failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        mutter_cast_stop(c->session);
        error("The screen recording could not start.", ud);
        cast_free(c);
        return;
    }
    g_variant_unref(v);
    c->timeout = g_timeout_add_seconds(STREAM_TIMEOUT_S, stream_timeout, c);
}

void mutter_cast_stop(const char *session) {
    GDBusConnection *b = session ? bus() : NULL;
    if (!b) return;
    GVariant *v = call(b, session, CAST_SESSION, "Stop", NULL, NULL, NULL);   /* Already gone: fine. */
    if (v) g_variant_unref(v);
    g_object_unref(b);
}
