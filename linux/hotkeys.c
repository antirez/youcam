/* Global shortcuts (GlobalHotKeys.swift) through the GlobalShortcuts portal.
 *
 * Control-Option-Command on the Mac becomes Control-Alt-Super. GNOME asks
 * the user to confirm the bindings once; until then, or if the portal is
 * missing, the menu says the shortcuts are unavailable. */

#include <string.h>
#include <unistd.h>
#include "youcam.h"

#define IFACE "org.freedesktop.portal.GlobalShortcuts"

static struct {
    char *session;
    guint activated;
    int available;     /* -1 unknown, 0 no, 1 yes */
    void (*on_record)(void);
    void (*on_pause)(void);
} H = {.available = -1};

int hotkeys_available(void) { return H.available != 0; }

static void on_activated(GDBusConnection *c, const char *sender, const char *path,
                         const char *iface, const char *signal, GVariant *params, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)data;
    const char *session, *id;
    g_variant_get_child(params, 0, "&o", &session);
    g_variant_get_child(params, 1, "&s", &id);
    if (!H.session || strcmp(session, H.session) != 0) return;
    if (strcmp(id, "toggle-recording") == 0) H.on_record();
    else if (strcmp(id, "toggle-pause") == 0) H.on_pause();
}

static void bound(GVariant *results, void *ud) {
    (void)ud;
    H.available = results != NULL;
}

static void add_shortcut(GVariantBuilder *b, const char *id, const char *text, const char *trigger) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&props, "{sv}", "description", g_variant_new_string(text));
    g_variant_builder_add(&props, "{sv}", "preferred_trigger", g_variant_new_string(trigger));
    g_variant_builder_add(b, "(sa{sv})", id, &props);
}

static void unavailable(const char *why) {
    g_printerr("youcam: global shortcuts unavailable: %s\n", why);
    H.available = 0;
}

static void created(GVariant *results, void *ud) {
    (void)ud;
    const char *handle = NULL;
    if (!results || !g_variant_lookup(results, "session_handle", "&s", &handle))
        return unavailable("session refused");
    H.session = g_strdup(handle);
    H.activated = g_dbus_connection_signal_subscribe(portal_bus(), "org.freedesktop.portal.Desktop",
        IFACE, "Activated", "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_activated, NULL, NULL);
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa{sv})"));
    add_shortcut(&list, "toggle-recording", "Start or stop recording", "CTRL+ALT+LOGO+r");
    add_shortcut(&list, "toggle-pause", "Pause or resume recording", "CTRL+ALT+LOGO+p");
    GVariant *args[] = {g_variant_new_object_path(H.session), g_variant_builder_end(&list),
                        g_variant_new_string("")};
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GError *err = NULL;
    if (!portal_call(IFACE, "BindShortcuts", args, 3, &opts, bound, NULL, &err)) {
        unavailable(err->message);
        g_clear_error(&err);
    }
}

void hotkeys_init(void (*on_record)(void), void (*on_pause)(void)) {
    H.on_record = on_record;
    H.on_pause = on_pause;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    char *token = g_strdup_printf("youcamkeys%d", (int)getpid());
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token));
    g_free(token);
    GError *err = NULL;
    if (!portal_call(IFACE, "CreateSession", NULL, 0, &opts, created, NULL, &err)) {
        unavailable(err->message);
        g_clear_error(&err);
    }
}

void hotkeys_shutdown(void) {
    if (H.activated) g_dbus_connection_signal_unsubscribe(portal_bus(), H.activated);
    portal_close_session(H.session);
    g_free(H.session);
    H.session = NULL;
    H.activated = 0;
}
