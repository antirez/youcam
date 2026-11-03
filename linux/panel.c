/* Recording panel (RecordingPanel.swift): a small dark floating window with
 * the area button, transport controls, timer, input gain and level meter.
 * Layout uses the same frames as the macOS view, flipped to a top-left
 * origin, inside a 300x156 panel. */

#include <math.h>
#include <string.h>
#include "youcam.h"

#define PANEL_W 300
#define PANEL_H 156
#define METER_SEGMENTS 40
#define CLIP_HOLD_US 1500000

/* Input gain + level meter (AudioControlView): one in the panel, one in
 * the Audio Input menu while it exists. */
typedef struct { GtkWidget *slider, *value, *meter, *source; } audio_view;

static struct {
    GtkWidget *win, *area, *area_label, *close, *play, *pause, *stop, *status;
    audio_view panel_audio, menu_audio;
    recording_phase shown_phase;
    gint64 clip_until;
    int updating_slider;
} P;

static const char *Css =
    ".youcam-panel label { color: rgba(235,235,245,0.6); font-size: 11px; }"
    ".youcam-panel .source { font-size: 10px; }"
    ".youcam-panel .recording { color: #ff453a; }"
    ".youcam-panel .paused { color: #ff9f0a; }"
    ".youcam-panel .clipping { color: #ff453a; }"
    ".youcam-panel button { min-height: 0; min-width: 0; padding: 2px 6px; }"
    ".youcam-panel .area-button label { color: white; }";

/* Rounded dark background; the window itself is transparent. */
static gboolean draw_background(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w), r = 12;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, width - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, width - r, height - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, height - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, 0.98);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    return FALSE;   /* Children draw on top. */
}

/* GtkFixed gives children their natural size: cap labels so long text is
 * ellipsized inside the frame instead of widening the panel. */
static GtkWidget *place(GtkWidget *fixed, GtkWidget *w, int x, int y, int width, int height) {
    if (GTK_IS_LABEL(w)) gtk_label_set_max_width_chars(GTK_LABEL(w), 1);
    gtk_widget_set_size_request(w, width, height);
    gtk_fixed_put(GTK_FIXED(fixed), w, x, y);
    return w;
}

static int is_clipping(void) { return g_get_monotonic_time() < P.clip_until; }

/* Red record dot; a play triangle when paused (RecordingStartCell). */
static gboolean draw_play(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double cx = gtk_widget_get_allocated_width(w) / 2.0, cy = gtk_widget_get_allocated_height(w) / 2.0;
    recording_phase phase = recording_phase_now();
    int enabled = gtk_widget_get_sensitive(P.play);
    if (phase == PHASE_PAUSED) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_move_to(cr, cx - 4, cy - 6);
        cairo_line_to(cr, cx + 6, cy);
        cairo_line_to(cr, cx - 4, cy + 6);
        cairo_close_path(cr);
        cairo_fill(cr);
        return TRUE;
    }
    /* The active circle is also a status indicator: keep it bright while
     * recording even though starting another recording is disabled. */
    if (phase == PHASE_RECORDING) cairo_set_source_rgba(cr, 1, 0.42, 0.45, 1);
    else cairo_set_source_rgba(cr, 0.82, 0.19, 0.23, enabled ? 1 : 0.35);
    cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
    cairo_fill(cr);
    return TRUE;
}

