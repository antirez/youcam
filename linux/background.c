/* Background removal: the mode names, image helpers, the segmentation
 * worker thread and the layers camera_layer.c paints. */

#include <errno.h>
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"
#include "selfie_landscape.h"
#include "selfie_square.h"

const guint8 *background_model_bytes(seg_variant v, size_t *size) {
    *size = v == SEG_SQUARE ? selfie_square_ysm_len : selfie_landscape_ysm_len;
    return v == SEG_SQUARE ? selfie_square_ysm : selfie_landscape_ysm;
}

const char *const BackgroundNames[BG_COUNT] = {"off", "transparent", "blur", "image", "color"};
const char *const BackgroundTitles[BG_COUNT] = {"Off", "Transparent", "Blur", "Image", "Color"};

#define MASK_SMOOTHING 0.8   /* Weight of the newest mask: lower is steadier, higher follows faster. */
#define EDGE_LOW 0.35        /* Model alpha at or below this is background (the halo)... */
#define EDGE_HIGH 0.75       /* ...at or above this the person; smoothstep in between. */
#define BLUR_FACTOR 16       /* The blurred background is computed this many times smaller... */
#define BLUR_RADIUS 2        /* ...with a box blur of this radius, then scaled up smoothly. */
#define ROOM_MATCH_LOW 10.0  /* RGB distance to the empty room at or below this: room, even if the model says person... */
#define ROOM_MATCH_HIGH 20.0 /* ...at or above this the model decides; linear in between. Wider eats faces over dark TVs. */
#define ROOM_SURE_BELOW 0.05 /* Model alpha under this is sure room: it measures the exposure change since learning. */
#define ROOM_GAIN_MIN 0.5    /* An exposure change outside these is not believed. */
#define ROOM_GAIN_MAX 2.0
#define ROOM_LEARN_FRAMES 15 /* Model frames averaged into the empty room (~1 s). */
#define IMAGE_MAX_SIDE 1280  /* A background image is kept at most this long: a camera sized window. */

background_mode background_mode_parse(const char *name) {
    for (int i = 0; name && i < BG_COUNT; i++)
        if (strcmp(name, BackgroundNames[i]) == 0) return i;
    return BG_OFF;
}

int background_color_parse(const char *text, double rgb[3]) {
    unsigned v;
    char end;
    if (!text || strlen(text) != 7 || sscanf(text, "#%6x%c", &v, &end) != 1) return 0;
    for (int i = 0; i < 3; i++) rgb[i] = ((v >> (16 - 8 * i)) & 0xff) / 255.0;
    return 1;
}

static void frame_resize(camera_frame *f, int w, int h) {
    if (f->width == w && f->height == h && f->data) return;
    f->data = g_realloc(f->data, (size_t)w * h * 4);
    f->width = w;
    f->height = h;
    f->stride = w * 4;
}

/* Box-average shrink of a BGRx frame, to packed RGB (rgb = 1) or BGRx. */
static void shrink(const guint8 *bgrx, int w, int h, int stride, guint8 *out, int ow, int oh, int rgb) {
    int bpp = rgb ? 3 : 4;
    for (int oy = 0; oy < oh; oy++) {
        int y0 = oy * h / oh, y1 = MAX(y0 + 1, (oy + 1) * h / oh);
        for (int ox = 0; ox < ow; ox++) {
            int x0 = ox * w / ow, x1 = MAX(x0 + 1, (ox + 1) * w / ow);
            unsigned sum[3] = {0, 0, 0}, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int y = y0; y < y1; y++) {
                const guint8 *p = bgrx + (size_t)y * stride + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
            }
            guint8 *o = out + ((size_t)oy * ow + ox) * bpp;
            for (int c = 0; c < 3; c++) o[rgb ? 2 - c : c] = (guint8)((sum[c] + n / 2) / n);
            if (!rgb) o[3] = 255;
        }
    }
}

void frame_to_model(const guint8 *bgrx, int w, int h, int stride, guint8 *rgb, int mw, int mh) {
    shrink(bgrx, w, h, stride, rgb, mw, mh, 1);
}

void mask_smooth(guint8 *mask, const float *raw, int n, int has_previous) {
    for (int i = 0; i < n; i++) {
        double v = fmin(fmax(raw[i], 0), 1) * 255;
        if (has_previous) v = MASK_SMOOTHING * v + (1 - MASK_SMOOTHING) * mask[i];
        mask[i] = (guint8)lround(v);
    }
}

