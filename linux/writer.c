/* Movie writer (RecordingWriter.swift): screen + optional microphone into a
 * QuickTime .mov with H.264 video and AAC audio. Linux adds the other
 * containers and, for Choose After Recording, an MKV project with the
 * camera layer on its own FFV1 track (see export.c).
 *
 * Pausing pauses the whole pipeline. GStreamer's running time does not
 * advance while paused, so both tracks continue seamlessly after a resume
 * without the hand-made timeline the macOS code needs. A static screen still
 * produces frames thanks to pipewiresrc's keepalive and videorate, so the
 * movie always lasts until the real stop time. */

#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define VIDEO_BITRATE_KBPS 12000
#define AUDIO_BITRATE 96000
#define FINISH_TIMEOUT_S 8

struct writer {
    GstElement *pipeline;
    char *path;
    writer_callbacks cb;
    writer_layer layer;
    rect crop;
    int has_crop;
    guint timeout;
    gint64 started, paused_at, paused_total;
    int done;
    gint out_w, out_h;          /* Movie size, set from the streaming thread. */
    GstElement *camera;         /* appsrc of the camera layer, or NULL. */
    GstPad *camera_pad;         /* Its compositor pad. */
    GstElement *outsize;        /* Pins the compositor output to the screen size. */
    int camera_w, camera_h;
    gint screen_seen;           /* The first screen buffer went by (atomic). */
};