static gboolean draw_meter(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)data;
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    float db = audio_level_db(audio_current_level());
    double seg = (width - 42) / METER_SEGMENTS;
    int filled = (int)((fminf(0, db) + 60) / 60 * METER_SEGMENTS);
    for (int i = 0; i < METER_SEGMENTS; i++) {
        double threshold = -60 + (i + 1) * 60.0 / METER_SEGMENTS;
        double a = i < filled ? 1 : 0.2;
        if (threshold > -6) cairo_set_source_rgba(cr, 1, 0.27, 0.23, a);
        else if (threshold > -12) cairo_set_source_rgba(cr, 1, 0.84, 0.04, a);
        else cairo_set_source_rgba(cr, 0.19, 0.82, 0.35, a);
        cairo_rectangle(cr, i * seg, 3, fmax(1, seg - 2), height - 6);
        cairo_fill(cr);
    }
    char text[16];
    if (is_clipping()) snprintf(text, sizeof(text), "CLIP");
    else snprintf(text, sizeof(text), "%.0f", db);
    if (is_clipping()) cairo_set_source_rgb(cr, 1, 0.27, 0.23);
    else cairo_set_source_rgba(cr, 0.92, 0.92, 0.96, 0.6);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10);
    cairo_move_to(cr, width - 37, height - 5);
    cairo_show_text(cr, text);
    return TRUE;
}

static void set_class(GtkWidget *w, const char *cls, int on) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    if (on) gtk_style_context_add_class(ctx, cls);
    else gtk_style_context_remove_class(ctx, cls);
}

static void update_audio_view(audio_view *v) {
    if (!v->slider) return;
    P.updating_slider = 1;
    gtk_range_set_value(GTK_RANGE(v->slider), audio_gain_db());
    P.updating_slider = 0;
    gtk_widget_set_sensitive(v->slider, audio_has_audio());
    char value[16];
    snprintf(value, sizeof(value), "%+.0f dB", audio_gain_db());
    gtk_label_set_text(GTK_LABEL(v->value), value);
    const char *source = is_clipping() ? "Clipping — lower the input or microphone level" : audio_status();
    gtk_label_set_text(GTK_LABEL(v->source), source);
    gtk_widget_set_tooltip_text(v->source, source);
    set_class(v->source, "clipping", is_clipping());
    gtk_widget_queue_draw(v->meter);
}

void panel_update_audio(void) {
    if (audio_current_level().clipped) P.clip_until = g_get_monotonic_time() + CLIP_HOLD_US;
    update_audio_view(&P.panel_audio);
    update_audio_view(&P.menu_audio);
}

void panel_update(void) {
    if (!P.win) return;
    recording_phase phase = recording_phase_now();
    panel_buttons b = panel_buttons_for(phase, region_is_selecting());
    char *area = g_strdup_printf("%s ▾", app_area_name());
    gtk_label_set_text(GTK_LABEL(P.area_label), area);
    g_free(area);
    char *tip = g_strdup_printf("%s — %s", app_area_name(),
        PHASE_BUSY(phase) ? "Stop recording to change the area" : "Choose the recording area");
    gtk_widget_set_tooltip_text(P.area, tip);
    g_free(tip);
    gtk_widget_set_sensitive(P.area, b.area);

    const char *status = recording_status();
    gtk_label_set_text(GTK_LABEL(P.status), status);
    gtk_widget_set_tooltip_text(P.status, status);
    set_class(P.status, "recording", phase == PHASE_RECORDING);
    set_class(P.status, "paused", phase == PHASE_PAUSED);

    static const char *play_tips[] = {"Start recording", "Starting recording", "Recording",
                                      "Resume recording", "Saving recording"};
    gtk_widget_set_sensitive(P.play, b.play);
    gtk_widget_set_tooltip_text(P.play, play_tips[phase]);
    gtk_widget_set_sensitive(P.pause, b.pause);
    gtk_widget_set_sensitive(P.stop, b.stop);
    gtk_widget_set_tooltip_text(P.stop, phase == PHASE_STARTING ? "Cancel recording" : "Stop recording");
    if (P.shown_phase != phase) {
        P.shown_phase = phase;
        gtk_widget_queue_draw(P.play);
    }
    panel_update_audio();
}

static void on_play(GtkButton *b, gpointer d) { (void)b; (void)d; app_panel_play(); }
static void on_pause(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    if (recording_phase_now() == PHASE_RECORDING) recording_toggle_pause();
}
static void on_stop(GtkButton *b, gpointer d) { (void)b; (void)d; app_panel_stop(); }
static void on_close(GtkButton *b, gpointer d) { (void)b; (void)d; app_toggle_panel(); }

