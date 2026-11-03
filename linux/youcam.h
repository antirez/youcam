/* YouCam for Linux: a frameless camera overlay and screen recorder.
 *
 * Port of the macOS app in Sources/. Every .c file maps to the Swift
 * file with the same role. Geometry is in GDK coordinates: origin at the top
 * left, y grows downwards (AppKit uses a bottom-left origin, so the math
 * below is mirrored vertically compared to the Swift code). */

#ifndef YOUCAM_H
#define YOUCAM_H

#include <gtk/gtk.h>
#include <gst/gst.h>

#define APP_ID "app.youcam.YouCam"

/* ---------------------------------------------------------- geometry.c --- */

typedef struct { double x, y, w, h; } rect;

/* A decoded camera frame: BGRx, 4 bytes per pixel. */
typedef struct {
    int width, height, stride;
    guint8 *data;
} camera_frame;

typedef enum { SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_RECTANGLE, SHAPE_COUNT } camera_shape;

typedef enum {
    CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT, CORNER_COUNT
} area_corner;

#define OVERLAY_MIN_WIDTH 120
#define OVERLAY_MAX_WIDTH 960
#define OVERLAY_DEFAULT_WIDTH 280
#define OVERLAY_EDGE_MARGIN 32
#define AREA_MIN_SIZE 40

const char *shape_name(camera_shape s);
const char *shape_title(camera_shape s);
camera_shape shape_from_name(const char *name);
double shape_aspect(camera_shape s);

void overlay_size(double width, camera_shape s, rect screen, double *w, double *h);
rect overlay_constrain(rect frame, rect screen);
rect overlay_resized(rect frame, double width, camera_shape s, rect screen);
rect overlay_initial(double width, camera_shape s, rect screen);
double rect_intersection_area(rect a, rect b);
int rect_contains(rect outer, rect inner);
int rect_equal(rect a, rect b);

const char *corner_title(area_corner c);
void corner_point(area_corner c, rect r, double *x, double *y);
rect corner_resized(area_corner c, rect r, double px, double py, rect screen, double scale);

/* ----------------------------------------------------------- support.c --- */

typedef enum {
    PHASE_IDLE, PHASE_STARTING, PHASE_RECORDING, PHASE_PAUSED, PHASE_FINISHING
} recording_phase;

#define PHASE_BUSY(p) ((p) != PHASE_IDLE)

typedef struct { float peak; int clipped; float source_peak; } audio_level;   /* source_peak: before the gain. */

#define GAIN_DB_MIN (-36.0)
#define GAIN_DB_MAX 12.0

audio_level audio_gain_apply(float *samples, size_t count, float gain);
float audio_level_db(audio_level level);
double gain_db_clamp(double db);
#define NOISE_REDUCTION_LEVELS 5     /* Off, then webrtcdsp levels low...very high. */
#define NOISE_REDUCTION_DEFAULT 2    /* Moderate: removes room hum, keeps the voice natural. */
int noise_reduction_clamp(int level);
float gain_from_db(double db);
char *recording_next_path(const char *dir, time_t when, const char *ext);
char *recording_base_name(time_t when);   /* YouCam-Recording-<ddMMM-HH.mm.ss>. Caller frees. */

/* Linux addition: the recording container. Every one holds H.264 + AAC. */
typedef struct { const char *ext, *muxer, *label; } container;
extern const container Containers[];
#define CONTAINER_COUNT 4
const container *container_for(const char *ext);   /* Unknown: the default, MOV. */
void recording_time_label(double seconds, char *buf, size_t len);
void crop_pixels(rect fraction, int frame_w, int frame_h, int *left, int *right, int *top, int *bottom);

/* Which panel transport controls are usable in a phase (RecordingPanel). */
typedef struct { int play, pause, stop, area; } panel_buttons;
panel_buttons panel_buttons_for(recording_phase phase, int selecting_area);

/* ------------------------------------------------------------- prefs.c --- */

