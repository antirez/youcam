/* Choose After Recording (Linux addition): the window that finishes a
 * project. One frame from the middle of the recording is decoded once, as
 * two stills: the screen and the camera layer. Moving the slider only
 * repaints them with cairo, the layer at the opacity the export will use
 * (later_alpha), so it is instant. Save exports the movie to the Desktop
 * and deletes the project; Later and closing the window keep it for
 * "Adjust Last Recording Transparency…"; Discard deletes it after asking.
 * One window at a time. */

#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define PREVIEW_WIDTH 640
#define SPACING 10
#define BORDER 16
#define NOTE_CHARS 70

static struct {
    GtkWidget *win, *slider, *value, *progress, *error, *save, *later, *discard;
    char *project;
    project_info info;
    cairo_surface_t *screen, *layer;   /* PREVIEW_WIDTH wide; NULL when unreadable. */
    exporter *job;
} D;

static double chosen(void) { return D.slider ? gtk_range_get_value(GTK_RANGE(D.slider)) / 100 : 0; }

static void close_window(void) {
    if (D.job) export_cancel(D.job);   /* Deletes the .part; the project stays. */
    if (D.screen) cairo_surface_destroy(D.screen);
    if (D.layer) cairo_surface_destroy(D.layer);
    GtkWidget *win = D.win;
    g_free(D.project);
    memset(&D, 0, sizeof(D));
    if (win) gtk_widget_destroy(win);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e; (void)d;
    close_window();
    return TRUE;
}

/* The spec's still B (the layer over the screen) shown over A (the screen)
 * at opacity alpha is the layer itself painted at alpha over A. */
static gboolean draw_preview(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)w; (void)d;
    cairo_set_source_surface(cr, D.screen, 0, 0);
    cairo_paint(cr);
    cairo_set_source_surface(cr, D.layer, 0, 0);
    cairo_paint_with_alpha(cr, later_alpha(chosen(), D.info.t_a));
    return TRUE;
}

static void slider_moved(GtkRange *r, gpointer area) {
    char text[16];
    snprintf(text, sizeof(text), "%.0f%%", gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(D.value), text);
    gtk_widget_queue_draw(GTK_WIDGET(area));
}

static void set_busy(int busy) {
    if (D.slider) gtk_widget_set_sensitive(D.slider, !busy && later_max(D.info.t_a) > 0);
    if (D.save) gtk_widget_set_sensitive(D.save, !busy);
    if (D.later) gtk_widget_set_sensitive(D.later, !busy);
    gtk_widget_set_sensitive(D.discard, !busy);
    gtk_widget_set_visible(D.progress, busy);
    if (busy) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(D.progress), 0);
}

static void export_progress(double fraction, void *ud) {
    (void)ud;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(D.progress), fraction);
}

static void export_finished(const char *path, void *ud) {
    (void)ud;
    D.job = NULL;   /* The exporter freed itself before calling us. */
    if (g_unlink(D.project) != 0) g_printerr("youcam: could not delete the project %s\n", D.project);
    recording_set_last_path(path);
    close_window();
}

static void export_failed(const char *message, void *ud) {
    (void)ud;
    D.job = NULL;
    char *text = g_strdup_printf("The movie could not be saved: %s\nThe recording is kept: you can try again.", message);
    gtk_label_set_text(GTK_LABEL(D.error), text);
    gtk_widget_show(D.error);
    g_free(text);
    set_busy(0);
}

static void on_save(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GError *err = NULL;
    gtk_widget_hide(D.error);
    set_busy(1);
    D.job = export_start(D.project, &D.info, later_alpha(chosen(), D.info.t_a), recording_output_dir(),
                         (export_callbacks){export_progress, export_finished, export_failed, NULL}, &err);
    if (!D.job) {
        export_failed(err ? err->message : "the export could not start.", NULL);
        g_clear_error(&err);
    }
}

static void on_later(GtkButton *b, gpointer d) { (void)b; (void)d; close_window(); }

static void on_discard(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkWidget *ask = gtk_message_dialog_new(GTK_WINDOW(D.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                            GTK_BUTTONS_NONE, "Discard this recording?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask), "It is deleted and cannot be recovered.");
    gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL, "Discard", GTK_RESPONSE_ACCEPT, NULL);
    char *asked = g_strdup(D.project);
    int answer = gtk_dialog_run(GTK_DIALOG(ask));
    gtk_widget_destroy(ask);
    /* While asking, a new recording may have replaced the window's project:
     * delete the one the user was asked about, close only if still shown. */
    if (answer == GTK_RESPONSE_ACCEPT) {
        if (g_unlink(asked) != 0) g_printerr("youcam: could not delete %s\n", asked);
        if (D.project && strcmp(D.project, asked) == 0) close_window();
    }
    g_free(asked);
}

static GtkWidget *add_button(GtkWidget *box, const char *label, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, NULL);
    gtk_container_add(GTK_CONTAINER(box), b);
    return b;
}

