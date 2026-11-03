/* Application controller (AppDelegate.swift): owns the camera window, the
 * recording panel, the recording area, the menu and the global actions. */

#include <math.h>
#include <string.h>
#include "youcam.h"

#define SIZE_STEP 1.15
#define PANEL_BOTTOM_MARGIN 40
#define SAVE_DELAY_MS 300
#define SMOKE_TEST_S 2
#define LAYER_TICK_MS 33

static const struct { const char *name; int width; } Sizes[] = {
    {"Small", 180}, {"Medium", 280}, {"Large", 400}, {"Extra Large", 560}};

static const char *HelpText =
    "Drag the camera to move it. Scroll to resize. Right-click the camera or recording panel "
    "for controls.\n\nYour recording area is remembered across restarts. Choose Select Area and "
    "drag a rectangle to prepare a region. The surrounding screen stays dimmed; your selected area "
    "stays clear. Drag the white corner dots to fine-tune the area before recording. Choose Whole "
    "Screen to remove the dimming.\n\nStart Recording when ready. The first time, the system asks "
    "which screen YouCam may record; the choice is remembered. Movies are saved automatically to "
    "your Desktop. Control–Alt–Super–R starts/stops; Control–Alt–Super–P pauses/resumes.\n\n"
    "Audio Input selects the microphone and offers an input-level slider and live meter. "
    "Noise Reduction removes background noise from the recorded voice. Automatic Microphone Level "
    "adjusts the system microphone level while YouCam uses it, so the voice is neither too quiet nor "
    "clipped, and puts the original level back afterwards.\n\n"
    "Show Recording Panel displays optional controls. Hide Camera lets you record just the screen.\n\n"
    "Transparency optionally lets you see through the camera while you work. Adjust it from the "
    "camera menu; Reset Transparency to 0% turns it off. Saved Video chooses what recordings show: "
    "Same as Preview, Opaque, or Choose After Recording, which opens a window when you stop: pick the "
    "camera transparency with a preview, then Save the movie to your Desktop, keep it for Later, or "
    "Discard it. With the YouCam Capture Exclusion extension any value up to 80% is possible; without "
    "it, up to the preview transparency used while recording. Adjust Last Recording Transparency… "
    "reopens the last recording kept for later. Both choices are remembered.\n\n"
    "Center Stage (Follow Face) gently pans and zooms the camera to keep your face centred, in "
    "the preview and in recordings. Face detection uses pico by Nenad Markus (MIT license).\n\n"
    "Background (Linux only) removes what is behind you: Transparent shows only you, Blur softens the "
    "room, Image and Color replace it. It runs on your computer and applies to recordings too. If "
    "furniture next to you stays visible, Learn Empty Room shows YouCam the room without you.";

static struct {
    GtkApplication *gapp;
    GtkWidget *cam, *panel;
    int smoke, visible, quitting, panel_visible;
    recording_area area;
    guint save_timer;
} App;

/* ------------------------------------------------------------- geometry */

static GdkMonitor *camera_monitor(void) {
    GdkDisplay *d = gdk_display_get_default();
    GdkWindow *w = App.cam ? gtk_widget_get_window(App.cam) : NULL;
    GdkMonitor *m = w ? gdk_display_get_monitor_at_window(d, w) : NULL;
    if (!m) m = gdk_display_get_primary_monitor(d);
    return m ? m : gdk_display_get_monitor(d, 0);
}

static rect window_frame(void) {
    int x, y, w, h;
    gtk_window_get_position(GTK_WINDOW(App.cam), &x, &y);
    gtk_window_get_size(GTK_WINDOW(App.cam), &w, &h);
    return (rect){x, y, w, h};
}

static void set_camera_frame(rect f) {
    gtk_widget_set_size_request(App.cam, (int)f.w, (int)f.h);
    gtk_window_resize(GTK_WINDOW(App.cam), (int)f.w, (int)f.h);
    gtk_window_move(GTK_WINDOW(App.cam), (int)f.x, (int)f.y);
    overlay_apply_shape();
}

/* Saved position, on the display it overlaps most; else bottom right. */
static rect restored_frame(void) {
    GdkDisplay *d = gdk_display_get_default();
    rect screen = monitor_workarea(camera_monitor());
    if (!Prefs.has_origin) return overlay_initial(Prefs.width, Prefs.shape, screen);
    rect f = {Prefs.origin_x, Prefs.origin_y, 0, 0};
    overlay_size(Prefs.width, Prefs.shape, screen, &f.w, &f.h);
    GdkMonitor *best = NULL;
    double best_area = 0;
    for (int i = 0; i < gdk_display_get_n_monitors(d); i++) {
        GdkMonitor *m = gdk_display_get_monitor(d, i);
        double a = rect_intersection_area(f, monitor_workarea(m));
        if (a > best_area) { best_area = a; best = m; }
    }
    if (!best) return overlay_initial(Prefs.width, Prefs.shape, screen);
    return overlay_resized(f, Prefs.width, Prefs.shape, monitor_workarea(best));
}

static gboolean save_now(gpointer data) {
    (void)data;
    App.save_timer = 0;
    rect f = window_frame();
    Prefs.width = f.w;
    Prefs.origin_x = f.x;
    Prefs.origin_y = f.y;
    Prefs.has_origin = 1;
    prefs_save();
    return G_SOURCE_REMOVE;
}