typedef enum { AREA_WHOLE_SCREEN, AREA_DISPLAY, AREA_REGION } area_mode;

/* A recording area. 'monitor' is the GDK monitor model (connector name), which
 * is stable across sessions; 'local' is monitor-local, in logical pixels. */
typedef struct {
    area_mode mode;
    char monitor[64];
    rect local;
} recording_area;

/* What replaces the background behind the person (background.c). */
typedef enum { BG_OFF, BG_TRANSPARENT, BG_BLUR, BG_IMAGE, BG_COLOR, BG_COUNT } background_mode;

/* What the saved video shows of the camera transparency. Linux adds
 * SAVED_LATER: the transparency is chosen after recording (export.c). */
typedef enum { SAVED_PREVIEW, SAVED_OPAQUE, SAVED_LATER, SAVED_COUNT } saved_transparency;
extern const char *const SavedTransparencyNames[SAVED_COUNT];   /* Prefs values: "preview", "opaque", "later". */
saved_transparency saved_transparency_parse(const char *value, int has_legacy, int legacy);

typedef struct {
    camera_shape shape;
    int mirrored;
    double width;
    int has_origin;
    double origin_x, origin_y;
    char *camera;          /* Display name, NULL = default. */
    char *audio;           /* Display name, "none" = No Audio, NULL = default. */
    double gain_db;
    recording_area area;
    char *restore_token;   /* ScreenCast portal: remembers the shared monitor. */
    double transparency;   /* Camera preview transparency, 0...0.8. */
    saved_transparency saved_transparency;   /* Same as the preview, opaque, or chosen after recording. */
    int center_stage;      /* Follow the face (default on). */
    int noise_reduction;   /* Linux only: 0 off, 1...4 = WebRTC suppression low...very high. */
    char *container;       /* Linux only: file extension of Containers[], NULL = mov. */
    int auto_mic_level;    /* Linux only: adjust the system microphone level while in use. */
    char *mic_restore;     /* Linux only: "node:volume" to put back (survives a crash). */
    background_mode background_mode;   /* Linux only. */
    char *background_image;            /* Linux only: picture for BG_IMAGE. */
    char *background_color;            /* Linux only: "#rrggbb" for BG_COLOR. */
} prefs;

extern prefs Prefs;
void prefs_load(void);
void prefs_save(void);
void prefs_set_string(char **field, const char *value);
recording_area prefs_area_validate(recording_area area, GdkDisplay *display);

/* ------------------------------------------------------------ monitor helpers */

GdkMonitor *monitor_by_name(const char *name);
const char *monitor_name(GdkMonitor *m);
rect monitor_rect(GdkMonitor *m);
rect monitor_workarea(GdkMonitor *m);

/* ------------------------------------------------------------ camera.c --- */

typedef enum {
    CAMERA_IDLE, CAMERA_STARTING, CAMERA_RUNNING, CAMERA_NONE, CAMERA_FAILED
} camera_state_kind;

void camera_init(void (*on_change)(void), void (*on_frame)(void));
void camera_show(void);
void camera_hide(void);
void camera_retry(void);
void camera_select(const char *name);
void camera_shutdown(void);
camera_state_kind camera_state(void);
const char *camera_menu_title(void);
int camera_message(const char **title, const char **detail);
const camera_frame *camera_current_frame(void);
int camera_device_count(void);
const char *camera_device_name(int i);
const char *camera_selected_name(void);

/* ------------------------------------------------------------- audio.c --- */

void audio_init(void (*on_change)(void), void (*on_device_lost)(void));
int audio_has_audio(void);
const char *audio_name(void);
const char *audio_status(void);
audio_level audio_current_level(void);
double audio_gain_db(void);
void audio_set_gain(double db);
void audio_select(const char *name);
int audio_device_count(void);
const char *audio_device_name(int i);
const char *audio_selected_name(void);
GstElement *audio_create_source(void);
void audio_monitor(const char *reason, int enabled);
void audio_post_level(audio_level level);
void audio_shutdown(void);
void audio_set_auto_mic_level(int on);
void audio_install_gain_probe(GstElement *element);