/* The movie size is the cropped stream size, known once caps flow. */
static GstPadProbeReturn size_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad;
    writer *w = data;
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
    GstCaps *caps;
    gst_event_parse_caps(ev, &caps);
    int width = 0, height = 0;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    g_atomic_int_set(&w->out_h, height);
    g_atomic_int_set(&w->out_w, width);
    /* A live compositor negotiates on its first timeout, possibly before the
     * screen pad has data: it then knows no size and picks the minimum (16x16
     * after x264). PipeWire starts late, so pin the output to the screen. */
    if (w->outsize && width > 0 && height > 0) {
        /* Same format and colorimetry as its inputs too (see SRGB_LABEL):
         * otherwise the compositor converts every pixel, which took six CPU
         * cores at 2560x1600. */
        GstCaps *size = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
        gst_caps_set_simple(size, "format", G_TYPE_STRING, "BGRA", "colorimetry", G_TYPE_STRING, "sRGB", NULL);
        g_object_set(w->outsize, "caps", size, NULL);
        gst_caps_unref(size);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn screen_arrived(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad; (void)info;
    g_atomic_int_set(&((writer *)data)->screen_seen, 1);
    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn drop_until_screen(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad; (void)info;
    return g_atomic_int_get(&((writer *)data)->screen_seen) ? GST_PAD_PROBE_REMOVE : GST_PAD_PROBE_DROP;
}

int writer_output_size(writer *w, int *width, int *height) {
    *width = g_atomic_int_get(&w->out_w);
    *height = g_atomic_int_get(&w->out_h);
    return *width > 0 && *height > 0;
}

static void set_camera_caps(writer *w, int width, int height) {
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
    g_object_set(w->camera, "caps", caps, NULL);
    gst_caps_unref(caps);
    w->camera_w = width;
    w->camera_h = height;
}

/* Copies a BGRA frame into a buffer for the camera appsrc; NULL pushes a
 * fully transparent one. The caps follow the frame size. */
static void push_frame(writer *w, const guint8 *bgra, int width, int height, int stride) {
    if (width != w->camera_w || height != w->camera_h) set_camera_caps(w, width, height);
    GstBuffer *buf = gst_buffer_new_allocate(NULL, (gsize)width * height * 4, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    if (!bgra) memset(map.data, 0, map.size);
    for (int row = 0; bgra && row < height; row++)
        memcpy(map.data + (size_t)row * width * 4, bgra + (size_t)row * stride, (size_t)width * 4);
    gst_buffer_unmap(buf, &map);
    GstFlowReturn ret;
    g_signal_emit_by_name(w->camera, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);
}

/* Composite: camera layer frame, placed at (x, y) in movie pixels. When not
 * visible the layer is just made transparent; the compositor keeps its last frame. */
void writer_push_camera(writer *w, const guint8 *bgra, int width, int height, int stride,
                        int x, int y, int visible) {
    if (!w->camera_pad || w->done) return;
    if (!visible) {
        g_object_set(w->camera_pad, "alpha", 0.0, NULL);
        return;
    }
    if (w->paused_at) return;
    push_frame(w, bgra, width, height, stride);
    g_object_set(w->camera_pad, "xpos", x, "ypos", y, "width", width, "height", height, "alpha", 1.0, NULL);
}

/* Track: the whole movie-size layer. Its size is pinned by the first frame
 * (Matroska cannot change caps later), so frames come only once the movie
 * size is known, at exactly that size. */
void writer_push_layer(writer *w, const guint8 *bgra, int stride) {
    int width, height;
    if (w->layer != WRITER_LAYER_TRACK || w->done || w->paused_at || !writer_output_size(w, &width, &height)) return;
    push_frame(w, bgra, width, height, stride);
}

int writer_layer_track(writer *w) { return w->layer == WRITER_LAYER_TRACK; }

/* The portal stream's pixel size is known only once caps arrive. */
static GstPadProbeReturn crop_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    writer *w = data;
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
    GstCaps *caps;
    gst_event_parse_caps(ev, &caps);
    int fw = 0, fh = 0, l, r, t, b;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(s, "width", &fw);
    gst_structure_get_int(s, "height", &fh);
    if (fw > 0 && fh > 0) {
        crop_pixels(w->crop, fw, fh, &l, &r, &t, &b);
        GstElement *crop = gst_pad_get_parent_element(pad);
        g_object_set(crop, "left", l, "right", r, "top", t, "bottom", b, NULL);
        gst_object_unref(crop);
    }
    return GST_PAD_PROBE_OK;
}

static void stop_in_thread(GstElement *element, gpointer data) {
    (void)data;
    gst_element_set_state(element, GST_STATE_NULL);
}

/* Stopping from the main loop right after a source error can deadlock inside
 * the source (seen with pipewiresrc); stopping off-thread is always safe. */
void pipeline_stop_async(GstElement *pipeline) {
    gst_element_call_async(pipeline, stop_in_thread, NULL, NULL);
}

static void complete(writer *w, const char *error) {
    if (w->done) return;
    w->done = 1;
    if (w->timeout) g_source_remove(w->timeout);
    w->timeout = 0;
    GstBus *bus = gst_element_get_bus(w->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    /* After an error the sources may be wedged: stop off the main thread. */
    if (error) pipeline_stop_async(w->pipeline);
    else gst_element_set_state(w->pipeline, GST_STATE_NULL);
    if (w->camera) gst_object_unref(w->camera);
    if (w->camera_pad) gst_object_unref(w->camera_pad);
    if (w->outsize) gst_object_unref(w->outsize);
    gst_object_unref(w->pipeline);
    if (error) w->cb.failed(error, w->cb.ud);
    else w->cb.finished(w->path, w->cb.ud);
    g_free(w->path);
    g_free(w);
}

static void on_eos(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)msg;
    complete(data, NULL);
}

static void on_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    GError *err = NULL;
    char *debug = NULL;
    gst_message_parse_error(msg, &err, &debug);
    /* The dialog shows the short message; the log keeps which element failed and why. */
    g_printerr("youcam: recording error from %s: %s\n%s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
               err ? err->message : "?", debug ? debug : "");
    g_free(debug);
    char *text = g_strdup(err ? err->message : "The movie could not be written.");
    g_clear_error(&err);
    complete(data, text);
    g_free(text);
}

static gboolean finish_timeout(gpointer data) {
    writer *w = data;
    w->timeout = 0;
    complete(w, "Finishing the movie timed out.");
    return G_SOURCE_REMOVE;
}

/* H.264 encoders, best first. Encoding a 2560x1600 screen in software costs
 * about 60% of a CPU core, color conversion included; the Intel GPU (VA-API,
 * conversion on the GPU too) about 5%, and it does not wake a discrete GPU
 * on hybrid laptops. NVENC comes next, x264 always works. */
static const struct { const char *name, *chain; } Encoders[] = {
    /* drop-allocation: left to itself the compositor blends straight into
     * mapped GPU memory, which is very slow to read back (6 CPU cores). */
    {"va", "identity drop-allocation=true ! vapostproc ! video/x-raw(memory:VAMemory),format=NV12 ! "
           "vah264enc rate-control=vbr bitrate=" G_STRINGIFY(VIDEO_BITRATE_KBPS) " key-int-max=60"},
    {"nvenc", "videoconvert ! video/x-raw,format=NV12 ! "
              "nvh264enc bitrate=" G_STRINGIFY(VIDEO_BITRATE_KBPS) " gop-size=60"},
    {"x264", "videoconvert ! video/x-raw,format=I420 ! "
             "x264enc tune=zerolatency speed-preset=veryfast bitrate=" G_STRINGIFY(VIDEO_BITRATE_KBPS)
             " key-int-max=60"},
};

/* An encoder plugin can be installed while its GPU is missing or busy, so
 * each candidate encodes two frames before it is trusted. */
static int encoder_works(const char *chain) {
    char *desc = g_strdup_printf("videotestsrc num-buffers=2 ! video/x-raw,format=BGRA,width=320,height=240 ! "
                                 "%s ! fakesink", chain);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    if (!p) return 0;
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    int ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

/* Chosen once per run. YOUCAM_ENCODER=va|nvenc|x264 forces one (tests, bug reports). */
const char *video_encoder_name(void) {
    static const char *chosen;
    if (chosen) return chosen;
    const char *forced = g_getenv("YOUCAM_ENCODER");
    for (size_t i = 0; i < G_N_ELEMENTS(Encoders) && !chosen; i++) {
        int last = i == G_N_ELEMENTS(Encoders) - 1;
        if (forced ? strcmp(forced, Encoders[i].name) == 0 : last || encoder_works(Encoders[i].chain))
            chosen = Encoders[i].name;
    }
    if (!chosen) chosen = "x264";
    g_print("youcam: video encoder %s\n", chosen);
    return chosen;
}

const char *video_encoder_chain(void) {
    const char *name = video_encoder_name();
    for (size_t i = 0; i < G_N_ELEMENTS(Encoders); i++)
        if (strcmp(name, Encoders[i].name) == 0) return Encoders[i].chain;
    return Encoders[G_N_ELEMENTS(Encoders) - 1].chain;
}

/* Linux addition: WebRTC noise suppression (the one in browsers' calls) plus
 * its high-pass filter, which removes hum and room noise from cheap built-in
 * microphones. Echo cancelling and automatic gain stay off: nothing plays back
 * and the user owns the gain. Missing plugin: record unprocessed. */
static char *noise_reduction_element(int level) {
    static const char *names[] = {"", "low", "moderate", "high", "very-high"};
    level = noise_reduction_clamp(level);
    GstElementFactory *f = gst_element_factory_find("webrtcdsp");
    if (!level || !f) {
        if (f) gst_object_unref(f);
        return g_strdup("");
    }
    gst_object_unref(f);
    return g_strdup_printf("audio/x-raw,format=S16LE,channels=1,rate=48000 ! "
        "webrtcdsp echo-cancel=false gain-control=false high-pass-filter=true "
        "noise-suppression=true noise-suppression-level=%s ! audioconvert ! ", names[level]);
}

/* A live pipeline starts on its first data, which may come from the camera
 * before PipeWire delivers the screen: hold the camera until then. */
static void hold_camera_until_screen(writer *w, GstPad *screen, GstPad *camera) {
    gst_pad_add_probe(screen, GST_PAD_PROBE_TYPE_BUFFER, screen_arrived, w, NULL);
    gst_pad_add_probe(camera, GST_PAD_PROBE_TYPE_BUFFER, drop_until_screen, w, NULL);
    gst_object_unref(screen);
    gst_object_unref(camera);
}

/* The settings go into the header (WritingApp), on disk from the start, and
 * into the COMMENT tag, which the muxer writes only at the end. */
static void tag_project(GstElement *pipeline, const project_info *project) {
    char text[PROJECT_INFO_MAX];
    if (!project || !project_info_format(project, text, sizeof(text))) return;
    GstElement *mux = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
    g_object_set(mux, "writing-app", text, NULL);
    gst_tag_setter_add_tags(GST_TAG_SETTER(mux), GST_TAG_MERGE_REPLACE, GST_TAG_COMMENT, text, NULL);
    gst_object_unref(mux);
}

writer *writer_start(const char *path, const char *video_source, GstElement *mic, const rect *crop_fraction,
                     writer_layer layer, const project_info *project, writer_callbacks cb, GError **err) {
    char *denoise = noise_reduction_element(Prefs.noise_reduction);
    char *audio = mic ? g_strdup_printf(
        "audioconvert name=aconvert ! audioresample ! %s"
        "audio/x-raw,format=F32LE,channels=1,rate=48000 ! identity name=gain ! "
        "audioconvert ! avenc_aac bitrate=%d ! queue ! mux. ", denoise, AUDIO_BITRATE) : g_strdup("");
    g_free(denoise);
    /* Composite: the camera layer as an appsrc laid over the screen. */
    const char *composite = layer == WRITER_LAYER_COMPOSITE
        ? "videoconvert ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_0 "
          "appsrc name=camera is-live=true do-timestamp=true format=time "
          "caps=video/x-raw,format=BGRA,width=2,height=2,framerate=0/1 ! videoconvert ! "
          "video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_1 "
          "compositor name=comp ignore-inactive-pads=true background=black ! "
          "capsfilter name=outsize caps=video/x-raw,framerate=30/1 ! "
        : "";
    /* Track: the layer at movie size on its own lossless track, linked after
     * the screen so it is track 2 (export.c relies on it). No caps here: they
     * come with the first frame. A frame is 16 MB at 2560x1600, so a busy
     * encoder drops new ones rather than queueing them; the export keeps
     * showing the last one. max-bytes=0: its default (200000) is below one
     * frame and would make the queue "full" after a single buffer. */
    const char *track = layer == WRITER_LAYER_TRACK
        ? "appsrc name=camera is-live=true do-timestamp=true format=time max-buffers=4 max-bytes=0 leaky-type=upstream ! "
          "avenc_ffv1 ! queue ! mux. "
        : "";
    const char *ext = strrchr(path, '.') ? strrchr(path, '.') + 1 : NULL;
    char *desc = g_strdup_printf(
        "%s ! videoconvert ! videocrop name=crop ! videorate ! video/x-raw,framerate=30/1 ! %s"
        "%s ! h264parse ! queue ! mux. %s%s%s name=mux ! filesink name=file",
        video_source, composite, video_encoder_chain(), track, audio,
        container_for(layer == WRITER_LAYER_TRACK ? "mkv" : ext)->muxer);
    GstElement *p = gst_parse_launch(desc, err);
    g_free(desc);
    g_free(audio);
    if (!p) return NULL;
    /* pipewiresrc offers its own clock and the pipeline picks it, but with
     * that clock a resume does not subtract the pause: the layer and audio
     * jumped by the whole pause while videorate filled the screen track. */
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(p), clock);
    gst_object_unref(clock);

    if (mic) {
        GstElement *convert = gst_bin_get_by_name(GST_BIN(p), "aconvert");
        int ok = gst_bin_add(GST_BIN(p), mic) && gst_element_link(mic, convert);
        gst_object_unref(convert);
        if (!ok) {
            g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                        "The microphone could not be connected.");
            gst_object_unref(p);
            return NULL;
        }
        GstElement *gain = gst_bin_get_by_name(GST_BIN(p), "gain");
        audio_install_gain_probe(gain);
        gst_object_unref(gain);
    }

    writer *w = g_new0(writer, 1);
    w->pipeline = p;
    w->path = g_strdup(path);
    w->cb = cb;
    w->layer = layer;
    GstElement *file = gst_bin_get_by_name(GST_BIN(p), "file");
    g_object_set(file, "location", path, NULL);
    gst_object_unref(file);
    GstElement *crop = gst_bin_get_by_name(GST_BIN(p), "crop");
    GstPad *pad = gst_element_get_static_pad(crop, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, size_probe, w, NULL);
    if (layer != WRITER_LAYER_NONE) w->camera = gst_bin_get_by_name(GST_BIN(p), "camera");
    if (layer == WRITER_LAYER_TRACK) {
        hold_camera_until_screen(w, gst_object_ref(pad), gst_element_get_static_pad(w->camera, "src"));
        tag_project(p, project);
    }
    gst_object_unref(pad);
    if (crop_fraction) {
        w->crop = *crop_fraction;
        pad = gst_element_get_static_pad(crop, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, crop_probe, w, NULL);
        gst_object_unref(pad);
    }
    gst_object_unref(crop);
    if (layer == WRITER_LAYER_COMPOSITE) {
        GstElement *comp = gst_bin_get_by_name(GST_BIN(p), "comp");
        w->camera_pad = gst_element_get_static_pad(comp, "sink_1");
        w->outsize = gst_bin_get_by_name(GST_BIN(p), "outsize");
        hold_camera_until_screen(w, gst_element_get_static_pad(comp, "sink_0"),
                                 gst_element_get_static_pad(w->outsize, "src"));
        gst_object_unref(comp);
        g_object_set(w->camera_pad, "alpha", 0.0, "zorder", 1, NULL);
        w->camera_w = w->camera_h = 2;
        guint8 blank[2 * 2 * 4] = {0};
        writer_push_camera(w, blank, 2, 2, 8, 0, 0, 1);
        g_object_set(w->camera_pad, "alpha", 0.0, NULL);
    }
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::eos", G_CALLBACK(on_eos), w);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), w);
    gst_object_unref(bus);

    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, "The recording could not start.");
        bus = gst_element_get_bus(p);
        gst_bus_remove_signal_watch(bus);
        gst_object_unref(bus);
        gst_element_set_state(p, GST_STATE_NULL);
        if (w->camera) gst_object_unref(w->camera);
        if (w->camera_pad) gst_object_unref(w->camera_pad);
        if (w->outsize) gst_object_unref(w->outsize);
        gst_object_unref(p);
        g_unlink(path);
        g_free(w->path);
        g_free(w);
        return NULL;
    }
    w->started = g_get_monotonic_time();
    return w;
}