static GtkWidget *add_label(GtkWidget *box, const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), NOTE_CHARS);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static void add_preview(GtkWidget *box) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, cairo_image_surface_get_width(D.screen), cairo_image_surface_get_height(D.screen));
    g_signal_connect(area, "draw", G_CALLBACK(draw_preview), NULL);
    gtk_box_pack_start(GTK_BOX(box), area, FALSE, FALSE, 0);

    double max = later_max(D.info.t_a);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Camera Transparency"), FALSE, FALSE, 0);
    /* A scale needs min < max: with nothing to choose (opaque preview, no
     * extension) it sits at 0, disabled (set_busy). */
    D.slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, max > 0 ? max * 100 : 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(D.slider), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(D.slider), 0);
    atk_object_set_name(gtk_widget_get_accessible(D.slider), "Camera transparency of the saved video in percent");
    gtk_box_pack_start(GTK_BOX(row), D.slider, TRUE, TRUE, 0);
    D.value = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(D.value), 4);
    gtk_box_pack_start(GTK_BOX(row), D.value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_signal_connect(D.slider, "value-changed", G_CALLBACK(slider_moved), area);
    gtk_range_set_value(GTK_RANGE(D.slider), round(fmin(D.info.preview, max) * 100));
    slider_moved(GTK_RANGE(D.slider), area);   /* The label, also when the value stayed 0. */
    if (D.info.t_a < 1) {
        char note[200];
        snprintf(note, sizeof(note), "Up to %.0f%%, the transparency the camera had on screen while recording.\n"
                 "With the YouCam Capture Exclusion extension active (log out and back in once) you can choose up to 80%%.",
                 max * 100);
        add_label(box, note);
    }
}

void later_dialog_open(const char *project) {
    if (g_strcmp0(project, recording_active_project()) == 0) {
        /* Still being written: Discard would delete the recording, Save export half of it. */
        g_printerr("youcam: %s is still being recorded\n", project);
        return;
    }
    if (D.win && (D.job || strcmp(D.project, project) == 0)) {
        gtk_window_present(GTK_WINDOW(D.win));   /* While saving, a newer project waits for the menu. */
        return;
    }
    close_window();   /* The project it showed stays in pending/. */
    D.project = g_strdup(project);
    GError *err = NULL;
    int readable = project_read_info(project, &D.info) &&
                   project_stills(project, PREVIEW_WIDTH, &D.screen, &D.layer, &err);
    if (!readable)
        g_printerr("youcam: cannot read the project %s: %s\n", project, err ? err->message : "no YouCam settings");
    g_clear_error(&err);

    D.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(D.win);
    gtk_window_set_title(w, "Camera Transparency");
    gtk_window_set_role(w, CAPTURE_HIDDEN_ROLE);   /* Out of a new recording, with the extension. */
    gtk_window_set_resizable(w, FALSE);
    gtk_window_set_position(w, GTK_WIN_POS_CENTER);
    g_signal_connect(D.win, "delete-event", G_CALLBACK(on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
    gtk_container_set_border_width(GTK_CONTAINER(box), BORDER);
    gtk_container_add(GTK_CONTAINER(D.win), box);
    if (readable) add_preview(box);
    else add_label(box, "This recording cannot be read. It can only be discarded.");
    D.error = add_label(box, "");
    D.progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), D.progress, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), SPACING);
    D.discard = add_button(buttons, "Discard…", G_CALLBACK(on_discard));
    if (readable) {
        D.later = add_button(buttons, "Later", G_CALLBACK(on_later));
        D.save = add_button(buttons, "Save", G_CALLBACK(on_save));
        gtk_style_context_add_class(gtk_widget_get_style_context(D.save), GTK_STYLE_CLASS_SUGGESTED_ACTION);
    }
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    gtk_widget_show_all(D.win);
    gtk_widget_hide(D.error);
    set_busy(0);
}

GtkWidget *later_dialog_window(void) { return D.win; }

void later_dialog_shutdown(void) { close_window(); }
