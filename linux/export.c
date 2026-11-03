/* Choose After Recording (Linux addition): projects and their export.
 *
 * A project is an MKV holding the screen (H.264, as any recording), the
 * camera layer at screen size (FFV1: lossless, keeps alpha) and the
 * microphone (AAC). Exporting lays the layer over the screen with opacity
 *
 *     alpha = 1 - t / T_A
 *
 * where T_A is the transparency the camera already has in the screen
 * track: 1 when the Capture Exclusion extension kept the camera window out
 * of it, the preview transparency otherwise. The camera then ends up
 * exactly t transparent: its opacity is alpha + (1 - alpha)(1 - T_A) = 1 - t.
 *
 * The settings travel in the MKV header (the WritingApp field), on disk
 * from the first second: GStreamer writes tags only when the file is
 * finished, so a killed recording would lose them. A copy goes into the
 * COMMENT tag for players and ffprobe.
 *
 * The dialog's preview comes from two stills of the same frame, decoded by
 * project_stills(); the export decodes both tracks, composites them, and
 * encodes with the recorder's own encoder chain (video_encoder_chain()) into
 * the container chosen when the recording started; the AAC is copied. */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define PROJECT_MAGIC "YouCam project;"
#define PROJECT_HEADER_BYTES 16384   /* WritingApp sits near the start (byte 288 in our files). */
#define KEY_T_A ";YOUCAM_T_A="
#define KEY_PREVIEW ";YOUCAM_PREVIEW_TRANSPARENCY="
#define KEY_CONTAINER ";YOUCAM_CONTAINER="
#define KEY_AUDIO ";YOUCAM_AUDIO="
#define EXT_CHARS "abcdefghijklmnopqrstuvwxyz0123456789"
#define PROGRESS_MS 200
#define STILL_TIMEOUT_S 5
#define PUBLISH_TRIES 100   /* Names taken between two tries: only a race, never a loop. */
/* The screen and the layer wait for each other at the compositor: queues
 * bounded by time only, since an FFV1 frame can be megabytes. */
#define TRACK_QUEUE "queue max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000"
/* output-corrupt=false: a damaged GOP is skipped instead of shown gray. */
#define SCREEN_DECODE "h264parse ! avdec_h264 output-corrupt=false ! videoconvert"
#define SCREEN_STILL "d.video_0 ! queue ! " SCREEN_DECODE " ! video/x-raw,format=BGRx"
#define LAYER_STILL "d.video_1 ! queue ! avdec_ffv1 ! videoconvert ! video/x-raw,format=BGRA"

double later_max(double t_a) {
    return isfinite(t_a) && t_a > 0 ? fmin(t_a, TRANSPARENCY_MAX) : 0;
}

/* T_A = 0 (opaque preview, no extension): the camera in the screen track
 * is already opaque and the slider only offers 0, so the layer is opaque. */
double later_alpha(double t, double t_a) {
    if (!isfinite(t_a) || t_a <= 0) return 1;
    if (!isfinite(t)) t = 0;
    return fmin(1, fmax(0, 1 - t / t_a));
}

