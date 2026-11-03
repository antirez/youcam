/* Records synthetic screen + microphone through the real writer pipeline,
 * with a pause in the middle, then inspects the movie (RecordingTests.swift,
 * movie part). A second movie without audio must have no audio track.
 * Choose After Recording: a project keeps screen, camera layer and
 * microphone apart, with its settings in the MKV header. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gst/pbutils/pbutils.h>
#include "youcam.h"

#define RECORD_S 2
#define PAUSE_S 2
#define PROJECT_S 4
#define PROJECT_COPY_S 2   /* When the "killed" copy is taken. */
#define SQUARE 64          /* The project's camera: an opaque red square... */
#define SQUARE_X 100       /* ...at this position in the 640x360 movie. */
#define SQUARE_Y 100

static GMainLoop *Loop;
static char *Result;
static char *Error;
static writer *W;
static char *ProjectPath, *KilledPath;

#define EXPECT(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } \
} while (0)

static void finished(const char *path, void *ud) { (void)ud; Result = g_strdup(path); g_main_loop_quit(Loop); }
static void failed(const char *msg, void *ud) { (void)ud; Error = g_strdup(msg); g_main_loop_quit(Loop); }
static gboolean do_pause(gpointer d) { (void)d; writer_pause(W); return G_SOURCE_REMOVE; }
static gboolean do_resume(gpointer d) { (void)d; writer_resume(W); return G_SOURCE_REMOVE; }
static gboolean do_finish(gpointer d) { (void)d; writer_finish(W); return G_SOURCE_REMOVE; }

/* Camera layer: an opaque red square pushed at ~30 fps, moving. */
static int LayerFrames;

static gboolean push_layer(gpointer d) {
    (void)d;
    if (!W) return G_SOURCE_REMOVE;
    static guint8 red[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) { red[i * 4 + 2] = 255; red[i * 4 + 3] = 255; }
    int x = (LayerFrames * 4) % 500;
    writer_push_camera(W, red, 64, 64, 64 * 4, x, 100, 1);
    LayerFrames++;
    return G_SOURCE_CONTINUE;
}

/* Project layer: the movie-size frame, transparent but for the red square. */
static gboolean push_project_layer(gpointer d) {
    (void)d;
    if (!W) return G_SOURCE_REMOVE;
    int w, h;
    if (!writer_output_size(W, &w, &h)) return G_SOURCE_CONTINUE;
    guint8 *frame = g_malloc0((size_t)w * h * 4);
    for (int y = SQUARE_Y; y < SQUARE_Y + SQUARE && y < h; y++)
        for (int x = SQUARE_X; x < SQUARE_X + SQUARE && x < w; x++) {
            frame[((size_t)y * w + x) * 4 + 2] = 255;
            frame[((size_t)y * w + x) * 4 + 3] = 255;
        }
    writer_push_layer(W, frame, w * 4);
    g_free(frame);
    LayerFrames++;
    return G_SOURCE_CONTINUE;
}

/* What a killed YouCam leaves behind: the project as it is mid-recording. */
static gboolean copy_project(gpointer d) {
    (void)d;
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(ProjectPath, &data, &len, NULL)) g_file_set_contents(KilledPath, data, len, NULL);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static GstDiscovererInfo *discover(const char *path) {
    GError *err = NULL;
    char *uri = gst_filename_to_uri(path, NULL);
    GstDiscoverer *d = gst_discoverer_new(5 * GST_SECOND, NULL);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(d, uri, &err);
    EXPECT(info, "movie unreadable: %s", err ? err->message : "?");
    g_free(uri);
    g_object_unref(d);
    return info;
}