/* Dragging produces many configure events: save once it settles. */
void app_save_geometry(void) {
    if (!App.cam || !gtk_widget_get_visible(App.cam)) return;
    if (App.save_timer) g_source_remove(App.save_timer);
    App.save_timer = g_timeout_add(SAVE_DELAY_MS, save_now, NULL);
}

void app_resize(double width) {
    set_camera_frame(overlay_resized(window_frame(), width, Prefs.shape, monitor_workarea(camera_monitor())));
    overlay_queue_draw();
    app_save_geometry();
}

static void fit_on_screen(void) { app_resize(window_frame().w); }

/* -------------------------------------------------------- recording area */

const char *app_area_name(void) {
    static char buf[128];
    switch (App.area.mode) {
    case AREA_WHOLE_SCREEN: return "Whole Screen";
    case AREA_DISPLAY: return monitor_by_name(App.area.monitor) ? App.area.monitor : "Disconnected Display";
    case AREA_REGION:
        snprintf(buf, sizeof(buf), "Selected Area (%d × %d)", (int)App.area.local.w, (int)App.area.local.h);
        return buf;
    }
    return "";
}

static void update_ui(void);
static gboolean layer_tick(gpointer data);
static guint LayerTimer;

static void refresh_area_overlay(void) {
    GdkMonitor *m = App.area.mode == AREA_REGION ? monitor_by_name(App.area.monitor) : NULL;
    if (region_is_selecting() || App.quitting || !m) area_overlay_hide();
    else area_overlay_show(m, App.area.local);
    if (App.cam && gtk_widget_get_realized(App.cam)) gdk_window_raise(gtk_widget_get_window(App.cam));
    if (App.panel_visible) gdk_window_raise(gtk_widget_get_window(App.panel));
}

static void set_area(recording_area area) {
    if (PHASE_BUSY(recording_phase_now()) || App.quitting) return;
    App.area = area;
    Prefs.area = area;
    prefs_save();
    refresh_area_overlay();
    update_ui();
}

static void area_dragged(GdkMonitor *m, rect local) {
    if (PHASE_BUSY(recording_phase_now()) || App.quitting) return;
    App.area.mode = AREA_REGION;
    g_strlcpy(App.area.monitor, monitor_name(m), sizeof(App.area.monitor));
    App.area.local = local;
    Prefs.area = App.area;
    prefs_save();
    update_ui();
}

static void area_picked(GdkMonitor *m, const rect *local) {
    if (App.quitting) return;
    if (m && local) {
        recording_area a = {AREA_REGION, "", *local};
        g_strlcpy(a.monitor, monitor_name(m), sizeof(a.monitor));
        set_area(a);
    } else {
        refresh_area_overlay();
        update_ui();
    }
}

static void select_area(void) {
    if (PHASE_BUSY(recording_phase_now()) || region_is_selecting()) return;
    area_overlay_hide();
    region_select(area_picked);
    update_ui();
}

static void select_whole_screen(void) { set_area((recording_area){AREA_WHOLE_SCREEN, "", {0, 0, 0, 0}}); }

static void select_display(const char *name) {
    if (!monitor_by_name(name)) return;
    recording_area a = {AREA_DISPLAY, "", {0, 0, 0, 0}};
    g_strlcpy(a.monitor, name, sizeof(a.monitor));
    set_area(a);
}

static void monitors_changed(GdkScreen *screen, gpointer data) {
    (void)screen; (void)data;
    region_cancel();
    fit_on_screen();
    recording_area valid = prefs_area_validate(App.area, gdk_display_get_default());
    if (valid.mode != App.area.mode) {
        App.area = valid;
        if (PHASE_BUSY(recording_phase_now())) recording_stop();
    }
    refresh_area_overlay();
    update_ui();
}

/* ------------------------------------------------------------- recording */

void app_toggle_recording(void) {
    if (App.smoke) return;
    if (PHASE_BUSY(recording_phase_now())) {
        recording_stop();
        return;
    }
    if (region_is_selecting()) return;
    GdkMonitor *m = camera_monitor();
    if (App.area.mode != AREA_WHOLE_SCREEN) m = monitor_by_name(App.area.monitor);
    if (!m) return;
    recording_start(m, App.area.mode == AREA_REGION ? &App.area.local : NULL);
}

void app_toggle_pause(void) { recording_toggle_pause(); }

void app_panel_play(void) {
    recording_phase p = recording_phase_now();
    if (p == PHASE_PAUSED) recording_toggle_pause();
    else if (p == PHASE_IDLE) app_toggle_recording();
}

void app_panel_stop(void) { recording_stop(); }

void app_show_message(const char *title, const char *text, GtkMessageType type) {
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, type, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), type == GTK_MESSAGE_INFO ? "Got It" : "OK", GTK_RESPONSE_OK);
    gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void recording_error(const char *message) {
    if (App.quitting || App.smoke) return;
    if (g_getenv("YOUCAM_NO_DIALOGS")) {   /* --record-test: report, never block. */
        g_printerr("youcam: recording stopped: %s\n", message);
        app_quit();
        return;
    }
    app_show_message("Recording stopped", message, GTK_MESSAGE_WARNING);
}

/* A Choose After Recording movie stopped: the dialog finishes it. When
 * quitting it waits for "Adjust Last Recording Transparency…". */
static void project_ready(const char *path) {
    if (App.quitting || App.smoke || g_getenv("YOUCAM_NO_DIALOGS")) return;   /* recording.c printed the path. */
    later_dialog_open(path);
}

/* The recording goes on, opaque: say why without blocking, and keep the
 * message out of the recording when the extension is active. */
