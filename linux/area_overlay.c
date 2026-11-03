/* Recording area guide (RecordingAreaOverlay.swift): the rest of the monitor
 * is dimmed, the selected area stays clear, and four white dots on its corners
 * resize it before recording.
 *
 * The shade lets every click through (empty input shape); only the dots take
 * the mouse. The guide sits outside the selected area, so cropping already
 * keeps it out of a region recording. */

#include <math.h>
#include "youcam.h"

#define HANDLE_SIZE 24
#define DIM_ALPHA 0.4

static struct {
    GtkWidget *shade;
    GtkWidget *handles[CORNER_COUNT];
    GdkMonitor *monitor;
    rect local;                 /* Monitor-local selection. */
    int editable;
    int dragging;
    area_corner drag_corner;
    double grab_dx, grab_dy;
    void (*on_change)(GdkMonitor *m, rect local);
} O = {.editable = 1};

void area_overlay_set_on_change(void (*cb)(GdkMonitor *m, rect local)) { O.on_change = cb; }

static rect global_selection(void) {
    rect m = monitor_rect(O.monitor);
    return (rect){m.x + O.local.x, m.y + O.local.y, O.local.w, O.local.h};
}

static GtkWidget *overlay_window(const char *title) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *win = GTK_WINDOW(w);
    gtk_window_set_role(win, CAPTURE_HIDDEN_ROLE);   /* Kept out of recordings. */
    gtk_window_set_title(win, title);
    gtk_window_set_decorated(win, FALSE);
    gtk_window_set_keep_above(win, TRUE);
    gtk_window_set_skip_taskbar_hint(win, TRUE);
    gtk_window_set_skip_pager_hint(win, TRUE);
    gtk_window_set_accept_focus(win, FALSE);
    gtk_window_set_focus_on_map(win, FALSE);
    gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_stick(win);
    gtk_widget_set_app_paintable(w, TRUE);
    GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(w));
    if (visual) gtk_widget_set_visual(w, visual);
    g_signal_connect(w, "delete-event", G_CALLBACK(gtk_true), NULL);
    return w;
}

static gboolean draw_shade(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    /* The shell may keep the shade below its top bar: draw the selection
     * where the window really is, in line with the corner dots. */
    rect m = monitor_rect(O.monitor);
    int wx, wy;
    gtk_window_get_position(GTK_WINDOW(w), &wx, &wy);
    double sx = m.x + O.local.x - wx, sy = m.y + O.local.y - wy;
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_rectangle(cr, sx, sy, O.local.w, O.local.h);
    cairo_set_source_rgba(cr, 0, 0, 0, DIM_ALPHA);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, sx - 0.5, sy - 0.5, O.local.w + 1, O.local.h + 1);
    cairo_stroke(cr);
    return TRUE;
}

static gboolean window_moved(GtkWidget *w, GdkEventConfigure *ev, gpointer data) {
    (void)ev; (void)data;
    gtk_widget_queue_draw(w);
    return FALSE;
}

/* The dot sits on its corner even when the window could not: a handle on the
 * monitor edge is kept inside it, by us and by the window manager. */
static void dot_center(GtkWidget *w, area_corner c, double *x, double *y) {
    int wx, wy;
    corner_point(c, global_selection(), x, y);
    gtk_window_get_position(GTK_WINDOW(w), &wx, &wy);
    *x -= wx;
    *y -= wy;
}

static gboolean draw_handle(GtkWidget *w, cairo_t *cr, gpointer data) {
    double cx, cy;
    dot_center(w, GPOINTER_TO_INT(data), &cx, &cy);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    double r = (HANDLE_SIZE - 12) / 2.0;
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    return TRUE;
}

static void update_frames(void) {
    rect m = monitor_rect(O.monitor);
    gtk_window_move(GTK_WINDOW(O.shade), (int)m.x, (int)m.y);
    gtk_window_resize(GTK_WINDOW(O.shade), (int)m.w, (int)m.h);
    gtk_widget_queue_draw(O.shade);
    rect sel = global_selection();
    for (int c = 0; c < CORNER_COUNT; c++) {
        double x, y;
        corner_point(c, sel, &x, &y);
        double hx = CLAMP(round(x - HANDLE_SIZE / 2.0), m.x, m.x + m.w - HANDLE_SIZE);
        double hy = CLAMP(round(y - HANDLE_SIZE / 2.0), m.y, m.y + m.h - HANDLE_SIZE);
        gtk_window_move(GTK_WINDOW(O.handles[c]), (int)hx, (int)hy);
        gtk_widget_queue_draw(O.handles[c]);
    }
}