/* ---------------------------------------------------------- background.c --- */

typedef enum { SEG_LANDSCAPE, SEG_SQUARE, SEG_VARIANTS } seg_variant;
const guint8 *background_model_bytes(seg_variant v, size_t *size);

extern const char *const BackgroundNames[BG_COUNT];    /* Prefs values: "off", "transparent", ... */
extern const char *const BackgroundTitles[BG_COUNT];   /* Menu labels: "Off", "Transparent", ... */
#define BACKGROUND_DEFAULT_COLOR "#333333"

background_mode background_mode_parse(const char *name);              /* Unknown or NULL -> BG_OFF. */
int background_color_parse(const char *text, double rgb[3]);          /* "#rrggbb"; 0 if invalid. */
void frame_to_model(const guint8 *bgrx, int w, int h, int stride, guint8 *rgb, int mw, int mh);
void mask_smooth(guint8 *mask, const float *raw, int n, int has_previous);
void mask_compose(const camera_frame *f, const guint8 *mask, int mw, int mh, camera_frame *out);
guint8 mask_edge(guint8 alpha);   /* Tightens the soft model edge: halo gone, person kept. */
void blur_small(const camera_frame *f, camera_frame *out);
GdkPixbuf *background_image_fit(GdkPixbuf *image);   /* New ref: upright, longer side capped. */

/* What camera_layer.c paints instead of the plain frame. */
typedef struct {
    background_mode mode;     /* Never BG_OFF. */
    camera_frame person;      /* Premultiplied ARGB32, alpha = person. */
    camera_frame blurred;     /* BG_BLUR: small BGRx frame. */
    cairo_surface_t *image;   /* BG_IMAGE: fitted and converted once per file. */
    double rgb[3];            /* BG_COLOR. */
} background_layer;

void mask_remove_room(float *raw, const guint8 *rgb, const guint8 *room, int n);   /* Clears furniture matching the empty room. */
typedef void (*background_room_done)(void);
void background_set_camera(const char *name);         /* Main thread: whose empty room applies. */
void background_learn_room(background_room_done done); /* Next ~1 s of frames become the empty room; done on the main thread. */
int background_has_room(void);                         /* The current camera has a learned empty room. */
void background_forget_room(void);
int background_available(void);                      /* Both models load (checked once). */
void background_apply_prefs(void);                   /* Starts or stops the worker for Prefs.background_mode. */
void background_feed(const guint8 *bgrx, int w, int h, int stride);   /* Camera thread. */
const background_layer *background_now(const camera_frame *f);      /* Main thread; NULL = plain picture. */

/* ------------------------------------------------------ camera_layer.c --- */

#define TRANSPARENCY_MAX 0.8

/* Center Stage pan and zoom of the camera picture inside its mask. The
 * shift is a fraction of the mask size, so the camera window and the
 * recording layer, drawn at different pixel sizes, show the same picture. */
typedef struct { double zoom, x, y; } stage_transform;

#define STAGE_IDENTITY ((stage_transform){1, 0, 0})

typedef struct {
    rect frame;          /* Output pixels, top-left origin. */
    double radius;
    camera_shape shape;
} camera_layer;

double transparency_normalize(double value);
int camera_layer_needed(double transparency, int apply_to_video, int visible, int running);
camera_layer camera_layer_map(rect window, rect monitor, rect region, int out_w, int out_h,
                              camera_shape shape);
int camera_layer_clip(camera_layer l, int out_w, int out_h, int *x, int *y, int *w, int *h);
void camera_layer_draw(cairo_t *cr, camera_layer l, int cx, int cy, const camera_frame *f,
                       int mirrored, stage_transform t, const background_layer *bg);