static void recording_notice(const char *message) {
    if (App.quitting || App.smoke || g_getenv("YOUCAM_NO_DIALOGS")) return;   /* recording.c logged it. */
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
                                          "Recording with an opaque camera");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", message);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_window_set_role(GTK_WINDOW(d), CAPTURE_HIDDEN_ROLE);
    gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}

static void microphone_lost(void) {
    recording_fail("The microphone was disconnected. The recording has been stopped to preserve your movie.");
}

static void reveal_recording(void) {
    const char *path = recording_last_path();
    if (!path) return;
    char *uri = g_filename_to_uri(path, NULL, NULL);
    const char *uris[] = {uri, NULL};
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(portal_bus(), "org.freedesktop.FileManager1",
        "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
        g_variant_new("(^ass)", uris, ""), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (r) g_variant_unref(r);
    else {
        char *dir = g_path_get_dirname(path);
        char *dir_uri = g_filename_to_uri(dir, NULL, NULL);
        g_app_info_launch_default_for_uri(dir_uri, NULL, NULL);
        g_free(dir_uri);
        g_free(dir);
        g_clear_error(&err);
    }
    g_free(uri);
}

/* ------------------------------------------------------------ visibility */

static void toggle_visibility(void) {
    App.visible = !App.visible;
    if (App.visible) {
        gtk_widget_show(App.cam);
        if (!App.smoke) camera_show();
    } else {
        gtk_widget_hide(App.cam);
        camera_hide();
    }
    update_ui();
}

int app_panel_visible(void) { return App.panel_visible; }

void app_toggle_panel(void) {
    if (App.panel_visible) {
        gtk_widget_hide(App.panel);
        App.panel_visible = 0;
        audio_monitor("panel", 0);
    } else {
        rect wa = monitor_workarea(camera_monitor());
        int w, h;
        gtk_widget_show(App.panel);
        gtk_window_get_size(GTK_WINDOW(App.panel), &w, &h);
        gtk_window_move(GTK_WINDOW(App.panel), (int)(wa.x + wa.w / 2 - w / 2.0),
                        (int)(wa.y + wa.h - h - PANEL_BOTTOM_MARGIN));
        App.panel_visible = 1;
        audio_monitor("panel", 1);
        update_ui();
    }
}

/* Launching YouCam again brings it back (applicationShouldHandleReopen). */
void app_reopen(void) {
    if (!App.cam) return;
    if (PHASE_BUSY(recording_phase_now()) || App.panel_visible) {
        if (!App.panel_visible) app_toggle_panel();
        gtk_window_present(GTK_WINDOW(App.panel));
        return;
    }
    if (!App.visible) toggle_visibility();
    fit_on_screen();
    gtk_window_present(GTK_WINDOW(App.cam));
}

/* -------------------------------------------------------------- updates */

static void update_ui(void) {
    recording_phase phase = recording_phase_now();
    area_overlay_set_editable(!PHASE_BUSY(phase) && !region_is_selecting() && !App.quitting);
    panel_update();
    const char *icon = phase == PHASE_RECORDING ? "media-record-symbolic"
                     : phase == PHASE_PAUSED ? "media-playback-pause-symbolic"
                     : App.visible ? "camera-video-symbolic" : "camera-disabled-symbolic";
    char *tip = g_strdup_printf("YouCam — %s", PHASE_BUSY(phase) ? recording_status() : camera_menu_title());
    status_icon_update(icon, tip);
    g_free(tip);
    if (phase == PHASE_RECORDING && !LayerTimer) LayerTimer = g_timeout_add(LAYER_TICK_MS, layer_tick, NULL);
    if (App.quitting && phase == PHASE_IDLE) app_quit();
}

static void camera_changed(void) {
    overlay_queue_draw();
    update_ui();
}

static void audio_changed(void) { panel_update_audio(); }

/* ------------------------------------------------------------------ menu */

typedef void (*action)(void);

static void run_action(GtkMenuItem *item, gpointer data) {
    (void)item;
    ((action)data)();
}

static GtkWidget *add_item(GtkWidget *menu, const char *label, action cb, int enabled) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    if (cb) g_signal_connect(item, "activate", G_CALLBACK(run_action), cb);
    gtk_widget_set_sensitive(item, enabled && cb);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* Check items look like radio items where the macOS menu shows one choice. */
static GtkWidget *add_check(GtkWidget *menu, const char *label, int active, int radio, int enabled) {
    GtkWidget *item = gtk_check_menu_item_new_with_label(label);
    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), radio);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
    gtk_widget_set_sensitive(item, enabled);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* A menu item hosting a slider. GTK delivers the pointer events to the menu
 * item's own window, so the slider inside never sees them: forward them.
 * Returning TRUE also keeps the menu open while dragging. */
static gboolean forward_to_slider(GtkWidget *item, GdkEvent *event, gpointer slider) {
    (void)item;
    gtk_widget_event(GTK_WIDGET(slider), event);
    return TRUE;
}

static void find_scale(GtkWidget *w, gpointer found) {
    if (*(GtkWidget **)found) return;
    if (GTK_IS_SCALE(w)) *(GtkWidget **)found = w;
    else if (GTK_IS_CONTAINER(w)) gtk_container_foreach(GTK_CONTAINER(w), find_scale, found);
}

