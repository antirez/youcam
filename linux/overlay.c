/* The floating camera (CameraOverlay.swift): a borderless window drawing the
 * camera inside a circle or rounded rectangle.
 *
 * Clicks outside the visible shape go through to whatever is below, like the
 * hitTest override on macOS: the X input shape is set to the shape itself.
 * The window never takes keyboard focus, so clicking the camera does not
 * steal focus from the app being recorded (a non-activating panel). */

#include <math.h>
#include "youcam.h"

#define CORNER_RADIUS 14
#define SCROLL_POINTS_PER_LINE 8   /* As AppKit's non-precise scroll deltas. */

static GtkWidget *Win;

static int is_circle(void) { return Prefs.shape == SHAPE_CIRCLE; }

static void shape_path(cairo_t *cr, double w, double h) {
    if (is_circle()) {
        cairo_arc(cr, w / 2, h / 2, fmin(w, h) / 2, 0, 2 * M_PI);
        return;
    }
    double r = CORNER_RADIUS;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* Row-by-row region matching the drawn shape, for the input shape. */
static cairo_region_t *shape_region(int w, int h) {
    cairo_region_t *region = cairo_region_create();
    double radius = is_circle() ? fmin(w, h) / 2.0 : CORNER_RADIUS;
    for (int y = 0; y < h; y++) {
        double cy = y + 0.5, inset = 0;
        double top = radius, bottom = h - radius;
        if (cy < top || cy > bottom) {
            double dy = cy < top ? top - cy : cy - bottom;
            inset = radius - sqrt(fmax(0, radius * radius - dy * dy));
        }
        if (is_circle()) {   /* Circle: radius-based rows across the whole height. */
            double dy = cy - h / 2.0;
            inset = w / 2.0 - sqrt(fmax(0, radius * radius - dy * dy));
        }
        int x0 = (int)ceil(inset), x1 = (int)floor(w - inset);
        if (x1 > x0) {
            cairo_rectangle_int_t row = {x0, y, x1 - x0, 1};
            cairo_region_union_rectangle(region, &row);
        }
    }
    return region;
}

void overlay_apply_shape(void) {
    GdkWindow *gw = gtk_widget_get_window(Win);
    if (!gw) return;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(Win), &w, &h);
    cairo_region_t *region = shape_region(w, h);
    gdk_window_input_shape_combine_region(gw, region, 0, 0);
    cairo_region_destroy(region);
}

void overlay_queue_draw(void) { if (Win) gtk_widget_queue_draw(Win); }

/* Redraws every display frame while a Center Stage move is animating. */
static guint AnimationTick;

static gboolean animation_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock; (void)data;
    gtk_widget_queue_draw(widget);
    if (center_stage_animating()) return G_SOURCE_CONTINUE;
    AnimationTick = 0;
    return G_SOURCE_REMOVE;
}

void overlay_animate(void) {
    if (Win && !AnimationTick) AnimationTick = gtk_widget_add_tick_callback(Win, animation_tick, NULL, NULL);
}

/* Choose After Recording with the Capture Exclusion extension: the camera
 * stays on screen but out of the screen track, so its transparency can be
 * anything later. The extension hides windows whose title starts with
 * CAPTURE_HIDDEN_ROLE and follows title changes (notify::title); the role
 * cannot change once the window is mapped. */
void overlay_set_capture_hidden(int hidden) {
    if (Win) gtk_window_set_title(GTK_WINDOW(Win), hidden ? CAPTURE_HIDDEN_ROLE " YouCam" : "YouCam");
}