int project_info_format(const project_info *p, char *buf, size_t len) {
    char t_a[G_ASCII_DTOSTR_BUF_SIZE], preview[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(t_a, sizeof(t_a), "%.3f", p->t_a);   /* Never a decimal comma, whatever the locale. */
    g_ascii_formatd(preview, sizeof(preview), "%.3f", p->preview);
    int n = snprintf(buf, len, PROJECT_MAGIC "YOUCAM_T_A=%s" KEY_PREVIEW "%s" KEY_CONTAINER "%s" KEY_AUDIO "%d;",
                     t_a, preview, container_for(p->container)->ext, p->has_audio ? 1 : 0);
    return n > 0 && (size_t)n < len;
}

/* The number after key, ended by ';', within [0, max]. */
static int parse_value(const char *text, const char *key, double max, double *out) {
    const char *at = strstr(text, key);
    if (!at) return 0;
    at += strlen(key);
    char *end;
    double v = g_ascii_strtod(at, &end);
    if (end == at || *end != ';' || !isfinite(v) || v < 0 || v > max) return 0;
    *out = v;
    return 1;
}

/* Any file in pending/ can be passed here, truncated or not ours: every
 * field is checked, and nothing is read past len. */
int project_info_parse(const guint8 *bytes, size_t len, project_info *out) {
    size_t magic = strlen(PROJECT_MAGIC);
    for (size_t i = 0; i + magic <= len; i++) {
        if (memcmp(bytes + i, PROJECT_MAGIC, magic) != 0) continue;
        char text[PROJECT_INFO_MAX];
        size_t n = MIN(len - i, sizeof(text) - 1);
        memcpy(text, bytes + i, n);
        text[n] = 0;   /* Binary MKV bytes after the field may hold a NUL earlier: strstr stops there. */
        project_info p = {0};
        const char *ext = strstr(text, KEY_CONTAINER), *audio = strstr(text, KEY_AUDIO);
        if (!parse_value(text, KEY_T_A, 1, &p.t_a) || !parse_value(text, KEY_PREVIEW, TRANSPARENCY_MAX, &p.preview) ||
            !ext || !audio)
            return 0;
        ext += strlen(KEY_CONTAINER);
        audio += strlen(KEY_AUDIO);
        size_t k = strspn(ext, EXT_CHARS);
        if (k == 0 || k >= sizeof(p.container) || ext[k] != ';' || (audio[0] != '0' && audio[0] != '1') ||
            audio[1] != ';')
            return 0;
        char name[sizeof(p.container)];
        memcpy(name, ext, k);
        name[k] = 0;
        g_strlcpy(p.container, container_for(name)->ext, sizeof(p.container));   /* Unknown: MOV. */
        p.has_audio = audio[0] == '1';
        *out = p;
        return 1;
    }
    return 0;
}

int project_read_info(const char *path, project_info *out) {
    FILE *f = g_fopen(path, "rb");
    if (!f) return 0;
    guint8 *head = g_malloc(PROJECT_HEADER_BYTES);
    size_t n = fread(head, 1, PROJECT_HEADER_BYTES, f);
    fclose(f);
    int ok = project_info_parse(head, n, out);
    g_free(head);
    return ok;
}

char *project_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "youcam", "pending", NULL);
}

/* <dir>/<the final movie's name>.youcam.mkv, never overwriting. */
char *project_new_path(const char *dir, time_t when) {
    char *base = recording_base_name(when);
    char *path = g_strdup_printf("%s/%s" PROJECT_SUFFIX, dir, base);
    for (int suffix = 2; g_file_test(path, G_FILE_TEST_EXISTS); suffix++) {
        g_free(path);
        path = g_strdup_printf("%s/%s-%d" PROJECT_SUFFIX, dir, base, suffix);
    }
    g_free(base);
    return path;
}

/* The menu opens the newest project; older ones wait, without a list.
 * 'exclude' (may be NULL) is the project being recorded right now. */
char *project_latest(const char *dir, const char *exclude) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    char *best = NULL;
    time_t best_time = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, PROJECT_SUFFIX)) continue;
        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_strcmp0(path, exclude) != 0 && g_stat(path, &st) == 0 && (!best || st.st_mtime > best_time)) {
            g_free(best);
            best = path;
            best_time = st.st_mtime;
        } else {
            g_free(path);
        }
    }
    g_dir_close(d);
    return best;
}

/* A .part left in pending/ by a crash is never useful. The Desktop is the
 * user's own folder: nothing there is touched. */
void project_clean_parts(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".part")) continue;
        char *path = g_build_filename(dir, name, NULL);
        if (g_unlink(path) != 0) g_printerr("youcam: could not remove %s\n", path);
        g_free(path);
    }
    g_dir_close(d);
}

/* <dir>/<project name without .youcam.mkv>.<ext>; an existing movie is
 * never overwritten: " (2)", " (3)"... before the extension. Caller frees. */
char *export_output_path(const char *dir, const char *project, const char *ext) {
    char *name = g_path_get_basename(project);
    if (g_str_has_suffix(name, PROJECT_SUFFIX)) name[strlen(name) - strlen(PROJECT_SUFFIX)] = 0;
    ext = container_for(ext)->ext;
    char *path = g_strdup_printf("%s/%s.%s", dir, name, ext);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path);
        path = g_strdup_printf("%s/%s (%d).%s", dir, name, n, ext);
    }
    g_free(name);
    return path;
}

static int has_factory(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    int found = f != NULL;
    if (f) gst_object_unref(f);
    return found;
}

/* The layer codec and the screen decoder come with gst-libav. Checked once. */
int export_available(void) {
    static int known, available;
    if (!known) {
        available = has_factory("avenc_ffv1") && has_factory("avdec_ffv1") && has_factory("avdec_h264");
        known = 1;
    }
    return available;
}

/* Prerolls one track of a project at 'at' (at < 0: the middle) with an
 * accurate seek. One pipeline per track, the others left unlinked: with
 * both video tracks linked the demuxer seeks by the FFV1 index and starts
 * the H.264 mid-GOP (a gray picture). */