static void add_slider_item(GtkWidget *menu, GtkWidget *content) {
    GtkWidget *holder = gtk_menu_item_new(), *slider = NULL;
    gtk_container_add(GTK_CONTAINER(holder), content);
    find_scale(content, &slider);
    const char *events[] = {"button-press-event", "button-release-event", "motion-notify-event"};
    for (size_t i = 0; slider && i < G_N_ELEMENTS(events); i++)
        g_signal_connect(holder, events[i], G_CALLBACK(forward_to_slider), slider);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), holder);
}

static void add_separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static GtkWidget *add_submenu(GtkWidget *menu, const char *label, int enabled) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_widget_set_sensitive(item, enabled);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return sub;
}

static void show_accel(GtkWidget *item, guint key, GdkModifierType mods) {
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(item));
    if (GTK_IS_ACCEL_LABEL(label)) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(label), key, mods);
}

static void on_choice(GtkCheckMenuItem *item, gpointer data) {
    void (*cb)(const char *) = data;
    if (gtk_check_menu_item_get_active(item))
        cb(g_object_get_data(G_OBJECT(item), "value"));
}

static GtkWidget *add_choice(GtkWidget *menu, const char *label, const char *value, int active,
                             int enabled, void (*cb)(const char *)) {
    GtkWidget *item = add_check(menu, label, active, 1, enabled);
    g_object_set_data_full(G_OBJECT(item), "value", g_strdup(value), g_free);
    g_signal_connect(item, "toggled", G_CALLBACK(on_choice), cb);
    return item;
}

GtkWidget *app_build_area_menu(void) {
    GtkWidget *menu = gtk_menu_new();
    int enabled = !PHASE_BUSY(recording_phase_now()) && !region_is_selecting();
    GtkWidget *whole = add_check(menu, "Whole Screen", App.area.mode == AREA_WHOLE_SCREEN, 1, enabled);
    g_signal_connect_swapped(whole, "activate", G_CALLBACK(select_whole_screen), NULL);
    GtkWidget *region = add_check(menu, "Select Area…", App.area.mode == AREA_REGION, 1, enabled);
    g_signal_connect_swapped(region, "activate", G_CALLBACK(select_area), NULL);
    GdkDisplay *d = gdk_display_get_default();
    if (gdk_display_get_n_monitors(d) > 1) {
        add_separator(menu);
        for (int i = 0; i < gdk_display_get_n_monitors(d); i++) {
            const char *name = monitor_name(gdk_display_get_monitor(d, i));
            char *label = g_strdup_printf("Whole Screen — %s", name);
            int active = App.area.mode == AREA_DISPLAY && strcmp(App.area.monitor, name) == 0;
            add_choice(menu, label, name, active, enabled, select_display);
            g_free(label);
        }
    }
    gtk_widget_show_all(menu);
    return menu;
}

static void select_camera_name(const char *name) { camera_select(name); }
static void select_audio_name(const char *name) {
    if (!PHASE_BUSY(recording_phase_now())) audio_select(name);
}

static void select_shape_name(const char *name) {
    Prefs.shape = shape_from_name(name);
    prefs_save();
    app_resize(window_frame().w);
}

static void select_size(const char *width) { app_resize(g_ascii_strtod(width, NULL)); }
static void increase_size(void) { app_resize(window_frame().w * SIZE_STEP); }
static void decrease_size(void) { app_resize(window_frame().w / SIZE_STEP); }

/* ------------------------------------------------------------ transparency */

static guint8 *LayerPixels;
static size_t LayerSize;

static void ensure_layer_pixels(size_t size) {
    if (size <= LayerSize) return;
    LayerPixels = g_realloc(LayerPixels, size);
    LayerSize = size;
}

/* Choose After Recording: the layer is the whole movie, transparent except
 * for the opaque camera where its window is, and wholly transparent while
 * the camera is hidden or stopped. It runs for the whole recording. */
static void push_project_layer(writer *w, rect monitor, rect region) {
    int out_w, out_h;
    const camera_frame *f = camera_current_frame();
    if (!writer_output_size(w, &out_w, &out_h)) return;
    if (!f || !App.visible || camera_state() != CAMERA_RUNNING) {
        writer_push_layer(w, NULL, 0);
        return;
    }
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, out_w);
    ensure_layer_pixels((size_t)stride * out_h);
    cairo_surface_t *s = cairo_image_surface_create_for_data(LayerPixels, CAIRO_FORMAT_ARGB32, out_w, out_h, stride);
    cairo_t *cr = cairo_create(s);
    camera_layer l = camera_layer_map(window_frame(), monitor, region, out_w, out_h, Prefs.shape);
    camera_layer_draw(cr, l, 0, 0, f, Prefs.mirrored,
                      center_stage_now(l.frame.w, l.frame.h, Prefs.shape, Prefs.mirrored), background_now(f));
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    writer_push_layer(w, LayerPixels, stride);
}

/* While recording, keep the camera copy in the movie in step with the
 * window: position, size, shape, mirroring and the latest frame. Opaque
 * camera over a translucent preview, or the project layer (Choose After
 * Recording). */