static GstDiscovererInfo *record(const char *path, int with_audio, int with_pause, int with_layer) {
    GstElement *mic = NULL;
    if (with_audio) {
        mic = gst_element_factory_make("audiotestsrc", NULL);
        g_object_set(mic, "is-live", TRUE, NULL);
    }
    rect crop = {0.25, 0.25, 0.5, 0.5};
    GError *err = NULL;
    Result = Error = NULL;
    W = writer_start(path, "videotestsrc is-live=true ! video/x-raw,width=1280,height=720,framerate=30/1",
                     mic, &crop, with_layer ? WRITER_LAYER_COMPOSITE : WRITER_LAYER_NONE, NULL,
                     (writer_callbacks){finished, failed, NULL}, &err);
    EXPECT(W, "writer did not start: %s", err ? err->message : "?");
    guint layer = with_layer ? g_timeout_add(33, push_layer, NULL) : 0;
    int stop_at = RECORD_S;
    if (with_pause) {
        g_timeout_add_seconds(RECORD_S, do_pause, NULL);
        g_timeout_add_seconds(RECORD_S + PAUSE_S, do_resume, NULL);
        stop_at = 2 * RECORD_S + PAUSE_S;
    }
    g_timeout_add_seconds(stop_at, do_finish, NULL);
    g_main_loop_run(Loop);
    W = NULL;
    if (layer) g_source_remove(layer);
    EXPECT(!Error, "recording failed: %s", Error);
    return discover(Result);
}

/* A blue screen, so the camera centre has a known colour under it. */
static GstDiscovererInfo *record_project(const project_info *project) {
    GstElement *mic = gst_element_factory_make("audiotestsrc", NULL);
    g_object_set(mic, "is-live", TRUE, NULL);
    rect crop = {0.25, 0.25, 0.5, 0.5};
    GError *err = NULL;
    Result = Error = NULL;
    LayerFrames = 0;
    W = writer_start(ProjectPath, "videotestsrc is-live=true pattern=solid-color foreground-color=0xff0000ff ! "
                     "video/x-raw,width=1280,height=720,framerate=30/1", mic, &crop, WRITER_LAYER_TRACK, project,
                     (writer_callbacks){finished, failed, NULL}, &err);
    EXPECT(W, "project writer did not start: %s", err ? err->message : "?");
    guint layer = g_timeout_add(33, push_project_layer, NULL);
    g_timeout_add_seconds(PROJECT_COPY_S, copy_project, NULL);
    g_timeout_add_seconds(PROJECT_S, do_finish, NULL);
    g_main_loop_run(Loop);
    W = NULL;
    g_source_remove(layer);
    EXPECT(!Error, "project recording failed: %s", Error);
    return discover(Result);
}

/* export.c links d.video_0 to the H.264 decoder and d.video_1 to FFV1:
 * the screen must be track 1 and the layer track 2. */
static void check_project_tracks(GstDiscovererInfo *info) {
    GList *videos = gst_discoverer_info_get_video_streams(info);
    GList *audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(g_list_length(videos) == 2 && g_list_length(audios) == 1,
           "a project holds the screen, the camera layer and the microphone (%u video, %u audio)",
           g_list_length(videos), g_list_length(audios));
    for (GList *l = videos; l; l = l->next) {
        GstCaps *caps = gst_discoverer_stream_info_get_caps(l->data);
        const char *id = gst_discoverer_stream_info_get_stream_id(l->data);
        int screen = gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-h264");
        int layer = gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-ffv");
        EXPECT(id && ((screen && strstr(id, "/001")) || (layer && strstr(id, "/002"))),
               "screen on track 1, FFV1 layer on track 2 (got %s on %s)", gst_caps_to_string(caps), id);
        gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_stream_info_list_free(audios);
}

/* Decodes frame number 'index' of a movie as packed RGB. Caller unrefs. */
static GstSample *frame_at(const char *path, int index, int *width) {
    char *desc = g_strdup_printf("filesrc location=\"%s\" ! decodebin ! videoconvert ! "
                                 "video/x-raw,format=RGB ! appsink name=sink sync=false", path);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstSample *sample = NULL;
    for (int i = 0; i <= index; i++) {
        if (sample) gst_sample_unref(sample);
        g_signal_emit_by_name(sink, "pull-sample", &sample);
        if (!sample) break;
    }
    *width = 0;
    if (sample) gst_structure_get_int(gst_caps_get_structure(gst_sample_get_caps(sample), 0), "width", width);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(p);
    return sample;
}

#define RGB_STRIDE(w) (((w) * 3 + 3) & ~3)

/* Counts strongly red pixels on 'row' of frame 'index'. */
static int red_pixels_on_row(const char *path, int index, int row) {
    int w, count = 0;
    GstSample *sample = frame_at(path, index, &w);
    if (!sample) return 0;
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    gst_buffer_map(buf, &map, GST_MAP_READ);
    for (int x = 0; x < w; x++) {
        const guint8 *px = map.data + row * RGB_STRIDE(w) + x * 3;
        if (px[0] > 200 && px[1] < 60 && px[2] < 60) count++;
    }
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return count;
}

static void pixel_at(const char *path, int index, int x, int y, guint8 rgb[3]) {
    int w;
    GstSample *sample = frame_at(path, index, &w);
    EXPECT(sample && x < w, "frame %d of %s decodes", index, path);
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    gst_buffer_map(buf, &map, GST_MAP_READ);
    memcpy(rgb, map.data + (size_t)y * RGB_STRIDE(w) + (size_t)x * 3, 3);
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
}

#define CX (SQUARE_X + SQUARE / 2)   /* Camera centre in movie pixels. */
#define CY (SQUARE_Y + SQUARE / 2)
#define MIX_TOLERANCE 30             /* Two H.264 passes. */

static guint32 surface_pixel(cairo_surface_t *s, int x, int y) {
    cairo_surface_flush(s);
    return *(guint32 *)(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s) + x * 4);
}