static GstSample *still_at(const char *path, const char *track, gint64 at) {
    char *desc = g_strdup_printf("filesrc name=src ! matroskademux name=d %s ! appsink name=sink", track);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    if (!p) return NULL;
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src"), *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    g_object_set(src, "location", path, NULL);
    GstSample *sample = NULL;
    gint64 duration = 0;
    if (gst_element_set_state(p, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE &&
        gst_element_get_state(p, NULL, NULL, STILL_TIMEOUT_S * GST_SECOND) == GST_STATE_CHANGE_SUCCESS) {
        if (at < 0) at = gst_element_query_duration(p, GST_FORMAT_TIME, &duration) && duration > 0 ? duration / 2 : 0;
        if (gst_element_seek_simple(p, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, at) &&
            gst_element_get_state(p, NULL, NULL, STILL_TIMEOUT_S * GST_SECOND) == GST_STATE_CHANGE_SUCCESS)
            g_signal_emit_by_name(sink, "try-pull-preroll", (GstClockTime)(STILL_TIMEOUT_S * GST_SECOND), &sample);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(p);
    return sample;
}

/* A decoded BGRx/BGRA frame (rows packed, 4 bytes per pixel: cairo's own
 * layout on little-endian) as a cairo surface scaled to 'width'. */
static cairo_surface_t *sample_surface(GstSample *sample, cairo_format_t format, int width) {
    int w = 0, h = 0;
    GstStructure *s = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w <= 0 || h <= 0 || !gst_buffer_map(buf, &map, GST_MAP_READ)) return NULL;
    if (map.size < (gsize)w * h * 4) {
        gst_buffer_unmap(buf, &map);
        return NULL;
    }
    cairo_surface_t *full = cairo_image_surface_create(format, w, h);
    int stride = cairo_image_surface_get_stride(full);
    guint8 *dst = cairo_image_surface_get_data(full);
    for (int row = 0; row < h; row++)
        memcpy(dst + (size_t)row * stride, map.data + (size_t)row * w * 4, (size_t)w * 4);
    gst_buffer_unmap(buf, &map);
    cairo_surface_mark_dirty(full);
    int height = MAX(1, (int)lround((double)h * width / w));
    cairo_surface_t *out = cairo_image_surface_create(format, width, height);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, (double)width / w, (double)height / h);
    cairo_set_source_surface(cr, full, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(full);
    return out;
}

/* The screen at the middle, then the layer at that very frame's time. */
int project_stills(const char *path, int width, cairo_surface_t **screen, cairo_surface_t **layer, GError **err) {
    *screen = *layer = NULL;
    GstSample *a = still_at(path, SCREEN_STILL, -1);
    if (!a) a = still_at(path, SCREEN_STILL, 0);   /* Short, or killed: its duration is only a guess. */
    GstClockTime at = a ? GST_BUFFER_PTS(gst_sample_get_buffer(a)) : GST_CLOCK_TIME_NONE;
    GstSample *l = a ? still_at(path, LAYER_STILL, GST_CLOCK_TIME_IS_VALID(at) ? (gint64)at : 0) : NULL;
    if (a && l) {
        *screen = sample_surface(a, CAIRO_FORMAT_RGB24, width);
        *layer = sample_surface(l, CAIRO_FORMAT_ARGB32, width);
    }
    if (a) gst_sample_unref(a);
    if (l) gst_sample_unref(l);
    if (*screen && *layer) return 1;
    if (*screen) cairo_surface_destroy(*screen);
    if (*layer) cairo_surface_destroy(*layer);
    *screen = *layer = NULL;
    g_set_error(err, GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "The recording cannot be read.");
    return 0;
}

struct exporter {
    GstElement *pipeline, *comp, *demux;
    char *part, *path, *out_dir, *project;
    const char *ext;
    export_callbacks cb;
    guint poll;
    gint64 started;
};

static void export_free(exporter *e) {
    if (e->poll) g_source_remove(e->poll);
    GstBus *bus = gst_element_get_bus(e->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(e->pipeline, GST_STATE_NULL);   /* File sources only: safe on the main loop. */
    gst_object_unref(e->comp);
    gst_object_unref(e->demux);
    gst_object_unref(e->pipeline);
    g_free(e->part);
    g_free(e->path);
    g_free(e->out_dir);
    g_free(e->project);
    g_free(e);
}

/* Position of the compositor's output over the project's duration. A
 * killed project's duration is a guess, so the bar may lag there. */
static gboolean export_poll(gpointer data) {
    exporter *e = data;
    gint64 pos = 0, dur = 0;
    if (gst_element_query_position(e->comp, GST_FORMAT_TIME, &pos) &&
        gst_element_query_duration(e->demux, GST_FORMAT_TIME, &dur) && dur > 0)
        e->cb.progress(fmin(1, fmax(0, (double)pos / dur)), e->cb.ud);
    return G_SOURCE_CONTINUE;
}

/* Gives the .part its final name. rename() would replace a movie the user
 * saved under that name during the export; link() fails instead, and the
 * next free name is taken. Folders without hard links (FAT, some network
 * mounts) fall back to rename(), as before. NULL on failure; caller frees. */
static char *publish(const char *part, const char *dir, const char *project, const char *ext) {
    for (int i = 0; i < PUBLISH_TRIES; i++) {
        char *path = export_output_path(dir, project, ext);
        if (link(part, path) == 0) {
            g_unlink(part);
            return path;
        }
        int error = errno;
        if (error != EEXIST && g_rename(part, path) == 0) return path;
        g_free(path);
        if (error != EEXIST) break;
    }
    return NULL;
}

static void export_eos(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)msg;
    exporter *e = data;
    char *part = g_strdup(e->part), *dir = g_strdup(e->out_dir), *project = g_strdup(e->project);
    const char *ext = e->ext;
    double seconds = (g_get_monotonic_time() - e->started) / 1e6;
    export_callbacks cb = e->cb;
    export_free(e);
    char *path = publish(part, dir, project, ext);
    if (path) {
        g_print("youcam: exported %s in %.1fs\n", path, seconds);
        cb.finished(path, cb.ud);
    } else {
        g_unlink(part);
        cb.failed("The movie could not be given its final name.", cb.ud);
    }
    g_free(path);
    g_free(part);
    g_free(dir);
    g_free(project);
}

static void export_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    exporter *e = data;
    GError *err = NULL;
    char *debug = NULL;
    gst_message_parse_error(msg, &err, &debug);
    g_printerr("youcam: export error from %s: %s\n%s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
               err ? err->message : "?", debug ? debug : "");
    char *text = g_strdup(err ? err->message : "The movie could not be written.");
    g_clear_error(&err);
    g_free(debug);
    char *part = g_strdup(e->part);
    export_callbacks cb = e->cb;
    export_free(e);
    g_unlink(part);
    cb.failed(text, cb.ud);
    g_free(text);
    g_free(part);
}

