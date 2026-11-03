/* Region picker (RegionPicker.swift): every monitor is dimmed, the user drags
 * a rectangle on one of them; Escape cancels. Selections smaller than 40x40
 * are ignored so a stray click does not pick a useless area. */

#include <math.h>
#include "youcam.h"

#define DIM_ALPHA 0.4

typedef struct {
    GtkWidget *win;
    GdkMonitor *monitor;
    int dragging, has_selection;
    double ax, ay;
    rect sel;
} picker_window;

static struct {
    GPtrArray *windows;
    void (*done)(GdkMonitor *m, const rect *local);
} S;

int region_is_selecting(void) { return S.windows != NULL; }

typedef struct { GdkMonitor *m; rect r; int ok; void (*done)(GdkMonitor *, const rect *); } result;

/* Finish mouse handling before the picker windows are gone. */
static gboolean deliver(gpointer data) {
    result *res = data;
    res->done(res->ok ? res->m : NULL, res->ok ? &res->r : NULL);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static void finish(picker_window *pw) {
    result *res = g_new0(result, 1);
    res->done = S.done;
    if (pw) {
        res->ok = 1;
        res->m = pw->monitor;
        res->r = pw->sel;
    }
    GPtrArray *windows = S.windows;
    S.windows = NULL;
    for (guint i = 0; i < windows->len; i++) {
        picker_window *p = windows->pdata[i];
        gtk_widget_destroy(p->win);
        g_free(p);
    }
    g_ptr_array_free(windows, TRUE);
    g_idle_add(deliver, res);
}

void region_cancel(void) { if (S.windows) finish(NULL); }

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    picker_window *pw = data;
    double width = gtk_widget_get_allocated_width(w);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, DIM_ALPHA);
    cairo_paint(cr);
    if (pw->has_selection) {
        rect s = pw->sel;
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_rectangle(cr, s.x, s.y, s.w, s.h);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, s.x + 0.5, s.y + 0.5, s.w - 1, s.h - 1);
        cairo_stroke(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    char *msg = pw->has_selection
        ? g_strdup_printf("%d × %d · Release to select · Esc to cancel", (int)pw->sel.w, (int)pw->sel.h)
        : g_strdup("Drag to select a recording area · Esc to cancel");
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans Medium 18px");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, msg, -1);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (width - tw) / 2, 80 - th);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
    g_free(msg);
    return TRUE;
}

static void update_selection(picker_window *pw, double x, double y) {
    double w = gtk_widget_get_allocated_width(pw->win), h = gtk_widget_get_allocated_height(pw->win);
    double x0 = fmax(0, fmin(pw->ax, x)), y0 = fmax(0, fmin(pw->ay, y));
    double x1 = fmin(w, fmax(pw->ax, x)), y1 = fmin(h, fmax(pw->ay, y));
    pw->sel = (rect){x0, y0, x1 - x0, y1 - y0};
    pw->has_selection = 1;
    gtk_widget_queue_draw(pw->win);
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    picker_window *pw = data;
    if (ev->button != GDK_BUTTON_PRIMARY) return TRUE;
    pw->dragging = 1;
    pw->ax = ev->x;
    pw->ay = ev->y;
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    (void)w;
    picker_window *pw = data;
    if (pw->dragging) update_selection(pw, ev->x, ev->y);
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    (void)w;
    picker_window *pw = data;
    if (ev->button != GDK_BUTTON_PRIMARY || !pw->dragging) return TRUE;
    update_selection(pw, ev->x, ev->y);
    pw->dragging = 0;
    if (pw->sel.w >= AREA_MIN_SIZE && pw->sel.h >= AREA_MIN_SIZE) finish(pw);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w; (void)data;
    if (ev->keyval == GDK_KEY_Escape) finish(NULL);
    return TRUE;
}

static void on_realize(GtkWidget *w, gpointer data) {
    (void)data;
    GdkCursor *cross = gdk_cursor_new_from_name(gtk_widget_get_display(w), "crosshair");
    gdk_window_set_cursor(gtk_widget_get_window(w), cross);
    if (cross) g_object_unref(cross);
}

void region_select(void (*done)(GdkMonitor *m, const rect *local)) {
    if (S.windows) return;
    S.done = done;
    S.windows = g_ptr_array_new();
    GdkDisplay *d = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(d); i++) {
        picker_window *pw = g_new0(picker_window, 1);
        pw->monitor = gdk_display_get_monitor(d, i);
        pw->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        GtkWindow *win = GTK_WINDOW(pw->win);
        gtk_window_set_role(win, CAPTURE_HIDDEN_ROLE);   /* Kept out of recordings. */
        gtk_window_set_title(win, "Select Recording Area");
        gtk_window_set_decorated(win, FALSE);
        gtk_window_set_keep_above(win, TRUE);
        gtk_window_set_skip_taskbar_hint(win, TRUE);
        gtk_widget_set_app_paintable(pw->win, TRUE);
        GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(pw->win));
        if (visual) gtk_widget_set_visual(pw->win, visual);
        gtk_widget_add_events(pw->win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                       GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
        g_signal_connect(pw->win, "draw", G_CALLBACK(on_draw), pw);
        g_signal_connect(pw->win, "button-press-event", G_CALLBACK(on_press), pw);
        g_signal_connect(pw->win, "motion-notify-event", G_CALLBACK(on_motion), pw);
        g_signal_connect(pw->win, "button-release-event", G_CALLBACK(on_release), pw);
        g_signal_connect(pw->win, "key-press-event", G_CALLBACK(on_key), pw);
        g_signal_connect(pw->win, "realize", G_CALLBACK(on_realize), NULL);
        g_signal_connect(pw->win, "delete-event", G_CALLBACK(gtk_true), NULL);
        rect m = monitor_rect(pw->monitor);
        gtk_window_move(win, (int)m.x, (int)m.y);
        gtk_window_fullscreen_on_monitor(win, gtk_widget_get_screen(pw->win), i);
        g_ptr_array_add(S.windows, pw);
        gtk_widget_show_all(pw->win);
        gtk_window_present(win);
    }
}