static void update_handle_visibility(void) {
    for (int c = 0; c < CORNER_COUNT; c++) {
        if (!O.handles[c]) continue;
        if (O.editable) gtk_widget_show(O.handles[c]);
        else gtk_widget_hide(O.handles[c]);
    }
    if (!O.editable) O.dragging = 0;
}

static gboolean handle_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    if (ev->button != GDK_BUTTON_PRIMARY || !O.editable) return TRUE;
    O.dragging = 1;
    O.drag_corner = GPOINTER_TO_INT(data);
    double cx, cy;
    dot_center(w, O.drag_corner, &cx, &cy);
    O.grab_dx = ev->x - cx;   /* Off-center grabs must not jump. */
    O.grab_dy = ev->y - cy;
    return TRUE;
}

static void drag_to(double root_x, double root_y) {
    if (!O.dragging || !O.editable || !O.monitor) return;
    rect m = monitor_rect(O.monitor);
    double scale = gdk_monitor_get_scale_factor(O.monitor);
    rect r = corner_resized(O.drag_corner, global_selection(), root_x - O.grab_dx, root_y - O.grab_dy, m, scale);
    rect local = {r.x - m.x, r.y - m.y, r.w, r.h};
    if (rect_equal(local, O.local)) return;
    O.local = local;
    update_frames();
    if (O.on_change) O.on_change(O.monitor, local);
}

static gboolean handle_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w; (void)data;
    drag_to(ev->x_root, ev->y_root);
    return TRUE;
}

static gboolean handle_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w; (void)data;
    drag_to(ev->x_root, ev->y_root);
    O.dragging = 0;
    return TRUE;
}

static void handle_realize(GtkWidget *w, gpointer data) {
    (void)data;
    GdkCursor *cross = gdk_cursor_new_from_name(gtk_widget_get_display(w), "crosshair");
    gdk_window_set_cursor(gtk_widget_get_window(w), cross);
    if (cross) g_object_unref(cross);
}

void area_overlay_show(GdkMonitor *m, rect local) {
    O.monitor = m;
    O.local = local;
    if (!O.shade) {
        O.shade = overlay_window("YouCam Recording Area");
        g_signal_connect(O.shade, "draw", G_CALLBACK(draw_shade), NULL);
        /* GTK's own shape: one set on the GdkWindow at realize is reset, and
         * the shade then took every click, the camera's right click too. */
        cairo_region_t *empty = cairo_region_create();
        gtk_widget_input_shape_combine_region(O.shade, empty);
        cairo_region_destroy(empty);
        g_signal_connect(O.shade, "configure-event", G_CALLBACK(window_moved), NULL);
        gtk_widget_show(O.shade);
    }
    if (!O.handles[0]) {
        for (int c = 0; c < CORNER_COUNT; c++) {
            char *title = g_strdup_printf("YouCam Recording Area — %s", corner_title(c));
            GtkWidget *h = overlay_window(title);
            g_free(title);
            char *tip = g_strdup_printf("%s recording area corner", corner_title(c));
            atk_object_set_name(gtk_widget_get_accessible(h), tip);
            g_free(tip);
            gtk_widget_set_tooltip_text(h, "Drag to resize the recording area");
            gtk_window_set_default_size(GTK_WINDOW(h), HANDLE_SIZE, HANDLE_SIZE);
            gtk_widget_set_size_request(h, HANDLE_SIZE, HANDLE_SIZE);
            gtk_widget_add_events(h, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
            g_signal_connect(h, "draw", G_CALLBACK(draw_handle), GINT_TO_POINTER(c));
            g_signal_connect(h, "configure-event", G_CALLBACK(window_moved), NULL);
            g_signal_connect(h, "button-press-event", G_CALLBACK(handle_press), GINT_TO_POINTER(c));
            g_signal_connect(h, "motion-notify-event", G_CALLBACK(handle_motion), NULL);
            g_signal_connect(h, "button-release-event", G_CALLBACK(handle_release), NULL);
            g_signal_connect(h, "realize", G_CALLBACK(handle_realize), NULL);
            O.handles[c] = h;
        }
    }
    update_frames();
    update_handle_visibility();
}

void area_overlay_hide(void) {
    O.dragging = 0;
    for (int c = 0; c < CORNER_COUNT; c++) {
        if (O.handles[c]) gtk_widget_destroy(O.handles[c]);
        O.handles[c] = NULL;
    }
    if (O.shade) gtk_widget_destroy(O.shade);
    O.shade = NULL;
    O.monitor = NULL;
}

void area_overlay_set_editable(int editable) {
    if (O.editable == editable) return;
    O.editable = editable;
    update_handle_visibility();
}
