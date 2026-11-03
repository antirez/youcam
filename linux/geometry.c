/* Overlay and recording-area geometry (OverlayGeometry.swift and
 * RecordingAreaCorner.swift). Pure functions, covered by tests.c. */

#include <math.h>
#include <string.h>
#include "youcam.h"

static const char *ShapeNames[SHAPE_COUNT] = {"circle", "square", "rectangle"};
static const char *ShapeTitles[SHAPE_COUNT] = {"Circle", "Square", "Rectangle (16:9)"};

const char *shape_name(camera_shape s) { return ShapeNames[s]; }
const char *shape_title(camera_shape s) { return ShapeTitles[s]; }
double shape_aspect(camera_shape s) { return s == SHAPE_RECTANGLE ? 16.0 / 9.0 : 1.0; }

camera_shape shape_from_name(const char *name) {
    for (int i = 0; name && i < SHAPE_COUNT; i++)
        if (strcmp(name, ShapeNames[i]) == 0) return i;
    return SHAPE_CIRCLE;
}

/* The camera never gets smaller than usable, larger than 960 points, or
 * larger than the screen it lives on. */
void overlay_size(double width, camera_shape s, rect screen, double *w, double *h) {
    double aspect = shape_aspect(s);
    double limit = fmin(OVERLAY_MAX_WIDTH, fmin(screen.w, screen.h * aspect));
    double safe = isfinite(width) ? width : OVERLAY_DEFAULT_WIDTH;
    *w = fmin(fmax(safe, OVERLAY_MIN_WIDTH), limit);
    *h = *w / aspect;
}

rect overlay_constrain(rect f, rect screen) {
    f.x = fmax(screen.x, fmin(f.x, screen.x + screen.w - f.w));
    f.y = fmax(screen.y, fmin(f.y, screen.y + screen.h - f.h));
    return f;
}

/* Keep the top-left corner still when changing size or aspect ratio. */
rect overlay_resized(rect frame, double width, camera_shape s, rect screen) {
    rect r = {frame.x, frame.y, 0, 0};
    overlay_size(width, s, screen, &r.w, &r.h);
    return overlay_constrain(r, screen);
}

/* Bottom right corner, with a margin. */
rect overlay_initial(double width, camera_shape s, rect screen) {
    rect r;
    overlay_size(width, s, screen, &r.w, &r.h);
    r.x = screen.x + screen.w - r.w - OVERLAY_EDGE_MARGIN;
    r.y = screen.y + screen.h - r.h - OVERLAY_EDGE_MARGIN;
    return overlay_constrain(r, screen);
}

double rect_intersection_area(rect a, rect b) {
    double w = fmin(a.x + a.w, b.x + b.w) - fmax(a.x, b.x);
    double h = fmin(a.y + a.h, b.y + b.h) - fmax(a.y, b.y);
    return (w > 0 && h > 0) ? w * h : 0;
}

int rect_contains(rect o, rect i) {
    return i.x >= o.x && i.y >= o.y && i.x + i.w <= o.x + o.w && i.y + i.h <= o.y + o.h;
}

int rect_equal(rect a, rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ---------------------------------------------------------- area corners */

static const char *CornerTitles[CORNER_COUNT] = {"Top left", "Top right", "Bottom left", "Bottom right"};

const char *corner_title(area_corner c) { return CornerTitles[c]; }
static int corner_is_left(area_corner c) { return c == CORNER_TOP_LEFT || c == CORNER_BOTTOM_LEFT; }
static int corner_is_top(area_corner c) { return c == CORNER_TOP_LEFT || c == CORNER_TOP_RIGHT; }

void corner_point(area_corner c, rect r, double *x, double *y) {
    *x = corner_is_left(c) ? r.x : r.x + r.w;
    *y = corner_is_top(c) ? r.y : r.y + r.h;
}

/* Keep the opposite corner fixed, stay on this display, and retain the
 * picker's 40-point minimum size. Align the moving edges to screen pixels. */
rect corner_resized(area_corner c, rect r, double px, double py, rect screen, double scale) {
    double x = screen.x + round((px - screen.x) * scale) / scale;
    double y = screen.y + round((py - screen.y) * scale) / scale;
    double max_x = r.x + r.w, max_y = r.y + r.h;
    double sx2 = screen.x + screen.w, sy2 = screen.y + screen.h;
    int left = corner_is_left(c), top = corner_is_top(c);

    double min_x = left ? fmax(screen.x, fmin(max_x - AREA_MIN_SIZE, x)) : r.x;
    double new_max_x = left ? max_x : fmin(sx2, fmax(r.x + AREA_MIN_SIZE, x));
    double min_y = top ? fmax(screen.y, fmin(max_y - AREA_MIN_SIZE, y)) : r.y;
    double new_max_y = top ? max_y : fmin(sy2, fmax(r.y + AREA_MIN_SIZE, y));
    return (rect){min_x, min_y, new_max_x - min_x, new_max_y - min_y};
}