static void on_area(GtkButton *b, gpointer d) {
    (void)d;
    GtkWidget *menu = app_build_area_menu();
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, NULL);
}

static void on_slider(GtkRange *r, gpointer d) {
    (void)d;
    if (!P.updating_slider) audio_set_gain(gtk_range_get_value(r));
}

/* Builds the controls at the AudioControlView frames, flipped to a top-left
 * origin: compact (panel, 72 high) or full (menu, 110 high, with caption). */
static void build_audio_view(GtkWidget *fixed, audio_view *v, int top, int compact) {
    v->slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, GAIN_DB_MIN, GAIN_DB_MAX, 1);
    gtk_scale_set_draw_value(GTK_SCALE(v->slider), FALSE);
    gtk_widget_set_can_focus(v->slider, FALSE);
    atk_object_set_name(gtk_widget_get_accessible(v->slider), "Microphone input gain in decibels");
    g_signal_connect(v->slider, "value-changed", G_CALLBACK(on_slider), NULL);
    v->value = gtk_label_new("0 dB");
    gtk_label_set_xalign(GTK_LABEL(v->value), 1);
    v->meter = gtk_drawing_area_new();
    g_signal_connect(v->meter, "draw", G_CALLBACK(draw_meter), NULL);
    atk_object_set_name(gtk_widget_get_accessible(v->meter), "Microphone input level");
    v->source = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(v->source), "source");
    gtk_label_set_xalign(GTK_LABEL(v->source), 0);
    gtk_label_set_ellipsize(GTK_LABEL(v->source), PANGO_ELLIPSIZE_MIDDLE);
    if (compact) {
        place(fixed, v->slider, 12, top + 2, 214, 24);
        place(fixed, v->value, 232, top + 6, 53, 16);
        place(fixed, v->meter, 15, top + 29, 270, 18);
        place(fixed, v->source, 15, top + 53, 270, 14);
    } else {
        GtkWidget *caption = gtk_label_new("Input level");
        gtk_label_set_xalign(GTK_LABEL(caption), 0);
        place(fixed, caption, 14, top + 7, 140, 16);
        place(fixed, v->value, 205, top + 7, 80, 16);
        place(fixed, v->slider, 12, top + 29, 276, 24);
        place(fixed, v->meter, 15, top + 58, 270, 21);
        place(fixed, v->source, 15, top + 85, 270, 16);
    }
}

static void menu_audio_gone(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    memset(&P.menu_audio, 0, sizeof(P.menu_audio));
}

GtkWidget *audio_controls_new(void) {
    GtkWidget *fixed = gtk_fixed_new();
    gtk_widget_set_size_request(fixed, 300, 110);
    build_audio_view(fixed, &P.menu_audio, 0, 0);
    g_signal_connect(fixed, "destroy", G_CALLBACK(menu_audio_gone), NULL);
    update_audio_view(&P.menu_audio);
    return fixed;
}

/* Right-click (or Control-click) shows the full menu; dragging the
 * background moves the panel. */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    (void)d;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    if (ev->button == GDK_BUTTON_SECONDARY ||
        (ev->button == GDK_BUTTON_PRIMARY && (ev->state & GDK_CONTROL_MASK))) {
        app_popup_menu(app_build_menu(), (GdkEvent *)ev);
        return TRUE;
    }
    if (ev->button == GDK_BUTTON_PRIMARY) {
        gtk_window_begin_move_drag(GTK_WINDOW(w), ev->button, (int)ev->x_root, (int)ev->y_root, ev->time);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *icon_button(const char *icon, const char *label) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, label);
    atk_object_set_name(gtk_widget_get_accessible(b), label);
    gtk_widget_set_can_focus(b, FALSE);
    return b;
}

