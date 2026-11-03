/* Menu bar icon (NSStatusItem in AppDelegate.swift) as a StatusNotifierItem.
 *
 * GNOME shows these only with the AppIndicator extension (on by default in
 * Ubuntu); KDE and most other desktops show them natively. Without a
 * org.kde.StatusNotifierWatcher on the bus nothing happens, and the icon
 * appears later if a watcher starts. Clicking the icon opens YouCam's menu. */

#include <unistd.h>
#include "youcam.h"

#define ITEM_PATH "/StatusNotifierItem"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define WATCHER "org.kde.StatusNotifierWatcher"

static const char *Xml =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/>"
    "</interface></node>";

static struct {
    GDBusConnection *bus;
    char *name;
    char *icon;
    char *tooltip;
    void (*on_activate)(int x, int y);
} T;

static void call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                 const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)data;
    int x = 0, y = 0;
    if (g_variant_is_of_type(params, G_VARIANT_TYPE("(ii)"))) g_variant_get(params, "(ii)", &x, &y);
    if (g_strcmp0(method, "Scroll") != 0 && T.on_activate) T.on_activate(x, y);
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *get(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                     const char *prop, GError **err, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)data;
    if (!g_strcmp0(prop, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!g_strcmp0(prop, "Id")) return g_variant_new_string("youcam");
    if (!g_strcmp0(prop, "Title")) return g_variant_new_string("YouCam");
    if (!g_strcmp0(prop, "Status")) return g_variant_new_string("Active");
    if (!g_strcmp0(prop, "IconName")) return g_variant_new_string(T.icon);
    if (!g_strcmp0(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!g_strcmp0(prop, "ToolTip"))
        return g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0),
                             "YouCam", T.tooltip);
    return NULL;
}

static const GDBusInterfaceVTable VTable = {call, get, NULL, {0}};

static void watcher_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer data) {
    (void)name; (void)owner; (void)data;
    g_dbus_connection_call(c, WATCHER, "/StatusNotifierWatcher", WATCHER, "RegisterStatusNotifierItem",
                           g_variant_new("(s)", T.name), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

void status_icon_init(void (*on_activate)(int x, int y)) {
    T.on_activate = on_activate;
    T.icon = g_strdup("camera-video-symbolic");
    T.tooltip = g_strdup("YouCam — camera overlay");
    T.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!T.bus) return;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(Xml, NULL);
    g_dbus_connection_register_object(T.bus, ITEM_PATH, info->interfaces[0], &VTable, NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
    T.name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    g_bus_own_name_on_connection(T.bus, T.name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    g_bus_watch_name_on_connection(T.bus, WATCHER, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   watcher_appeared, NULL, NULL, NULL);
}

void status_icon_update(const char *icon_name, const char *tooltip) {
    if (!T.bus) return;
    if (g_strcmp0(icon_name, T.icon) != 0) {
        prefs_set_string(&T.icon, icon_name);
        g_dbus_connection_emit_signal(T.bus, NULL, ITEM_PATH, ITEM_IFACE, "NewIcon", NULL, NULL);
    }
    if (g_strcmp0(tooltip, T.tooltip) != 0) {
        prefs_set_string(&T.tooltip, tooltip);
        g_dbus_connection_emit_signal(T.bus, NULL, ITEM_PATH, ITEM_IFACE, "NewToolTip", NULL, NULL);
    }
}