/* The dialog's stills: the blue screen, and the layer with the opaque red
 * square on transparency. 640 wide, the movie's own width. */
static void check_stills(const char *path) {
    cairo_surface_t *screen, *layer;
    GError *err = NULL;
    EXPECT(project_stills(path, 640, &screen, &layer, &err), "stills of %s: %s", path, err ? err->message : "?");
    guint32 s = surface_pixel(screen, CX, CY), in = surface_pixel(layer, CX, CY), out = surface_pixel(layer, 10, 10);
    EXPECT((s & 0xff) > 200 && (s >> 16 & 0xff) < 60, "the screen still shows the screen (%08x)", s);
    EXPECT(in >> 24 == 255 && (in >> 16 & 0xff) > 200, "the layer still has the opaque camera (%08x)", in);
    EXPECT(out >> 24 == 0, "the layer still is transparent elsewhere (%08x)", out);
    cairo_surface_destroy(screen);
    cairo_surface_destroy(layer);
}

static char *ExportResult, *ExportError;
static void export_done(const char *path, void *ud) { (void)ud; ExportResult = g_strdup(path); g_main_loop_quit(Loop); }
static void export_fail(const char *msg, void *ud) { (void)ud; ExportError = g_strdup(msg); g_main_loop_quit(Loop); }
static void export_progress(double fraction, void *ud) { (void)fraction; (void)ud; }