void camera_paint_frame(cairo_t *cr, const camera_frame *f, double w, double h, int mirrored,
                        stage_transform t, const background_layer *bg);
void stage_canvas(double w, double h, double *cw, double *ch);

/* ------------------------------------------------------ center_stage.c --- */

/* A face in normalized camera coordinates, top-left origin: centre and size. */
typedef struct { double cx, cy, w, h; } stage_face;

double stage_soft_clamp(double value, double max);
double stage_zoom(const stage_face *f, camera_shape shape);
int stage_smooth(stage_face *smoothed, int has_smoothed, stage_face f);
stage_transform stage_transform_for(const stage_face *f, double w, double h, camera_shape shape, int mirrored);
int stage_detect(const guint8 *gray, int w, int h, stage_face *out);
void center_stage_feed(const guint8 *bgrx, int w, int h, int stride);
void center_stage_update(const stage_face *f);
void center_stage_reset(void);
int center_stage_has_face(void);
int center_stage_animating(void);
stage_transform center_stage_now(double w, double h, camera_shape shape, int mirrored);
void center_stage_init(void (*on_change)(void));

/* ------------------------------------------------------------ writer.c --- */

typedef struct writer writer;

/* Choose After Recording: what is needed to finish a project (export.c). */
typedef struct {
    double t_a;          /* Camera transparency in the screen track: 1 = absent (extension), else the preview's. */
    double preview;      /* Preview transparency when the recording started: where the slider starts. */
    char container[8];   /* Extension of the final movie (Containers[]). */
    int has_audio;
} project_info;
#define PROJECT_INFO_MAX 160    /* project_info_format() text, NUL included. */

typedef struct {
    void (*finished)(const char *path, void *ud);
    void (*failed)(const char *message, void *ud);
    void *ud;
} writer_callbacks;

/* NONE: screen only. COMPOSITE: the opaque camera copy laid over the screen
 * (writer_push_camera). TRACK: Choose After Recording, the layer on its own
 * lossless track of an MKV project (writer_push_layer). */
typedef enum { WRITER_LAYER_NONE, WRITER_LAYER_COMPOSITE, WRITER_LAYER_TRACK } writer_layer;

/* The screen arrives as BGRA labelled with varying spellings of sRGB
 * (0:0:4:1, 0:1:4:1). A differing label makes the compositor run a full
 * color conversion; relabelling, without touching pixels, keeps it a blend. */
#define SRGB_LABEL "capssetter join=true caps=video/x-raw,colorimetry=sRGB"

writer *writer_start(const char *path, const char *video_source, GstElement *mic, const rect *crop_fraction,
                     writer_layer layer, const project_info *project, writer_callbacks cb, GError **err);
int writer_output_size(writer *w, int *width, int *height);
const char *video_encoder_name(void);
const char *video_encoder_chain(void);   /* Encoder, conversion and bitrate, shared with export.c. */
void writer_push_layer(writer *w, const guint8 *bgra, int stride);   /* Movie-size frame; NULL = transparent. */
int writer_layer_track(writer *w);
void writer_push_camera(writer *w, const guint8 *bgra, int width, int height, int stride,
                        int x, int y, int visible);
void writer_pause(writer *w);
void writer_resume(writer *w);
double writer_elapsed(writer *w);
void writer_finish(writer *w);
void writer_cancel(writer *w);
void pipeline_stop_async(GstElement *pipeline);

/* ------------------------------------------------------------ export.c --- */

#define PROJECT_SUFFIX ".youcam.mkv"

double later_max(double t_a);                  /* Slider maximum: min(T_A, 0.8). */
double later_alpha(double t, double t_a);      /* Layer opacity for a final transparency t: 1 - t / T_A. */
int project_info_format(const project_info *p, char *buf, size_t len);
int project_info_parse(const guint8 *bytes, size_t len, project_info *out);   /* Untrusted bytes. */
int project_read_info(const char *path, project_info *out);
char *project_dir(void);                       /* ~/.local/share/youcam/pending, not created. */
char *project_new_path(const char *dir, time_t when);
char *project_latest(const char *dir, const char *exclude);   /* Newest *.youcam.mkv but exclude, NULL if none. */
void project_clean_parts(const char *dir);
char *export_output_path(const char *dir, const char *project, const char *ext);