/* Furniture the model is sure is you (a white cabinet beside a white shirt)
 * looks the same as in the learned empty room: those pixels lose alpha.
 * The room is scaled per channel by the exposure change measured where the
 * model is sure there is no one. */
void mask_remove_room(float *raw, const guint8 *rgb, const guint8 *room, int n) {
    double now[3] = {0}, then[3] = {0}, gain[3];
    for (int i = 0; i < n; i++)
        if (raw[i] < ROOM_SURE_BELOW)
            for (int c = 0; c < 3; c++) { now[c] += rgb[3 * i + c]; then[c] += room[3 * i + c]; }
    for (int c = 0; c < 3; c++) gain[c] = then[c] > 0 ? fmin(fmax(now[c] / then[c], ROOM_GAIN_MIN), ROOM_GAIN_MAX) : 1;
    for (int i = 0; i < n; i++) {
        double d2 = 0;
        for (int c = 0; c < 3; c++) {
            double e = rgb[3 * i + c] - room[3 * i + c] * gain[c];
            d2 += e * e;
        }
        double t = (sqrt(d2) - ROOM_MATCH_LOW) / (ROOM_MATCH_HIGH - ROOM_MATCH_LOW);
        raw[i] *= (float)fmin(fmax(t, 0), 1);
    }
}

/* The upscaled model edge is a wide soft band where the room shows through:
 * a smoothstep narrows it to a clean, still antialiased edge. */
guint8 mask_edge(guint8 alpha) {
    static guint8 lut[256];
    static gsize ready;
    if (g_once_init_enter(&ready)) {
        for (int i = 0; i < 256; i++) {
            double t = fmin(fmax((i / 255.0 - EDGE_LOW) / (EDGE_HIGH - EDGE_LOW), 0), 1);
            lut[i] = (guint8)lround(t * t * (3 - 2 * t) * 255);
        }
        g_once_init_leave(&ready, 1);
    }
    return lut[alpha];
}

/* The camera frame with the mask (bilinear, half-pixel centers) as alpha:
 * premultiplied ARGB32, the person surface painted over the backdrop.
 * Plain per-pixel float math costs ~2-4 ms at 720p; the x taps could be
 * computed once per frame instead of once per row if a profile ever asks. */
void mask_compose(const camera_frame *f, const guint8 *mask, int mw, int mh, camera_frame *out) {
    frame_resize(out, f->width, f->height);
    for (int y = 0; y < f->height; y++) {
        double my = fmax(0, (y + 0.5) * mh / f->height - 0.5);
        int y0 = MIN((int)my, mh - 1), y1 = MIN(y0 + 1, mh - 1);
        double fy = my - y0;
        const guint8 *p = f->data + (size_t)y * f->stride, *r0 = mask + (size_t)y0 * mw, *r1 = mask + (size_t)y1 * mw;
        guint8 *o = out->data + (size_t)y * out->stride;
        for (int x = 0; x < f->width; x++, p += 4, o += 4) {
            double mx = fmax(0, (x + 0.5) * mw / f->width - 0.5);
            int x0 = MIN((int)mx, mw - 1), x1 = MIN(x0 + 1, mw - 1);
            double fx = mx - x0;
            double top = r0[x0] + (r0[x1] - r0[x0]) * fx, bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
            unsigned a = mask_edge((guint8)lround(top + (bottom - top) * fy));
            for (int c = 0; c < 3; c++) o[c] = (guint8)((p[c] * a + 127) / 255);
            o[3] = (guint8)a;
        }
    }
}

static void box_pass(const guint8 *in, guint8 *out, int w, int h, int dx, int dy) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
                    int sx = CLAMP(x + k * dx, 0, w - 1), sy = CLAMP(y + k * dy, 0, h - 1);
                    sum += in[((size_t)sy * w + sx) * 4 + c];
                }
                out[((size_t)y * w + x) * 4 + c] = (guint8)(sum / (2 * BLUR_RADIUS + 1));
            }
}

/* A small, blurred copy of the frame (BGRx), painted scaled up behind the person. */
void blur_small(const camera_frame *f, camera_frame *out) {
    int w = MAX(1, f->width / BLUR_FACTOR), h = MAX(1, f->height / BLUR_FACTOR);
    frame_resize(out, w, h);
    guint8 *tmp = g_malloc((size_t)w * h * 4);
    shrink(f->data, f->width, f->height, f->stride, out->data, w, h, 0);
    memcpy(tmp, out->data, (size_t)w * h * 4);   /* Keeps the 255 alpha bytes. */
    box_pass(out->data, tmp, w, h, 1, 0);
    box_pass(tmp, out->data, w, h, 0, 1);
    g_free(tmp);
}