static gboolean layer_tick(gpointer data) {
    (void)data;
    rect monitor, region;
    writer *w = recording_layer_target(&monitor, &region);
    if (!w) {
        LayerTimer = 0;
        return G_SOURCE_REMOVE;
    }
    if (writer_layer_track(w)) {
        push_project_layer(w, monitor, region);
        return G_SOURCE_CONTINUE;
    }
    int out_w, out_h, x, y, cw, ch;
    const camera_frame *f = camera_current_frame();
    int needed = f && camera_layer_needed(Prefs.transparency, Prefs.saved_transparency == SAVED_PREVIEW,
                                          App.visible, camera_state() == CAMERA_RUNNING);
    if (!needed || !writer_output_size(w, &out_w, &out_h)) {
        writer_push_camera(w, NULL, 0, 0, 0, 0, 0, 0);
        return G_SOURCE_CONTINUE;
    }
    camera_layer l = camera_layer_map(window_frame(), monitor, region, out_w, out_h, Prefs.shape);
    if (!camera_layer_clip(l, out_w, out_h, &x, &y, &cw, &ch)) {
        writer_push_camera(w, NULL, 0, 0, 0, 0, 0, 0);   /* Outside the recorded area. */
        return G_SOURCE_CONTINUE;
    }
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, cw);
    ensure_layer_pixels((size_t)stride * ch);
    cairo_surface_t *s = cairo_image_surface_create_for_data(LayerPixels, CAIRO_FORMAT_ARGB32, cw, ch, stride);
    cairo_t *cr = cairo_create(s);
    camera_layer_draw(cr, l, x, y, f, Prefs.mirrored,
                      center_stage_now(l.frame.w, l.frame.h, Prefs.shape, Prefs.mirrored), background_now(f));
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    writer_push_camera(w, LayerPixels, cw, ch, stride, x, y, 1);
    return G_SOURCE_CONTINUE;
}

static void set_transparency(double value) {
    Prefs.transparency = transparency_normalize(value);
    prefs_save();
    gtk_widget_set_opacity(App.cam, 1 - Prefs.transparency);
}

static void reset_transparency(void) { set_transparency(0); }

static void transparency_slider(GtkRange *r, gpointer label) {
    char text[16];
    snprintf(text, sizeof(text), "%.0f%%", gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(label), text);
    set_transparency(gtk_range_get_value(r) / 100);
}

static void select_saved_transparency(const char *name) {
    Prefs.saved_transparency = saved_transparency_parse(name, 0, 0);
    prefs_save();
}

/* The project being recorded is not the last recording yet: skipped. */
static void adjust_last_recording(void) {
    char *dir = project_dir(), *latest = project_latest(dir, recording_active_project());
    if (latest) later_dialog_open(latest);
    g_free(latest);
    g_free(dir);
}

static int has_project(void) {
    char *dir = project_dir(), *latest = project_latest(dir, recording_active_project());
    int found = latest != NULL;
    g_free(latest);
    g_free(dir);
    return found;
}

/* Applied when a recording starts, like the Recording Format. */
static void build_saved_menu(GtkWidget *menu) {
    static const char *titles[SAVED_COUNT] = {"Same as Preview", "Opaque", "Choose After Recording"};
    for (int i = 0; i < SAVED_COUNT; i++)
        add_choice(menu, titles[i], SavedTransparencyNames[i], Prefs.saved_transparency == (saved_transparency)i,
                   i != SAVED_LATER || export_available(), select_saved_transparency);
    if (!export_available()) add_item(menu, "Choose After Recording needs FFV1 (gst-libav)", NULL, 0);
}

static void build_transparency_menu(GtkWidget *menu) {
    char text[64];
    if (recording_transparency_locked()) {
        /* The export needs the T_A this recording started with. */
        snprintf(text, sizeof(text), "Transparency %.0f%% (kept while recording)", Prefs.transparency * 100);
        add_item(menu, text, NULL, 0);
    } else {
        GtkWidget *fixed = gtk_fixed_new();
        gtk_widget_set_size_request(fixed, 300, 64);
        GtkWidget *caption = gtk_label_new("Transparency");
        gtk_label_set_xalign(GTK_LABEL(caption), 0);
        gtk_widget_set_size_request(caption, 205, 16);
        gtk_fixed_put(GTK_FIXED(fixed), caption, 14, 9);
        snprintf(text, sizeof(text), "%.0f%%", Prefs.transparency * 100);
        GtkWidget *value = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(value), 1);
        gtk_widget_set_size_request(value, 60, 16);
        gtk_fixed_put(GTK_FIXED(fixed), value, 226, 9);
        GtkWidget *slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, TRANSPARENCY_MAX * 100, 1);
        gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
        gtk_range_set_value(GTK_RANGE(slider), Prefs.transparency * 100);
        gtk_widget_set_size_request(slider, 276, 26);
        atk_object_set_name(gtk_widget_get_accessible(slider), "Camera transparency in percent");
        g_signal_connect(slider, "value-changed", G_CALLBACK(transparency_slider), value);
        gtk_fixed_put(GTK_FIXED(fixed), slider, 12, 33);
        add_slider_item(menu, fixed);
    }
    add_separator(menu);
    build_saved_menu(add_submenu(menu, "Saved Video", 1));
    add_item(menu, "Adjust Last Recording Transparency…", adjust_last_recording, has_project());
    add_item(menu, "Reset Transparency to 0%", reset_transparency, !recording_transparency_locked());
}

/* Linux addition: separates the person from the room. Choosing Image without one picked yet asks for
 * the picture first; every change goes through background_apply_prefs() so the worker starts or stops. */

static void set_background(background_mode mode) {
    Prefs.background_mode = mode;
    prefs_save();
    background_apply_prefs();
    overlay_queue_draw();
}

static void choose_background_image(void) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Choose Background Image", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images (PNG, JPEG)");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        prefs_set_string(&Prefs.background_image, path);
        g_free(path);
        set_background(BG_IMAGE);
    }
    gtk_widget_destroy(dialog);
}