/* Exports the project at final transparency t into info->container. */
static char *export_project(const project_info *info, double t, const char *dir) {
    GError *err = NULL;
    ExportResult = ExportError = NULL;
    exporter *e = export_start(ProjectPath, info, later_alpha(t, info->t_a), dir,
                               (export_callbacks){export_progress, export_done, export_fail, NULL}, &err);
    EXPECT(e, "export did not start: %s", err ? err->message : "?");
    g_main_loop_run(Loop);
    EXPECT(!ExportError, "export failed: %s", ExportError);
    char *part = g_strconcat(ExportResult, ".part", NULL);
    EXPECT(!g_file_test(part, G_FILE_TEST_EXISTS), "the temporary .part name is gone");
    g_free(part);
    return ExportResult;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    Loop = g_main_loop_new(NULL, FALSE);
    char *dir = g_dir_make_tmp("youcam-media-XXXXXX", NULL);
    char *path = g_build_filename(dir, "pause-resume.mov", NULL);

    /* The main movie also runs the microphone through WebRTC noise reduction. */
    Prefs.noise_reduction = NOISE_REDUCTION_DEFAULT;
    GstDiscovererInfo *info = record(path, 1, 1, 0);
    Prefs.noise_reduction = 0;
    double duration = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    GList *videos = gst_discoverer_info_get_video_streams(info);
    GList *audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(videos && audios, "MOV has video and microphone tracks");
    GstDiscovererVideoInfo *v = videos->data;
    EXPECT(gst_discoverer_video_info_get_width(v) == 640 && gst_discoverer_video_info_get_height(v) == 360,
           "the selected area is cropped (got %ux%u)", gst_discoverer_video_info_get_width(v),
           gst_discoverer_video_info_get_height(v));
    EXPECT(fabs(duration - 2 * RECORD_S) < PAUSE_S / 2.0,
           "the pause is removed from the movie (duration %.2fs)", duration);
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_stream_info_list_free(audios);
    gst_discoverer_info_unref(info);
    g_unlink(path);

    info = record(path, 0, 0, 0);
    audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(!audios, "No Audio creates a movie without an audio track");
    gst_discoverer_info_unref(info);
    g_unlink(path);

    /* Every other container: readable, both tracks, pause removed. */
    for (int i = 1; i < CONTAINER_COUNT; i++) {
        char *name = g_strdup_printf("format.%s", Containers[i].ext);
        char *other = g_build_filename(dir, name, NULL);
        info = record(other, 1, 1, 0);
        double d = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
        videos = gst_discoverer_info_get_video_streams(info);
        audios = gst_discoverer_info_get_audio_streams(info);
        EXPECT(videos && audios, "%s holds video and audio", Containers[i].label);
        EXPECT(fabs(d - 2 * RECORD_S) < PAUSE_S / 2.0, "%s duration %.2fs", Containers[i].label, d);
        gst_discoverer_stream_info_list_free(videos);
        gst_discoverer_stream_info_list_free(audios);
        gst_discoverer_info_unref(info);
        g_unlink(other);
        g_free(other);
        g_free(name);
    }

    /* Camera layer through the compositor, with a pause. */
    info = record(path, 1, 1, 1);
    double layered = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    videos = gst_discoverer_info_get_video_streams(info);
    EXPECT(videos && gst_discoverer_video_info_get_width(videos->data) == 640,
           "the camera layer keeps the movie at the cropped size");
    EXPECT(fabs(layered - 2 * RECORD_S) < PAUSE_S / 2.0, "camera layer movie duration %.2fs", layered);
    EXPECT(LayerFrames > 30, "camera frames were pushed (%d)", LayerFrames);
    int red = red_pixels_on_row(Result, 30, 132);
    EXPECT(red >= 60, "the camera layer is visible in the movie (%d red pixels)", red);
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_info_unref(info);
    g_unlink(path);
    int composited = LayerFrames;

    /* Choose After Recording: three tracks; the settings survive a kill. */
    project_info project = {0.5, 0.5, "mov", 1}, back = {0};
    ProjectPath = g_build_filename(dir, "project" PROJECT_SUFFIX, NULL);
    KilledPath = g_build_filename(dir, "killed" PROJECT_SUFFIX, NULL);
    info = record_project(&project);
    double project_s = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    check_project_tracks(info);
    char *comment = NULL;
    const GstTagList *tags = gst_discoverer_info_get_tags(info);
    EXPECT(tags && gst_tag_list_get_string(tags, GST_TAG_COMMENT, &comment) && strstr(comment, "YOUCAM_T_A=0.500;"),
           "a finished project also carries its settings in the MKV comment tag");
    g_free(comment);
    gst_discoverer_info_unref(info);
    EXPECT(fabs(project_s - PROJECT_S) < 0.5, "project duration %.2fs", project_s);
    EXPECT(LayerFrames > 60, "camera layer frames were pushed (%d)", LayerFrames);
    EXPECT(project_read_info(ProjectPath, &back) && back.t_a == 0.5 && back.preview == 0.5 &&
           strcmp(back.container, "mov") == 0 && back.has_audio, "a finished project is read back");
    EXPECT(project_read_info(KilledPath, &back) && back.t_a == 0.5,
           "a killed project keeps its settings (they are in the MKV header)");

    EXPECT(export_available(), "FFV1 and the H.264 decoder are installed (gst-libav)");
    check_stills(ProjectPath);
    check_stills(KilledPath);   /* Its duration is a guess: the first frame is used. */

    /* t = 0: the opaque camera; t = T_A: the screen alone; halfway: an even mix. */
    guint8 rgb[3];
    char *movie = export_project(&project, 0, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(rgb[0] > 200 && rgb[1] < 60 && rgb[2] < 60, "t = 0 shows the opaque camera (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    pixel_at(movie, 45, SQUARE_X / 2, SQUARE_Y / 2, rgb);   /* The layer keeps its alpha: clear off the camera. */
    EXPECT(rgb[0] < 60 && rgb[1] < 60 && rgb[2] > 200, "t = 0 shows the screen off the camera (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);
    movie = export_project(&project, project.t_a, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(rgb[0] < 60 && rgb[1] < 60 && rgb[2] > 200, "t = T_A shows only the screen (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);
    movie = export_project(&project, project.t_a / 2, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(abs(rgb[0] - 128) < MIX_TOLERANCE && abs(rgb[2] - 128) < MIX_TOLERANCE && rgb[1] < 60,
           "t = T_A / 2 mixes camera and screen evenly (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);

    /* A movie saved under the export's name while it runs is never replaced. */
    char *taken = export_output_path(dir, ProjectPath, project.container);
    GError *race_err = NULL;
    ExportResult = ExportError = NULL;
    exporter *race = export_start(ProjectPath, &project, 1, dir,
                                  (export_callbacks){export_progress, export_done, export_fail, NULL}, &race_err);
    EXPECT(race, "export did not start: %s", race_err ? race_err->message : "?");
    g_file_set_contents(taken, "mine", -1, NULL);
    g_main_loop_run(Loop);
    char *kept = NULL;
    EXPECT(!ExportError && ExportResult && g_str_has_suffix(ExportResult, " (2).mov"),
           "the export takes the next free name (%s)", ExportResult ? ExportResult : ExportError);
    EXPECT(g_file_get_contents(taken, &kept, NULL, NULL) && strcmp(kept, "mine") == 0,
           "the movie that took the name is untouched");
    g_unlink(taken);
    g_unlink(ExportResult);
    g_free(kept);
    g_free(taken);
    g_free(ExportResult);

    /* Every container, the audio copied, the length kept. */
    for (int i = 0; i < CONTAINER_COUNT; i++) {
        project_info as = project;
        g_strlcpy(as.container, Containers[i].ext, sizeof(as.container));
        movie = export_project(&as, project.t_a / 2, dir);
        EXPECT(g_str_has_suffix(movie, Containers[i].ext), "%s export has its extension", Containers[i].label);
        info = discover(movie);
        double d = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
        videos = gst_discoverer_info_get_video_streams(info);
        audios = gst_discoverer_info_get_audio_streams(info);
        EXPECT(videos && audios, "%s export holds video and the copied audio", Containers[i].label);
        EXPECT(fabs(d - project_s) < 0.5, "%s export lasts as long as the project (%.2fs, %.2fs)",
               Containers[i].label, d, project_s);
        gst_discoverer_stream_info_list_free(videos);
        gst_discoverer_stream_info_list_free(audios);
        gst_discoverer_info_unref(info);
        g_unlink(movie);
        g_free(movie);
    }

    /* A failed export keeps the project. */
    GError *bad_err = NULL;
    ExportResult = ExportError = NULL;
    exporter *bad = export_start(ProjectPath, &project, 1, "/nonexistent-youcam-dir",
                                 (export_callbacks){export_progress, export_done, export_fail, NULL}, &bad_err);
    if (bad) g_main_loop_run(Loop);
    EXPECT(!bad || ExportError, "an unwritable folder fails the export");
    EXPECT(g_file_test(ProjectPath, G_FILE_TEST_EXISTS), "a failed export keeps the project");
    g_clear_error(&bad_err);

    g_unlink(ProjectPath);
    g_unlink(KilledPath);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track; "
           "camera layer movie %.2fs with %d composited frames; mp4, mkv and avi ok; "
           "project %.2fs with screen, FFV1 layer and audio, settings read back after a finish and a kill; "
           "stills ok; exports at t = 0, T_A/2 and T_A ok, to MOV, MP4, MKV and AVI with audio; "
           "a name taken during the export is kept\n",
           duration, layered, composited, project_s);
    return 0;
}