/* As fast as decoding and encoding allow (no live source, no clock). */
exporter *export_start(const char *project, const project_info *info, double alpha, const char *out_dir,
                       export_callbacks cb, GError **err) {
    char *desc = g_strdup_printf(
        "filesrc name=src ! matroskademux name=d "
        "d.video_0 ! " TRACK_QUEUE " ! " SCREEN_DECODE " ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_0 "
        "d.video_1 ! " TRACK_QUEUE " ! avdec_ffv1 ! videoconvert ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_1 "
        "compositor name=comp background=black ! video/x-raw,format=BGRA,framerate=30/1 ! "
        "%s ! h264parse ! queue ! mux. %s%s name=mux ! filesink name=file",
        video_encoder_chain(), info->has_audio ? "d.audio_0 ! " TRACK_QUEUE " ! aacparse ! queue ! mux. " : "",
        container_for(info->container)->muxer);
    /* Fatal errors: a missing element returns NULL, never a pipeline and an error. */
    GstElement *p = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, err);
    g_free(desc);
    if (!p) return NULL;
    exporter *e = g_new0(exporter, 1);
    e->pipeline = p;
    e->cb = cb;
    e->out_dir = g_strdup(out_dir);
    e->project = g_strdup(project);
    e->ext = container_for(info->container)->ext;
    e->path = export_output_path(out_dir, project, e->ext);
    e->part = g_strconcat(e->path, ".part", NULL);
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src"), *file = gst_bin_get_by_name(GST_BIN(p), "file");
    g_object_set(src, "location", project, NULL);
    g_object_set(file, "location", e->part, NULL);
    gst_object_unref(src);
    gst_object_unref(file);
    e->comp = gst_bin_get_by_name(GST_BIN(p), "comp");
    e->demux = gst_bin_get_by_name(GST_BIN(p), "d");
    GstPad *layer = gst_element_get_static_pad(e->comp, "sink_1");
    g_object_set(layer, "alpha", alpha, "zorder", 1, NULL);   /* Set here: a %f in the description would follow the locale. */
    gst_object_unref(layer);
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::eos", G_CALLBACK(export_eos), e);
    g_signal_connect(bus, "message::error", G_CALLBACK(export_error), e);
    gst_object_unref(bus);
    e->started = g_get_monotonic_time();
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, "The export could not start.");
        export_cancel(e);
        return NULL;
    }
    e->poll = g_timeout_add(PROGRESS_MS, export_poll, e);
    return e;
}

void export_cancel(exporter *e) {
    char *part = g_strdup(e->part);
    export_free(e);
    g_unlink(part);
    g_free(part);
}