int export_available(void);   /* avenc_ffv1, avdec_ffv1 and avdec_h264 are installed (gst-libav). */
/* Two stills from the middle of the project (the first frame of a short or
 * killed one): the screen (RGB24) and the camera layer (ARGB32), 'width' wide. */
int project_stills(const char *path, int width, cairo_surface_t **screen, cairo_surface_t **layer, GError **err);

typedef struct exporter exporter;
typedef struct {
    void (*progress)(double fraction, void *ud);     /* 0...1, polled every 200 ms. */
    void (*finished)(const char *path, void *ud);    /* The movie, renamed from .part. */
    void (*failed)(const char *message, void *ud);   /* .part deleted, the project untouched. */
    void *ud;
} export_callbacks;   /* finished and failed run after the exporter is freed: drop the pointer. */
exporter *export_start(const char *project, const project_info *info, double alpha, const char *out_dir,
                       export_callbacks cb, GError **err);
void export_cancel(exporter *e);   /* No callback follows; .part deleted. */

/* ------------------------------------------------------------ portal.c --- */

void portal_register_app(void);
typedef void (*screencast_ready)(int fd, guint32 node, const char *token,
                                 const char *session, int has_pos, int x, int y, void *ud);
typedef void (*portal_error)(const char *message, void *ud);
void portal_screencast_start(const char *restore_token, screencast_ready ready,
                             portal_error error, void *ud);
void portal_close_session(const char *handle);
GDBusConnection *portal_bus(void);
typedef void (*request_done)(GVariant *results, void *ud);   /* results NULL = cancelled */
int portal_call(const char *iface, const char *method, GVariant **prefix_args, int n_prefix,
                GVariantBuilder *options, request_done done, void *ud, GError **err);

/* ----------------------------------------------------------- hotkeys.c --- */

void hotkeys_init(void (*on_record)(void), void (*on_pause)(void));
int hotkeys_available(void);
void hotkeys_shutdown(void);

/* --------------------------------------------------------- recording.c --- */

void recording_init(void (*on_change)(void), void (*on_error)(const char *));
recording_phase recording_phase_now(void);
const char *recording_status(void);
const char *recording_last_path(void);
void recording_set_last_path(const char *path);
const char *recording_active_project(void);   /* The project being recorded, NULL if none. */
const char *recording_output_dir(void);   /* The Desktop, else the home folder. */
const char *recording_target_monitor(void);
void recording_start(GdkMonitor *monitor, const rect *local);
void recording_toggle_pause(void);
void recording_stop(void);
void recording_fail(const char *message);
void recording_set_project_handlers(void (*on_project)(const char *path), void (*on_notice)(const char *message));
int recording_transparency_locked(void);
writer *recording_layer_target(rect *monitor, rect *region);

/* ----------------------------------------------------------- overlay.c --- */

GtkWidget *overlay_create(void);
void overlay_apply_shape(void);
void overlay_queue_draw(void);
void overlay_animate(void);
void overlay_set_capture_hidden(int hidden);

/* ------------------------------------------------------------- panel.c --- */

GtkWidget *panel_create(void);
void panel_update(void);
void panel_update_audio(void);
GtkWidget *audio_controls_new(void);

/* ------------------------------------------------------------ region.c --- */

void region_select(void (*done)(GdkMonitor *m, const rect *local));
int region_is_selecting(void);
void region_cancel(void);

/* ------------------------------------------------------ area_overlay.c --- */

void area_overlay_show(GdkMonitor *m, rect local);
void area_overlay_hide(void);
void area_overlay_set_editable(int editable);
void area_overlay_set_on_change(void (*cb)(GdkMonitor *m, rect local));

