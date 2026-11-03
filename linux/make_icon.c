/* App icon (Scripts/make-icon.swift): a rounded blue-violet tile with a white
 * video camera and a green light, drawn with cairo at each size.
 * Usage: make-icon <dir> <size>... -> <dir>/youcam-<size>.png */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <cairo.h>
#include <glib.h>

#define CANVAS 1024.0

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* The Swift drawing uses a bottom-left origin: flip y once here. */
static double fy(double y) { return CANVAS - y; }

static void draw(cairo_t *cr) {
    rounded(cr, 64, 64, 896, 896, 205);
    /* -45 degree gradient: from top left to bottom right. */
    cairo_pattern_t *g = cairo_pattern_create_linear(64, 64, 960, 960);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.23, 0.31, 0.95);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.48, 0.25, 0.88);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    cairo_set_source_rgb(cr, 1, 1, 1);
    rounded(cr, 220, fy(332 + 360), 405, 360, 85);
    cairo_fill(cr);
    cairo_move_to(cr, 658, fy(593));
    cairo_line_to(cr, 799, fy(667));
    cairo_curve_to(cr, 815, fy(676), 825, fy(666), 825, fy(649));
    cairo_line_to(cr, 825, fy(375));
    cairo_curve_to(cr, 825, fy(357), 815, fy(348), 799, fy(357));
    cairo_line_to(cr, 658, fy(431));
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.40, 0.95, 0.76);
    cairo_arc(cr, 290 + 35, fy(555 + 35), 35, 0, 2 * M_PI);
    cairo_fill(cr);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dir> <size>...\n", argv[0]);
        return 1;
    }
    g_mkdir_with_parents(argv[1], 0755);
    for (int i = 2; i < argc; i++) {
        int size = atoi(argv[i]);
        if (size <= 0) continue;
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t *cr = cairo_create(s);
        cairo_scale(cr, size / CANVAS, size / CANVAS);
        draw(cr);
        char *path = g_strdup_printf("%s/youcam-%d.png", argv[1], size);
        if (cairo_surface_write_to_png(s, path) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "could not write %s\n", path);
            return 1;
        }
        g_free(path);
        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }
    return 0;
}