/* A picture as it should be drawn: phones store photos sideways plus an
 * EXIF orientation, and a 12 MP image costs tens of milliseconds to paint,
 * so it is turned upright and shrunk once here instead of on every frame. */
GdkPixbuf *background_image_fit(GdkPixbuf *image) {
    GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(image);
    if (!upright) upright = g_object_ref(image);   /* Rotating ran out of memory: keep it sideways. */
    int w = gdk_pixbuf_get_width(upright), h = gdk_pixbuf_get_height(upright);
    if (MAX(w, h) <= IMAGE_MAX_SIDE) return upright;
    double scale = (double)IMAGE_MAX_SIDE / MAX(w, h);
    int sw = MAX(1, (int)lround(w * scale)), sh = MAX(1, (int)lround(h * scale));
    GdkPixbuf *small = gdk_pixbuf_scale_simple(upright, sw, sh, GDK_INTERP_BILINEAR);
    if (!small) return upright;   /* Out of memory: slow to paint, but still right. */
    g_object_unref(upright);
    return small;
}

/* ------------------------------------------------------------- worker --- */

#define SQUARE_BELOW_ASPECT 1.5   /* Cameras narrower than this use the 256x256 model. */
#define SEG_MAX_INPUT (256 * 256 * 3)

typedef struct { guint8 *mask; int w, h; } published_mask;
typedef enum { ROOM_KEEP, ROOM_RELOAD, ROOM_LEARN } room_request;

static struct {
    GMutex lock;
    GCond wake;
    GThread *thread;
    int quit, pending;            /* pending: input holds a frame the worker is running. */
    seg_variant variant;
    char *camera;                 /* Whose empty room applies; set by the main thread. */
    room_request room;            /* Main thread asks, the worker clears. */
    background_room_done room_done;
    guint8 input[SEG_MAX_INPUT];
    int available;                /* -1 not checked yet. */
    int model_w[SEG_VARIANTS], model_h[SEG_VARIANTS];
    /* Main thread only. */
    guint8 *mask;
    int mask_w, mask_h;
    background_layer layer;
    char *image_path;             /* The path B.layer.image was loaded from. */
} B = {.available = -1};

int background_available(void) {
    if (B.available >= 0) return B.available;
    B.available = 1;
    for (int v = 0; v < SEG_VARIANTS; v++) {
        size_t size;
        const guint8 *data = background_model_bytes(v, &size);
        GError *err = NULL;
        segment_model *m = segment_load(data, size, &err);
        if (!m) {
            g_printerr("youcam: background removal unavailable: %s\n", err->message);
            g_clear_error(&err);
            B.available = 0;
            return 0;
        }
        segment_input_size(m, &B.model_w[v], &B.model_h[v]);
        if ((size_t)B.model_w[v] * B.model_h[v] * 3 > SEG_MAX_INPUT) B.available = 0;
        segment_free(m);
    }
    return B.available;
}