GtkWidget *panel_create(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, Css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);

    P.shown_phase = -1;
    P.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(P.win);
    gtk_window_set_role(w, CAPTURE_HIDDEN_ROLE);   /* Kept out of recordings. */
    gtk_window_set_title(w, "YouCam Recording Controls");
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_keep_above(w, TRUE);
    gtk_window_set_skip_taskbar_hint(w, TRUE);
    gtk_window_set_skip_pager_hint(w, TRUE);
    gtk_window_set_accept_focus(w, FALSE);
    gtk_window_set_focus_on_map(w, FALSE);
    gtk_window_set_resizable(w, FALSE);
    gtk_window_stick(w);
    gtk_widget_set_app_paintable(P.win, TRUE);
    GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(P.win));
    if (visual) gtk_widget_set_visual(P.win, visual);

    g_signal_connect(P.win, "draw", G_CALLBACK(draw_background), NULL);
    GtkWidget *bg = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(bg), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(bg), "youcam-panel");
    gtk_container_add(GTK_CONTAINER(P.win), bg);
    GtkWidget *fixed = gtk_fixed_new();
    gtk_widget_set_size_request(fixed, PANEL_W, PANEL_H);
    gtk_container_add(GTK_CONTAINER(bg), fixed);

    P.area = gtk_button_new();
    gtk_widget_set_can_focus(P.area, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(P.area), "area-button");
    GtkWidget *area_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(area_box), gtk_image_new_from_icon_name("view-fullscreen-symbolic",
                       GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    P.area_label = gtk_label_new("Area: Whole Screen ▾");
    gtk_label_set_ellipsize(GTK_LABEL(P.area_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(P.area_label), 1);
    gtk_label_set_xalign(GTK_LABEL(P.area_label), 0);
    gtk_box_pack_start(GTK_BOX(area_box), P.area_label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(P.area), area_box);
    atk_object_set_name(gtk_widget_get_accessible(P.area), "Recording area");
    place(fixed, P.area, 10, 10, 254, 26);
    P.close = place(fixed, icon_button("window-close-symbolic", "Hide recording panel"), 268, 10, 24, 24);
    gtk_button_set_relief(GTK_BUTTON(P.close), GTK_RELIEF_NONE);

    P.play = gtk_button_new();
    gtk_widget_set_can_focus(P.play, FALSE);
    GtkWidget *dot = gtk_drawing_area_new();
    g_signal_connect(dot, "draw", G_CALLBACK(draw_play), NULL);
    gtk_container_add(GTK_CONTAINER(P.play), dot);
    atk_object_set_name(gtk_widget_get_accessible(P.play), "Start recording");
    place(fixed, P.play, 12, 44, 30, 30);
    P.pause = place(fixed, icon_button("media-playback-pause-symbolic", "Pause recording"), 46, 44, 30, 30);
    P.stop = place(fixed, icon_button("media-playback-stop-symbolic", "Stop recording"), 80, 44, 30, 30);
    P.status = gtk_label_new("Ready to record");
    gtk_label_set_xalign(GTK_LABEL(P.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(P.status), PANGO_ELLIPSIZE_END);
    place(fixed, P.status, 120, 51, 168, 16);

    build_audio_view(fixed, &P.panel_audio, 79, 1);

    g_signal_connect(P.play, "clicked", G_CALLBACK(on_play), NULL);
    g_signal_connect(P.pause, "clicked", G_CALLBACK(on_pause), NULL);
    g_signal_connect(P.stop, "clicked", G_CALLBACK(on_stop), NULL);
    g_signal_connect(P.close, "clicked", G_CALLBACK(on_close), NULL);
    g_signal_connect(P.area, "clicked", G_CALLBACK(on_area), NULL);
    gtk_widget_add_events(P.win, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(P.win, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(P.win, "delete-event", G_CALLBACK(gtk_true), NULL);
    gtk_widget_show_all(bg);
    return P.win;
}