void writer_pause(writer *w) {
    if (w->paused_at) return;
    w->paused_at = g_get_monotonic_time();
    gst_element_set_state(w->pipeline, GST_STATE_PAUSED);
}

void writer_resume(writer *w) {
    if (!w->paused_at) return;
    w->paused_total += g_get_monotonic_time() - w->paused_at;
    w->paused_at = 0;
    gst_element_set_state(w->pipeline, GST_STATE_PLAYING);
}

double writer_elapsed(writer *w) {
    gint64 end = w->paused_at ? w->paused_at : g_get_monotonic_time();
    return (end - w->started - w->paused_total) / 1e6;
}

/* EOS lets the muxer write the movie index; 'finished' or 'failed' follows. */
void writer_finish(writer *w) {
    if (w->timeout) return;
    writer_resume(w);
    gst_element_send_event(w->pipeline, gst_event_new_eos());
    w->timeout = g_timeout_add_seconds(FINISH_TIMEOUT_S, finish_timeout, w);
}

/* A cancelled start is not a recording: drop the file, no callbacks. */
void writer_cancel(writer *w) {
    char *path = g_strdup(w->path);
    w->cb.failed = NULL;
    w->done = 1;
    if (w->timeout) g_source_remove(w->timeout);
    GstBus *bus = gst_element_get_bus(w->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(w->pipeline, GST_STATE_NULL);
    if (w->camera) gst_object_unref(w->camera);
    if (w->camera_pad) gst_object_unref(w->camera_pad);
    if (w->outsize) gst_object_unref(w->outsize);
    gst_object_unref(w->pipeline);
    g_unlink(path);
    g_free(path);
    g_free(w->path);
    g_free(w);
}