/* -------------------------------------------------------- statusicon.c --- */

void status_icon_init(void (*on_activate)(int x, int y));
void status_icon_update(const char *icon_name, const char *tooltip);

/* ----------------------------------------------------------- mic_level.c --- */

#define MIC_AGC_CLIP_STEP_DB (-6.0)
#define MIC_AGC_STEP_DB 1.5
#define MIC_VOLUME_MIN 0.05
#define MIC_VOLUME_MAX 1.0

typedef struct { gint64 window_start, hold_until; float window_peak; } mic_agc;

double mic_agc_step(mic_agc *a, float source_peak, gint64 now_us);
double mic_volume_after(double volume, double db);
double mic_volume_parse(const char *wpctl_output);
void mic_level_use(const char *node);
void mic_level_feed(float source_peak);
void mic_level_recover(void);

/* ---------------------------------------------------------- mutter_cast.c --- */

/* Windows with this role stay out of recordings (see mutter_cast.c). */
#define CAPTURE_HIDDEN_ROLE "youcam-hidden"

int mutter_cast_available(void);
int mutter_monitor_rect(GVariant *state, const char *connector, int out[4]);
void mutter_cast_start(const char *connector, screencast_ready ready, portal_error error, void *ud);
void mutter_cast_stop(const char *session);

/* ----------------------------------------------------------- segment.c --- */

typedef struct { int h, w, c; } seg_dims;   /* An NHWC tensor, batch 1. */

typedef struct {
    const float *in; seg_dims in_dims;
    const float *weights, *bias;   /* Weights [kh][kw][in c][out c]; depthwise [kh][kw][c]. */
    int kh, kw, stride, pad_top, pad_left;
    float *out; seg_dims out_dims;
} seg_conv_args;

void seg_conv(const seg_conv_args *a);
void seg_dwconv(const seg_conv_args *a);
void seg_tconv(const seg_conv_args *a);      /* Kernel == stride, no padding. */
void seg_resize(const float *in, seg_dims di, float *out, seg_dims dout);
void seg_mean(const float *in, seg_dims di, float *out);
void seg_relu(const float *in, float *out, size_t n);
void seg_hswish(const float *in, float *out, size_t n);
void seg_sigmoid(const float *in, float *out, size_t n);
void seg_mul_channels(const float *in, const float *scale, seg_dims d, float *out);
void seg_add(const float *a, const float *b, float *out, size_t n);

typedef struct segment_model segment_model;
/* Load and free, like run, from one thread at a time: they count live
 * models and create or free the shared worker threads without a lock. */
segment_model *segment_load(const guint8 *data, size_t size, GError **err);
void segment_free(segment_model *m);
void segment_input_size(const segment_model *m, int *w, int *h);
/* rgb: packed 8-bit RGB at the input size; mask: person confidence 0...1, input size.
 * One caller at a time: the worker threads are shared by all models. */
void segment_run(segment_model *m, const guint8 *rgb, float *mask);

/* ------------------------------------------------------- later_dialog.c --- */

void later_dialog_open(const char *project);
GtkWidget *later_dialog_window(void);   /* NULL when closed. */
void later_dialog_shutdown(void);

/* --------------------------------------------------------------- app.c --- */

void app_startup(GtkApplication *gapp, int smoke_test);
void app_reopen(void);
void app_snapshot(const char *dir);
GtkWidget *app_build_menu(void);
GtkWidget *app_build_area_menu(void);
void app_popup_menu(GtkWidget *menu, GdkEvent *event);
void app_resize(double width);
void app_save_geometry(void);
void app_toggle_recording(void);
void app_toggle_pause(void);
void app_toggle_panel(void);
void app_panel_play(void);
void app_panel_stop(void);
const char *app_area_name(void);
int app_panel_visible(void);
void app_quit(void);
void app_show_message(const char *title, const char *text, GtkMessageType type);

#endif
