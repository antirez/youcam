/* Opaque camera layer for recordings (RecordingCameraOverlay.swift).
 *
 * On Linux the portal captures the camera window like any other window. When
 * the preview is translucent but the user wants an opaque camera in the
 * movie, the writer composites an opaque copy of the camera exactly over the
 * captured translucent one. These functions map the camera window into
 * output pixels and draw that copy; they are pure and covered by tests.c. */

#include <math.h>
#include "youcam.h"

#define CAMERA_CORNER_RADIUS 14
#define HALO_PX 1   /* Grow the copy slightly so no translucent edge shows. */
#define CAMERA_ASPECT (16.0 / 9.0)

double transparency_normalize(double value) {
    return isfinite(value) ? fmin(TRANSPARENCY_MAX, fmax(0, value)) : 0;
}

/* The copy is needed only to hide a translucent camera the user does not
 * want in the movie; a saved transparency is already in the capture. */
int camera_layer_needed(double transparency, int apply_to_video, int visible, int running) {
    return visible && running && !apply_to_video && transparency_normalize(transparency) > 0;
}

/* window: camera window, global logical coordinates. monitor: recorded
 * monitor, global. region: monitor-local recorded area (the whole monitor
 * when not cropping). out_w x out_h: movie pixels. */
camera_layer camera_layer_map(rect window, rect monitor, rect region, int out_w, int out_h,
                              camera_shape shape) {
    camera_layer l;
    double sx = out_w / region.w, sy = out_h / region.h;
    l.frame = (rect){(window.x - monitor.x - region.x) * sx, (window.y - monitor.y - region.y) * sy,
                     window.w * sx, window.h * sy};
    l.radius = (shape == SHAPE_CIRCLE ? window.w / 2 : CAMERA_CORNER_RADIUS) * fmin(sx, sy);
    l.shape = shape;
    return l;
}

/* Part of the layer inside the movie, in whole pixels (with the halo). */
int camera_layer_clip(camera_layer l, int out_w, int out_h, int *x, int *y, int *w, int *h) {
    double x0 = fmax(0, floor(l.frame.x - HALO_PX)), y0 = fmax(0, floor(l.frame.y - HALO_PX));
    double x1 = fmin(out_w, ceil(l.frame.x + l.frame.w + HALO_PX));
    double y1 = fmin(out_h, ceil(l.frame.y + l.frame.h + HALO_PX));
    if (x1 - x0 < 2 || y1 - y0 < 2) return 0;
    *x = (int)x0; *y = (int)y0; *w = (int)(x1 - x0); *h = (int)(y1 - y0);
    return 1;
}

/* Draws the camera (aspect fill, mirrored if asked, shape-masked, or with
 * a backdrop behind the person when bg is given) into a
 * BGRA canvas whose top-left is at (cx, cy) in output pixels, with the
 * same Center Stage transform as the camera window. */
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, stage_transform t, const background_layer *bg) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -cx, -cy);
    rect r = {l.frame.x - HALO_PX, l.frame.y - HALO_PX, l.frame.w + 2 * HALO_PX, l.frame.h + 2 * HALO_PX};
    double rad = fmin(l.radius + HALO_PX, fmin(r.w, r.h) / 2);   /* Transparent too: the chosen shape. */
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);
    cairo_translate(cr, r.x, r.y);
    camera_paint_frame(cr, f, r.w, r.h, mirrored, t, bg);
    cairo_restore(cr);
}

/* The video canvas behind a w x h mask: 16:9, covering the mask (as the
 * preview layer bounds in CameraOverlay.swift). A circle or square sees the
 * whole sensor width, which leaves room to pan sideways. */
void stage_canvas(double w, double h, double *cw, double *ch) {
    if (w / h < CAMERA_ASPECT) {
        *ch = h;
        *cw = h * CAMERA_ASPECT;
    } else {
        *cw = w;
        *ch = w / CAMERA_ASPECT;
    }
}

/* Paints w x h pixels of any cairo format with the camera transform:
 * aspect fill into the 16:9 canvas centred on the mask, then the Center
 * Stage shift and zoom around the mask centre. The geometry always comes
 * from the camera frame f; src is f itself or a smaller copy of it (the
 * blurred background), stretched over f's area. Taking the scale from the
 * copy would misplace it, since its size is f's divided and truncated. */
static void paint_pixels(cairo_t *cr, const camera_frame *f, const camera_frame *src, cairo_format_t format,
                         double w, double h, int mirrored, stage_transform t) {
    double cw, ch;
    stage_canvas(w, h, &cw, &ch);
    cairo_save(cr);
    cairo_translate(cr, w / 2 + t.x * w, h / 2 + t.y * h);
    cairo_scale(cr, t.zoom, t.zoom);
    cairo_translate(cr, -cw / 2, -ch / 2);
    if (mirrored) {
        cairo_translate(cr, cw, 0);
        cairo_scale(cr, -1, 1);
    }
    double scale = fmax(cw / f->width, ch / f->height);
    cairo_translate(cr, (cw - f->width * scale) / 2, (ch - f->height * scale) / 2);
    cairo_scale(cr, scale, scale);
    cairo_scale(cr, f->width / (double)src->width, f->height / (double)src->height);
    cairo_surface_t *image = cairo_image_surface_create_for_data(src->data, format, src->width, src->height,
                                                                 src->stride);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);   /* No dark seams when scaled up. */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, src->width, src->height);
    cairo_fill(cr);
    cairo_surface_destroy(image);
    cairo_restore(cr);
}

/* A background image fills the mask (cover), still and unmirrored. */
static void paint_cover(cairo_t *cr, cairo_surface_t *image, double w, double h) {
    double iw = cairo_image_surface_get_width(image), ih = cairo_image_surface_get_height(image);
    double scale = fmax(w / iw, h / ih);
    cairo_save(cr);
    cairo_translate(cr, (w - iw * scale) / 2, (h - ih * scale) / 2);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);   /* The upscaled edge stays opaque. */
    cairo_paint(cr);
    cairo_restore(cr);
}

/* Paints a camera frame into a w x h mask, shared with the preview so both
 * show the same picture. With a background layer, paints the backdrop
 * first and the person, cut out by the mask, on top. */
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored,
                        stage_transform t, const background_layer *bg) {
    if (!bg) {
        paint_pixels(cr, f, f, CAIRO_FORMAT_RGB24, w, h, mirrored, t);
        return;
    }
    if (bg->mode == BG_COLOR) {
        cairo_set_source_rgb(cr, bg->rgb[0], bg->rgb[1], bg->rgb[2]);
        cairo_paint(cr);
    } else if (bg->mode == BG_IMAGE) {
        paint_cover(cr, bg->image, w, h);
    } else if (bg->mode == BG_BLUR) {
        paint_pixels(cr, f, &bg->blurred, CAIRO_FORMAT_RGB24, w, h, mirrored, t);
    }
    paint_pixels(cr, f, &bg->person, CAIRO_FORMAT_ARGB32, w, h, mirrored, t);
}