static void draw_text(cairo_t *cr, const char *text, const char *font, double alpha,
                      double x, double y, double width, int max_lines) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(layout, -max_lines);
    cairo_set_source_rgba(cr, 1, 1, 1, alpha);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void draw_placeholder(cairo_t *cr, double w, double h, const char *title, const char *detail) {
    double width = w * (is_circle() ? 0.72 : 0.86), left = (w - width) / 2, center = h / 2;
    int compact = h < 160 || w < 200;
    if (!compact) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        GdkPixbuf *icon = gtk_icon_theme_load_icon(theme, "camera-video-symbolic", 26, 0, NULL);
        if (icon) {
            cairo_save(cr);
            cairo_push_group(cr);
            gdk_cairo_set_source_pixbuf(cr, icon, w / 2 - 13, center - 50);
            cairo_paint(cr);
            cairo_pattern_t *mask = cairo_pop_group(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
            cairo_mask(cr, mask);
            cairo_pattern_destroy(mask);
            cairo_restore(cr);
            g_object_unref(icon);
        }
        draw_text(cr, detail, "Sans 12", 0.65, left, center + 14, width, 3);
    }
    draw_text(cr, title, compact ? "Sans Bold 11" : "Sans Bold 15", 1,
              left, compact ? center - 8 : center - 17, width, 1);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)data;
    double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const char *title, *detail;
    int message = camera_message(&title, &detail);
    const camera_frame *f = message ? NULL : camera_current_frame();
    const background_layer *bg = f ? background_now(f) : NULL;
    shape_path(cr, w, h);   /* Transparent too: the person stays inside the chosen shape. */
    cairo_clip(cr);
    if (!bg || bg->mode != BG_TRANSPARENT) {
        cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
        cairo_paint(cr);
    }
    if (message) {
        draw_placeholder(cr, w, h, title, detail);
        return TRUE;
    }
    if (!f) return TRUE;
    camera_paint_frame(cr, f, w, h, Prefs.mirrored, center_stage_now(w, h, Prefs.shape, Prefs.mirrored), bg);
    return TRUE;
}

static gboolean on_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)data;
    if (ev->type != GDK_BUTTON_PRESS) return TRUE;
    int control = ev->state & GDK_CONTROL_MASK;
    if (ev->button == GDK_BUTTON_SECONDARY || (ev->button == GDK_BUTTON_PRIMARY && control)) {
        app_popup_menu(app_build_menu(), (GdkEvent *)ev);
    } else if (ev->button == GDK_BUTTON_PRIMARY) {
        gtk_window_begin_move_drag(GTK_WINDOW(widget), ev->button, (int)ev->x_root, (int)ev->y_root, ev->time);
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer data) {
    (void)data;
    double delta = 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        if (gdk_event_is_scroll_stop_event((GdkEvent *)ev)) return TRUE;
        delta = -ev->delta_y * SCROLL_POINTS_PER_LINE;
    } else if (ev->direction == GDK_SCROLL_UP) {
        delta = SCROLL_POINTS_PER_LINE;
    } else if (ev->direction == GDK_SCROLL_DOWN) {
        delta = -SCROLL_POINTS_PER_LINE;
    }
    if (delta != 0) {
        int w, h;
        gtk_window_get_size(GTK_WINDOW(widget), &w, &h);
        app_resize(w + delta);
    }
    return TRUE;
}

/* The window manager moved the window (drag): remember where. */
static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *ev, gpointer data) {
    (void)widget; (void)ev; (void)data;
    app_save_geometry();
    overlay_apply_shape();
    return FALSE;
}

static void on_realize(GtkWidget *widget, gpointer data) {
    (void)data;
    GdkDisplay *d = gtk_widget_get_display(widget);
    GdkCursor *hand = gdk_cursor_new_from_name(d, "grab");
    gdk_window_set_cursor(gtk_widget_get_window(widget), hand);
    if (hand) g_object_unref(hand);
    overlay_apply_shape();
}

GtkWidget *overlay_create(void) {
    Win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(Win);
    gtk_window_set_title(w, "YouCam");
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_keep_above(w, TRUE);
    gtk_window_set_skip_taskbar_hint(w, TRUE);
    gtk_window_set_skip_pager_hint(w, TRUE);
    gtk_window_set_accept_focus(w, FALSE);
    gtk_window_set_focus_on_map(w, FALSE);
    gtk_window_stick(w);
    gtk_widget_set_app_paintable(Win, TRUE);
    GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(Win));
    if (visual) gtk_widget_set_visual(Win, visual);
    gtk_widget_add_events(Win, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(Win, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(Win, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(Win, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(Win, "configure-event", G_CALLBACK(on_configure), NULL);
    g_signal_connect(Win, "realize", G_CALLBACK(on_realize), NULL);
    g_signal_connect(Win, "delete-event", G_CALLBACK(gtk_true), NULL);
    return Win;
}