static void choose_background_color(void) {
    GtkWidget *dialog = gtk_color_chooser_dialog_new("Choose Background Color", NULL);
    double rgb[3];
    if (background_color_parse(Prefs.background_color, rgb))
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &(GdkRGBA){rgb[0], rgb[1], rgb[2], 1});
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &c);
        char text[8];
        snprintf(text, sizeof(text), "#%02x%02x%02x", (int)lround(c.red * 255), (int)lround(c.green * 255),
                 (int)lround(c.blue * 255));
        prefs_set_string(&Prefs.background_color, text);
        set_background(BG_COLOR);
    }
    gtk_widget_destroy(dialog);
}

static void select_background(const char *name) {
    background_mode mode = background_mode_parse(name);
    if (mode == BG_IMAGE && !Prefs.background_image) choose_background_image();   /* Needs an image first. */
    else set_background(mode);
}

/* Furniture the model mistakes for you goes away once YouCam has seen the room without you. */
#define LEARN_ROOM_DELAY_S 3   /* Time to step out of view before the room is learned. */

/* The countdown window: shows the seconds left, then waits for the worker; Cancel closes it. */
static struct { GtkWidget *dialog; guint timer; int left; } Learn;

static void close_countdown(void) {
    if (Learn.timer) g_source_remove(Learn.timer);
    Learn.timer = 0;
    g_clear_pointer(&Learn.dialog, gtk_widget_destroy);
}

static void room_learned(void) {
    if (!Learn.dialog) return;   /* Cancelled: the room is still saved, just not announced. */
    close_countdown();
    app_show_message("Empty room learned", "You can come back now.", GTK_MESSAGE_INFO);
}

static gboolean countdown_tick(gpointer unused) {
    (void)unused;
    if (--Learn.left > 0) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(Learn.dialog), "%d", Learn.left);
        return G_SOURCE_CONTINUE;
    }
    Learn.timer = 0;
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(Learn.dialog), "Looking at the empty room…");
    background_learn_room(room_learned);
    return G_SOURCE_REMOVE;
}

static void learn_room(void) {
    GtkWidget *ask = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s",
        "After OK, step out of the camera view before the countdown ends. YouCam then looks at the "
        "empty room for about a second and tells you when you can come back.\n\n"
        "Keep the camera still afterwards; learn again if it moves or the light changes a lot.");
    int ok = gtk_dialog_run(GTK_DIALOG(ask)) == GTK_RESPONSE_OK;
    gtk_widget_destroy(ask);
    if (!ok || Learn.dialog) return;
    Learn.left = LEARN_ROOM_DELAY_S;
    Learn.dialog = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_CANCEL, "Step out of view");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(Learn.dialog), "%d", Learn.left);
    gtk_window_set_keep_above(GTK_WINDOW(Learn.dialog), TRUE);
    g_signal_connect(Learn.dialog, "response", G_CALLBACK(close_countdown), NULL);
    gtk_widget_show(Learn.dialog);
    Learn.timer = g_timeout_add_seconds(1, countdown_tick, NULL);
}

static void forget_room(void) {
    background_forget_room();
}

static void build_background_menu(GtkWidget *menu) {
    if (!background_available()) {
        add_item(menu, "Background removal unavailable", NULL, 0);
        return;
    }
    for (int m = 0; m < BG_COUNT; m++)
        add_choice(menu, BackgroundTitles[m], BackgroundNames[m], Prefs.background_mode == (background_mode)m, 1,
                   select_background);
    add_separator(menu);
    add_item(menu, "Choose Image…", choose_background_image, 1);
    add_item(menu, "Choose Color…", choose_background_color, 1);
    add_separator(menu);
    int on = Prefs.background_mode != BG_OFF;
    add_item(menu, "Learn Empty Room…", learn_room, on);
    add_item(menu, "Forget Empty Room", forget_room, on && background_has_room());
}

static void toggle_mirror(void) {
    Prefs.mirrored = !Prefs.mirrored;
    prefs_save();
    overlay_queue_draw();
}

static void toggle_center_stage(void) {
    Prefs.center_stage = !Prefs.center_stage;
    prefs_save();
    if (!Prefs.center_stage) center_stage_reset();
}

static void reset_position(void) {
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    set_camera_frame(overlay_initial(window_frame().w, Prefs.shape, monitor_workarea(m ? m : camera_monitor())));
    app_save_geometry();
}

static void retry_camera(void) {
    if (!App.visible) toggle_visibility();
    else if (!App.smoke) camera_retry();
}

static void show_help(void) { app_show_message("Your camera, without the window.", HelpText, GTK_MESSAGE_INFO); }

static void open_sound_settings(void) {
    if (!g_spawn_command_line_async("gnome-control-center sound", NULL))
        g_spawn_command_line_async("pavucontrol", NULL);
}

/* Linux addition: the file format of the next recordings. */
static void select_container(const char *ext) {
    g_free(Prefs.container);
    Prefs.container = g_strdup(container_for(ext)->ext);
    prefs_save();
}

static void build_format_menu(GtkWidget *menu) {
    const char *current = container_for(Prefs.container)->ext;
    for (int i = 0; i < CONTAINER_COUNT; i++)
        add_choice(menu, Containers[i].label, Containers[i].ext,
                   strcmp(current, Containers[i].ext) == 0, 1, select_container);
    gtk_widget_show_all(menu);
}