static gboolean deliver_mask(gpointer data) {
    published_mask *p = data;
    if (Prefs.background_mode == BG_OFF) {
        g_free(p->mask);
    } else {
        g_free(B.mask);
        B.mask = p->mask;
        B.mask_w = p->w;
        B.mask_h = p->h;
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------- empty room --- */

/* One PNG per camera at model size; NULL camera means no file. */
static char *room_path(const char *camera) {
    if (!camera) return NULL;
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, camera, -1);
    char *name = g_strdup_printf("room-%s.png", sum);
    char *path = g_build_filename(g_get_user_data_dir(), "youcam", name, NULL);
    g_free(sum);
    g_free(name);
    return path;
}

static void room_save(const char *camera, const guint8 *rgb, int w, int h) {
    char *path = room_path(camera);
    if (!path) return;
    char *dir = g_path_get_dirname(path);
    GError *err = NULL;
    GdkPixbuf *p = gdk_pixbuf_new_from_data(rgb, GDK_COLORSPACE_RGB, FALSE, 8, w, h, w * 3, NULL, NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0 || !gdk_pixbuf_save(p, path, "png", &err, NULL)) {
        g_printerr("youcam: cannot save the empty room to %s: %s\n", path, err ? err->message : g_strerror(errno));
        g_clear_error(&err);
    }
    g_object_unref(p);
    g_free(dir);
    g_free(path);
}

/* Packed RGB, or NULL when none was learned or it was for another camera shape. */
static guint8 *room_load(const char *camera, int w, int h) {
    char *path = room_path(camera);
    GdkPixbuf *p = path && g_file_test(path, G_FILE_TEST_EXISTS) ? gdk_pixbuf_new_from_file(path, NULL) : NULL;
    guint8 *rgb = NULL;
    if (p && gdk_pixbuf_get_width(p) == w && gdk_pixbuf_get_height(p) == h && gdk_pixbuf_get_n_channels(p) == 3) {
        rgb = g_malloc((size_t)w * h * 3);
        const guint8 *px = gdk_pixbuf_read_pixels(p);
        for (int y = 0; y < h; y++)
            memcpy(rgb + (size_t)y * w * 3, px + (size_t)y * gdk_pixbuf_get_rowstride(p), (size_t)w * 3);
    } else if (p) {
        g_printerr("youcam: ignoring the empty room in %s, learned for another camera shape\n", path);
    }
    if (p) g_object_unref(p);
    g_free(path);
    return rgb;
}

static gboolean room_learned(gpointer unused) {
    (void)unused;
    background_room_done done = B.room_done;
    B.room_done = NULL;
    if (done) done();
    return G_SOURCE_REMOVE;
}

void background_set_camera(const char *name) {
    g_mutex_lock(&B.lock);
    if (g_strcmp0(name, B.camera) != 0) {
        prefs_set_string(&B.camera, name);
        B.room = ROOM_RELOAD;
    }
    g_mutex_unlock(&B.lock);
}

void background_learn_room(background_room_done done) {
    B.room_done = done;
    g_mutex_lock(&B.lock);
    B.room = ROOM_LEARN;
    g_mutex_unlock(&B.lock);
}

int background_has_room(void) {
    char *path = room_path(B.camera);
    int has = path && g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return has;
}

void background_forget_room(void) {
    char *path = room_path(B.camera);
    if (path && g_remove(path) != 0 && errno != ENOENT)
        g_printerr("youcam: cannot remove %s: %s\n", path, g_strerror(errno));
    g_free(path);
    g_mutex_lock(&B.lock);
    B.room = ROOM_RELOAD;
    g_mutex_unlock(&B.lock);
}

/* Runs the model on the latest frame the camera thread left in B.input. */
static gpointer worker(gpointer unused) {
    (void)unused;
    segment_model *model = NULL;
    int loaded = -1, has_previous = 0, n = 0, load_failed = 0;
    float *raw = NULL;
    guint8 *smoothed = NULL, *room = NULL;
    double *learn = NULL;
    int learned = 0, room_loaded = 0;
    g_mutex_lock(&B.lock);
    for (;;) {
        while (!B.pending && !B.quit) g_cond_wait(&B.wake, &B.lock);
        if (B.quit) break;
        seg_variant v = B.variant;
        room_request request = B.room;
        B.room = ROOM_KEEP;
        char *camera = g_strdup(B.camera);
        g_mutex_unlock(&B.lock);
        if ((int)v != loaded) {   /* First frame, or the camera changed shape. */
            size_t size;
            const guint8 *data = background_model_bytes(v, &size);
            segment_free(model);
            /* The bytes were checked by background_available(), but the
             * activations are a large allocation that may still fail. */
            model = segment_load(data, size, NULL);
            if (!model) {
                if (!load_failed) g_printerr("youcam: background removal model did not load, skipping frames\n");
                load_failed = 1;
                loaded = -1;   /* Retry on the next frame, never run a NULL model. */
                g_mutex_lock(&B.lock);
                B.pending = 0;
                continue;
            }
            loaded = v;
            has_previous = 0;
            n = B.model_w[v] * B.model_h[v];
            raw = g_renew(float, raw, n);
            smoothed = g_renew(guint8, smoothed, n);
            room_loaded = 0;
            learned = 0;   /* A half learned room of the old shape is dropped. */
        }
        int mw = B.model_w[v], mh = B.model_h[v];
        if (request == ROOM_LEARN) {
            learn = g_renew(double, learn, (size_t)3 * n);
            memset(learn, 0, sizeof(double) * 3 * (size_t)n);
            learned = 1;
        } else if (learned) {
            learned++;
        }
        if (learned) {
            for (int i = 0; i < 3 * n; i++) learn[i] += B.input[i];
            if (learned == ROOM_LEARN_FRAMES) {
                room = g_renew(guint8, room, (size_t)3 * n);
                for (int i = 0; i < 3 * n; i++) room[i] = (guint8)lround(learn[i] / learned);
                room_save(camera, room, mw, mh);
                learned = 0;
                room_loaded = 1;
                g_idle_add(room_learned, NULL);
            }
        } else if (request == ROOM_RELOAD || !room_loaded) {
            g_clear_pointer(&room, g_free);
            room = room_load(camera, mw, mh);
            room_loaded = 1;
        }
        g_free(camera);
        segment_run(model, B.input, raw);
        if (room && !learned) mask_remove_room(raw, B.input, room, n);
        mask_smooth(smoothed, raw, n, has_previous);
        has_previous = 1;
        published_mask *p = g_new(published_mask, 1);
        *p = (published_mask){g_memdup2(smoothed, n), B.model_w[v], B.model_h[v]};
        g_idle_add(deliver_mask, p);
        g_mutex_lock(&B.lock);
        B.pending = 0;
    }
    g_mutex_unlock(&B.lock);
    segment_free(model);
    g_free(raw);
    g_free(smoothed);
    g_free(room);
    g_free(learn);
    return NULL;
}

/* Camera thread, every frame. Never queues: while the worker is busy the
 * frame is skipped, so a slow model lowers the mask rate, not the preview's. */
void background_feed(const guint8 *bgrx, int w, int h, int stride) {
    if (Prefs.background_mode == BG_OFF) return;
    g_mutex_lock(&B.lock);
    if (B.thread && !B.pending) {
        seg_variant v = (double)w / h < SQUARE_BELOW_ASPECT ? SEG_SQUARE : SEG_LANDSCAPE;
        frame_to_model(bgrx, w, h, stride, B.input, B.model_w[v], B.model_h[v]);
        B.variant = v;
        B.pending = 1;
        g_cond_signal(&B.wake);
    }
    g_mutex_unlock(&B.lock);
}

/* B.thread changes under the lock because background_feed reads it on the
 * camera thread. Stopping joins the worker, which frees the model. */
void background_apply_prefs(void) {
    int wanted = Prefs.background_mode != BG_OFF && background_available();
    g_mutex_lock(&B.lock);
    GThread *stopping = NULL;
    if (wanted && !B.thread) {
        B.quit = 0;
        B.pending = 0;
        B.thread = g_thread_new("youcam-segment", worker, NULL);
    } else if (!wanted && B.thread) {
        B.quit = 1;
        g_cond_signal(&B.wake);
        stopping = B.thread;
        B.thread = NULL;
    }
    g_mutex_unlock(&B.lock);
    if (stopping) {
        g_thread_join(stopping);
        g_clear_pointer(&B.mask, g_free);
    }
}

/* The chosen image, loaded once per path; NULL (and one log line) if it cannot be read. */
static cairo_surface_t *background_image(void) {
    const char *path = Prefs.background_image;
    if (g_strcmp0(path, B.image_path) == 0) return B.layer.image;
    g_clear_pointer(&B.layer.image, cairo_surface_destroy);
    prefs_set_string(&B.image_path, path);
    GError *err = NULL;
    GdkPixbuf *loaded = path ? gdk_pixbuf_new_from_file(path, &err) : NULL;
    if (!loaded) {
        g_printerr("youcam: background image unavailable, using Blur: %s\n", err ? err->message : "none chosen");
        g_clear_error(&err);
        return NULL;
    }
    /* Converted to a cairo surface here, not by every paint. Drawn by hand
     * because gdk_cairo_surface_create_from_pixbuf wants a display. */
    GdkPixbuf *fitted = background_image_fit(loaded);
    B.layer.image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gdk_pixbuf_get_width(fitted),
                                               gdk_pixbuf_get_height(fitted));
    cairo_t *cr = cairo_create(B.layer.image);
    gdk_cairo_set_source_pixbuf(cr, fitted, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(fitted);
    g_object_unref(loaded);
    return B.layer.image;
}

/* Recomposed on every call (preview and recording layer), about 2-4 ms each
 * at 720p; worth caching per camera frame only if it ever shows up. */
const background_layer *background_now(const camera_frame *f) {
    background_mode mode = Prefs.background_mode;
    if (mode == BG_OFF || !B.mask || !f) return NULL;
    background_layer *l = &B.layer;
    mask_compose(f, B.mask, B.mask_w, B.mask_h, &l->person);
    l->mode = mode;
    if (mode == BG_IMAGE && !background_image()) l->mode = BG_BLUR;
    if (l->mode == BG_BLUR) blur_small(f, &l->blurred);
    if (l->mode == BG_COLOR && !background_color_parse(Prefs.background_color, l->rgb))
        background_color_parse(BACKGROUND_DEFAULT_COLOR, l->rgb);
    return l;
}