/* Linux addition: applied when a recording starts, so it is locked while recording. */
static void select_noise_level(const char *value) {
    Prefs.noise_reduction = noise_reduction_clamp(atoi(value));
    prefs_save();
}

static void build_noise_menu(GtkWidget *menu) {
    static const char *labels[] = {"Off", "Low", "Moderate", "High", "Very High"};
    for (int i = 0; i < NOISE_REDUCTION_LEVELS; i++) {
        char value[4];
        snprintf(value, sizeof(value), "%d", i);
        add_choice(menu, labels[i], value, Prefs.noise_reduction == i, 1, select_noise_level);
    }
    gtk_widget_show_all(menu);
}

/* Linux addition: the system microphone level, adjusted only while YouCam uses it. */
static void toggle_auto_mic_level(void) { audio_set_auto_mic_level(!Prefs.auto_mic_level); }

static void audio_menu_mapped(GtkWidget *w, gpointer d) { (void)w; (void)d; audio_monitor("menu", 1); }

static void build_audio_menu(GtkWidget *menu) {
    int busy = PHASE_BUSY(recording_phase_now());
    const char *sel = audio_selected_name();
    add_choice(menu, "No Audio", "none", !audio_has_audio(), !busy, select_audio_name);
    for (int i = 0; i < audio_device_count(); i++) {
        const char *name = audio_device_name(i);
        add_choice(menu, name, name, sel && strcmp(sel, name) == 0, !busy, select_audio_name);
    }
    add_separator(menu);
    add_slider_item(menu, audio_controls_new());
    add_separator(menu);
    build_noise_menu(add_submenu(menu, "Noise Reduction", !busy));
    GtkWidget *agc = add_check(menu, "Automatic Microphone Level", Prefs.auto_mic_level, 0, 1);
    g_signal_connect_swapped(agc, "activate", G_CALLBACK(toggle_auto_mic_level), NULL);
    add_item(menu, "Sound Settings…", open_sound_settings, 1);
    g_signal_connect(menu, "map", G_CALLBACK(audio_menu_mapped), NULL);
}

GtkWidget *app_build_menu(void) {
    GtkWidget *menu = gtk_menu_new();
    recording_phase phase = recording_phase_now();
    int busy = PHASE_BUSY(phase), selecting = region_is_selecting();

    add_item(menu, "YouCam", NULL, 0);
    add_item(menu, camera_menu_title(), NULL, 0);
    add_item(menu, recording_status(), NULL, 0);
    char *area = g_strdup_printf("Area: %s", app_area_name());
    add_item(menu, area, NULL, 0);
    g_free(area);
    add_separator(menu);

    GtkWidget *start = add_item(menu, busy ? "Stop Recording" : "Start Recording", app_toggle_recording,
                                phase != PHASE_FINISHING && !selecting);
    show_accel(start, GDK_KEY_r, GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK);
    if (phase == PHASE_RECORDING || phase == PHASE_PAUSED) {
        GtkWidget *pause = add_item(menu, phase == PHASE_PAUSED ? "Resume Recording" : "Pause Recording",
                                    app_toggle_pause, 1);
        show_accel(pause, GDK_KEY_p, GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK);
    }
    GtkWidget *area_item = gtk_menu_item_new_with_label("Recording Area");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(area_item), app_build_area_menu());
    gtk_widget_set_sensitive(area_item, !busy && !selecting);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), area_item);
    build_format_menu(add_submenu(menu, "Recording Format", !busy));
    add_item(menu, App.panel_visible ? "Hide Recording Panel" : "Show Recording Panel", app_toggle_panel, 1);
    if (!hotkeys_available()) add_item(menu, "Global shortcuts unavailable", NULL, 0);
    if (recording_last_path()) add_item(menu, "Show Last Recording in Files", reveal_recording, 1);
    add_separator(menu);

    add_item(menu, App.visible ? "Hide Camera" : "Show Camera", toggle_visibility, 1);
    GtkWidget *cameras = add_submenu(menu, "Camera", 1);
    if (!camera_device_count()) add_item(cameras, "No cameras available", NULL, 0);
    const char *cam = camera_selected_name();
    for (int i = 0; i < camera_device_count(); i++) {
        const char *name = camera_device_name(i);
        add_choice(cameras, name, name, cam && strcmp(cam, name) == 0, 1, select_camera_name);
    }
    build_audio_menu(add_submenu(menu, "Audio Input", 1));

    GtkWidget *shapes = add_submenu(menu, "Shape", 1);
    for (int s = 0; s < SHAPE_COUNT; s++)
        add_choice(shapes, shape_title(s), shape_name(s), Prefs.shape == (camera_shape)s, 1, select_shape_name);
    GtkWidget *sizes = add_submenu(menu, "Size", 1);
    double width = window_frame().w;
    for (size_t i = 0; i < G_N_ELEMENTS(Sizes); i++) {
        char value[16];
        snprintf(value, sizeof(value), "%d", Sizes[i].width);
        add_choice(sizes, Sizes[i].name, value, fabs(width - Sizes[i].width) < 1, 1, select_size);
    }
    add_separator(sizes);
    add_item(sizes, "Increase Size", increase_size, 1);
    add_item(sizes, "Decrease Size", decrease_size, 1);
    build_transparency_menu(add_submenu(menu, "Transparency", 1));
    build_background_menu(add_submenu(menu, "Background", 1));
    GtkWidget *mirror = add_check(menu, "Mirror Camera", Prefs.mirrored, 0, 1);
    g_signal_connect_swapped(mirror, "activate", G_CALLBACK(toggle_mirror), NULL);
    GtkWidget *stage = add_check(menu, "Center Stage (Follow Face)", Prefs.center_stage, 0, 1);
    g_signal_connect_swapped(stage, "activate", G_CALLBACK(toggle_center_stage), NULL);
    add_item(menu, "Move to Bottom Right", reset_position, 1);
    add_separator(menu);
    add_item(menu, "Retry Camera", retry_camera, 1);
    add_item(menu, "How to Use YouCam", show_help, 1);
    add_separator(menu);
    add_item(menu, "Quit YouCam", app_quit, 1);
    gtk_widget_show_all(menu);
    return menu;
}

static gboolean destroy_menu(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void menu_closed(GtkMenuShell *menu, gpointer data) {
    (void)data;
    audio_monitor("menu", 0);
    g_idle_add(destroy_menu, menu);   /* Activation handlers run first. */
}

void app_popup_menu(GtkWidget *menu, GdkEvent *event) {
    gtk_menu_attach_to_widget(GTK_MENU(menu), App.cam, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(menu_closed), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

static void status_icon_clicked(int x, int y) {
    (void)x; (void)y;
    app_popup_menu(app_build_menu(), NULL);
}

/* --------------------------------------------------------------- lifecycle */

static void prepare_for_sleep(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                              const char *signal, GVariant *params, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)data;
    gboolean sleeping;
    g_variant_get(params, "(b)", &sleeping);
    if (sleeping) recording_stop();
}

/* Developer check (--snapshot DIR): render the camera and the panel to PNG
 * files, without capturing the desktop, then quit. */
static char *SnapshotDir;

static void render(GtkWidget *w, const char *name) {
    int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(w, cr);
    char *path = g_build_filename(SnapshotDir, name, NULL);
    cairo_surface_write_to_png(s, path);
    g_print("snapshot: %s (%dx%d)\n", path, width, height);
    g_free(path);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

static gboolean take_snapshot(gpointer data) {
    (void)data;
    render(App.cam, "camera.png");
    render(App.panel, "panel.png");
    if (later_dialog_window()) render(later_dialog_window(), "transparency.png");
    GtkWidget *menu = app_build_menu();   /* Exercise the full menu too. */
    g_print("menu: %d items\n", (int)g_list_length(gtk_container_get_children(GTK_CONTAINER(menu))));
    gtk_widget_destroy(menu);
    app_quit();
    return G_SOURCE_REMOVE;
}

void app_snapshot(const char *dir) {
    SnapshotDir = g_strdup(dir);
    app_toggle_panel();
    const char *project = g_getenv("YOUCAM_SNAPSHOT_PROJECT");   /* Developer check of the stop dialog. */
    if (project) later_dialog_open(project);
    g_timeout_add_seconds(4, take_snapshot, NULL);
}

static gboolean smoke_quit(gpointer data) {
    (void)data;
    app_quit();
    return G_SOURCE_REMOVE;
}

void app_startup(GtkApplication *gapp, int smoke) {
    App.gapp = gapp;
    App.smoke = smoke;
    App.visible = 1;
    g_application_hold(G_APPLICATION(gapp));
    prefs_load();
    App.area = prefs_area_validate(Prefs.area, gdk_display_get_default());
    if (!smoke) portal_register_app();

    camera_init(camera_changed, overlay_queue_draw);
    center_stage_init(overlay_animate);
    background_apply_prefs();
    audio_init(audio_changed, microphone_lost);
    recording_init(update_ui, recording_error);
    recording_set_project_handlers(project_ready, recording_notice);
    char *pending = project_dir();   /* Stale exports' .part files, never the Desktop. */
    project_clean_parts(pending);
    g_free(pending);
    App.cam = overlay_create();
    set_camera_frame(restored_frame());
    gtk_widget_set_opacity(App.cam, 1 - Prefs.transparency);
    App.panel = panel_create();
    area_overlay_set_on_change(area_dragged);
    if (!smoke) hotkeys_init(app_toggle_recording, app_toggle_pause);
    status_icon_init(status_icon_clicked);
    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(monitors_changed), NULL);
    GDBusConnection *system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (system) g_dbus_connection_signal_subscribe(system, "org.freedesktop.login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep", "/org/freedesktop/login1", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, prepare_for_sleep, NULL, NULL);

    gtk_widget_show(App.cam);
    refresh_area_overlay();
    update_ui();
    if (smoke) {
        g_print("Screen capture: %s\n", mutter_cast_available()
                    ? "through Mutter, without YouCam's panel and guides (Capture Exclusion extension)"
                    : "through the desktop portal (asked when recording starts)");
        g_print("Microphone: %s\n", audio_name());
        g_timeout_add_seconds(SMOKE_TEST_S, smoke_quit, NULL);
    } else {
        camera_show();
    }
}

/* Quitting during a recording first finishes the movie. */
void app_quit(void) {
    if (PHASE_BUSY(recording_phase_now()) && !App.quitting) {
        App.quitting = 1;
        recording_stop();
        return;
    }
    if (App.save_timer) save_now(NULL);
    region_cancel();
    later_dialog_shutdown();   /* A running export is cancelled; the project stays. */
    area_overlay_hide();
    hotkeys_shutdown();
    audio_shutdown();
    camera_shutdown();
    if (App.panel) gtk_widget_hide(App.panel);
    g_application_release(G_APPLICATION(App.gapp));
    g_application_quit(G_APPLICATION(App.gapp));
}
