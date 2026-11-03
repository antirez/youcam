/* Camera capture (CameraController.swift).
 *
 * Cameras come straight from the V4L2 device provider: PipeWire camera nodes
 * fail with "target not found" outside the camera portal, and V4L2 names tell
 * an RGB camera from its IR twin. Frames are decoded to 640 px wide BGRx and
 * handed to the overlay; one copy per frame is cheap at that size. */

#include <string.h>
#include "youcam.h"

#define PREVIEW_WIDTH 640
#define MAX_CAMERAS 16

static struct {
    GstDeviceProvider *provider;   /* V4L2, or NULL to use the monitor. */
    GstDeviceMonitor *monitor;
    GstDevice *devices[MAX_CAMERAS];
    char *names[MAX_CAMERAS];
    int count;
    char *selected;                /* Name of the active device. */
    char *configured;              /* Name the pipeline runs with. */
    GstElement *pipeline;
    camera_state_kind state;
    char *detail;                  /* Failure text. */
    int wants_camera;
    GMutex lock;
    camera_frame frame, back;      /* 'back' is filled by the streaming thread. */
    int frame_ready, draw_pending;
    void (*on_change)(void);
    void (*on_frame)(void);
} C;

static void set_state(camera_state_kind s, const char *detail) {
    C.state = s;
    prefs_set_string(&C.detail, detail);
    if (C.on_change) C.on_change();
}

static void stop_pipeline(int async) {
    if (!C.pipeline) return;
    GstBus *bus = gst_element_get_bus(C.pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    if (async) pipeline_stop_async(C.pipeline);
    else gst_element_set_state(C.pipeline, GST_STATE_NULL);
    gst_object_unref(C.pipeline);
    C.pipeline = NULL;
    g_free(C.configured);
    C.configured = NULL;
    g_mutex_lock(&C.lock);
    C.frame_ready = 0;
    g_mutex_unlock(&C.lock);
    center_stage_reset();
}

static gboolean deliver_frame(gpointer data) {
    (void)data;
    g_mutex_lock(&C.lock);
    C.draw_pending = 0;
    g_mutex_unlock(&C.lock);
    if (!C.pipeline) return G_SOURCE_REMOVE;
    if (C.state != CAMERA_RUNNING) set_state(CAMERA_RUNNING, NULL);
    if (C.on_frame) C.on_frame();
    return G_SOURCE_REMOVE;
}

static GstFlowReturn new_sample(GstElement *sink, gpointer data) {
    (void)data;
    GstSample *sample = NULL;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_OK;
    GstStructure *s = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w > 0 && h > 0 && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        size_t size = (size_t)w * h * 4;
        g_mutex_lock(&C.lock);
        if (C.back.width != w || C.back.height != h) {
            g_free(C.back.data);
            C.back.data = g_malloc(size);
            C.back.width = w;
            C.back.height = h;
            C.back.stride = w * 4;
        }
        memcpy(C.back.data, map.data, MIN(size, map.size));
        C.frame_ready = 1;
        int schedule = !C.draw_pending;
        C.draw_pending = 1;
        g_mutex_unlock(&C.lock);
        if (map.size >= size) center_stage_feed(map.data, w, h, w * 4);   /* Off the main thread. */
        if (map.size >= size) background_feed(map.data, w, h, w * 4);
        gst_buffer_unmap(buf, &map);
        if (schedule) g_idle_add(deliver_frame, NULL);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void on_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    GError *err = NULL;
    gst_message_parse_error(msg, &err, NULL);
    g_printerr("youcam: camera error: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    int was_running = C.state == CAMERA_RUNNING;
    stop_pipeline(1);
    set_state(CAMERA_FAILED, was_running
        ? "The camera was interrupted. Close other camera apps or choose Retry Camera."
        : "This camera could not be opened. Try another camera.");
}

static int find_device(const char *name) {
    for (int i = 0; name && i < C.count; i++)
        if (strcmp(C.names[i], name) == 0) return i;
    return -1;
}

static void start_selected(void) {
    int i = find_device(C.selected);
    if (i < 0) {
        stop_pipeline(0);
        set_state(CAMERA_NONE, NULL);
        return;
    }
    if (C.configured && strcmp(C.configured, C.selected) == 0) return;
    stop_pipeline(0);

    GError *err = NULL;
    GstElement *p = gst_parse_launch(
        "decodebin name=decode ! videoconvert ! videoscale ! "
        "video/x-raw,format=BGRx,width=" G_STRINGIFY(PREVIEW_WIDTH) ",pixel-aspect-ratio=1/1 ! "
        "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false", &err);
    GstElement *src = p ? gst_device_create_element(C.devices[i], NULL) : NULL;
    GstElement *decode = p ? gst_bin_get_by_name(GST_BIN(p), "decode") : NULL;
    if (!p || !src || !gst_bin_add(GST_BIN(p), src) || !gst_element_link(src, decode)) {
        g_printerr("youcam: camera pipeline: %s\n", err ? err->message : "link failed");
        g_clear_error(&err);
        if (decode) gst_object_unref(decode);
        if (p) gst_object_unref(p);
        set_state(CAMERA_FAILED, "This camera could not be opened. Try another camera.");
        return;
    }
    gst_object_unref(decode);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    g_signal_connect(sink, "new-sample", G_CALLBACK(new_sample), NULL);
    gst_object_unref(sink);
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), NULL);
    gst_object_unref(bus);

    C.pipeline = p;
    C.configured = g_strdup(C.selected);
    background_set_camera(C.selected);
    set_state(CAMERA_STARTING, NULL);
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop_pipeline(1);
        set_state(CAMERA_FAILED, "The camera could not start. Choose Retry Camera from the menu.");
    }
}

static gint compare_names(gconstpointer a, gconstpointer b) {
    char *na = gst_device_get_display_name(*(GstDevice **)a);
    char *nb = gst_device_get_display_name(*(GstDevice **)b);
    int r = g_utf8_collate(na, nb);
    g_free(na);
    g_free(nb);
    return r;
}

/* Preferred camera, else the one in use, else the first one. */
static void refresh_devices(void) {
    for (int i = 0; i < C.count; i++) {
        gst_object_unref(C.devices[i]);
        g_free(C.names[i]);
    }
    C.count = 0;
    GList *list = C.provider ? gst_device_provider_get_devices(C.provider)
                             : gst_device_monitor_get_devices(C.monitor);
    GPtrArray *sorted = g_ptr_array_new();
    for (GList *l = list; l; l = l->next) g_ptr_array_add(sorted, l->data);
    g_ptr_array_sort(sorted, compare_names);
    for (guint i = 0; i < sorted->len && C.count < MAX_CAMERAS; i++) {
        GstDevice *d = sorted->pdata[i];
        if (!gst_device_has_classes(d, "Video/Source")) continue;
        char *name = gst_device_get_display_name(d);
        if (find_device(name) >= 0) {   /* Same model twice: keep both, distinctly. */
            char *n = g_strdup_printf("%s (%d)", name, C.count + 1);
            g_free(name);
            name = n;
        }
        C.devices[C.count] = gst_object_ref(d);
        C.names[C.count++] = name;
    }
    g_ptr_array_free(sorted, TRUE);
    g_list_free_full(list, gst_object_unref);

    int lost = C.configured && find_device(C.configured) < 0;
    const char *pick = find_device(Prefs.camera) >= 0 ? Prefs.camera
                     : find_device(C.selected) >= 0 ? C.selected
                     : C.count ? C.names[0] : NULL;
    prefs_set_string(&C.selected, pick);
    if (lost) {
        stop_pipeline(0);
        set_state(CAMERA_FAILED, "The camera was disconnected.");
    }
    if (C.wants_camera && !(lost && !C.selected)) start_selected();
    else if (C.on_change) C.on_change();
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_ADDED ||
        GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_REMOVED) refresh_devices();
    return G_SOURCE_CONTINUE;
}

void camera_init(void (*on_change)(void), void (*on_frame)(void)) {
    g_mutex_init(&C.lock);
    C.on_change = on_change;
    C.on_frame = on_frame;
    C.state = CAMERA_IDLE;
    C.provider = gst_device_provider_factory_get_by_name("v4l2deviceprovider");
    GstBus *bus;
    if (C.provider) {
        bus = gst_device_provider_get_bus(C.provider);
        gst_device_provider_start(C.provider);
    } else {
        C.monitor = gst_device_monitor_new();
        gst_device_monitor_add_filter(C.monitor, "Video/Source", NULL);
        bus = gst_device_monitor_get_bus(C.monitor);
        gst_device_monitor_start(C.monitor);
    }
    gst_bus_add_watch(bus, on_bus, NULL);
    gst_object_unref(bus);
    refresh_devices();
}

void camera_show(void) {
    C.wants_camera = 1;
    if (!C.pipeline) start_selected();
}

void camera_hide(void) {
    C.wants_camera = 0;
    stop_pipeline(0);
    set_state(CAMERA_IDLE, NULL);
}

void camera_retry(void) {
    stop_pipeline(0);
    C.wants_camera = 1;
    refresh_devices();
}

void camera_select(const char *name) {
    prefs_set_string(&Prefs.camera, name);
    prefs_save();
    prefs_set_string(&C.selected, name);
    if (C.wants_camera) start_selected();
}

void camera_shutdown(void) {
    C.wants_camera = 0;
    stop_pipeline(0);
    if (C.provider) gst_device_provider_stop(C.provider);
    if (C.monitor) gst_device_monitor_stop(C.monitor);
}

/* The overlay reads frames on the main thread: swap in the latest one. */
const camera_frame *camera_current_frame(void) {
    if (!C.pipeline || C.state != CAMERA_RUNNING) return NULL;
    g_mutex_lock(&C.lock);
    if (C.frame_ready) {
        camera_frame t = C.frame;
        C.frame = C.back;
        C.back = t;
        if (C.back.width != C.frame.width || C.back.height != C.frame.height) {
            g_free(C.back.data);
            C.back = (camera_frame){0, 0, 0, NULL};
        }
        C.frame_ready = 0;
    }
    g_mutex_unlock(&C.lock);
    return C.frame.data ? &C.frame : NULL;
}

camera_state_kind camera_state(void) { return C.state; }
int camera_device_count(void) { return C.count; }
const char *camera_device_name(int i) { return C.names[i]; }
const char *camera_selected_name(void) { return C.selected; }

const char *camera_menu_title(void) {
    static char buf[256];
    switch (C.state) {
    case CAMERA_IDLE: return "Camera paused";
    case CAMERA_STARTING:
        snprintf(buf, sizeof(buf), "Starting %s…", C.selected ? C.selected : "camera");
        return buf;
    case CAMERA_RUNNING: return C.selected ? C.selected : "Camera";
    case CAMERA_NONE: return "No camera connected";
    case CAMERA_FAILED: return "Camera unavailable";
    }
    return "";
}

/* Placeholder text when there is no video; returns 0 while video shows. */
int camera_message(const char **title, const char **detail) {
    switch (C.state) {
    case CAMERA_RUNNING: return 0;
    case CAMERA_IDLE: *title = "Camera paused"; *detail = "Show the camera from the menu."; break;
    case CAMERA_STARTING: *title = "Starting camera…"; *detail = "Your camera will appear here."; break;
    case CAMERA_NONE: *title = "No camera connected"; *detail = "Connect a camera to get started."; break;
    case CAMERA_FAILED: *title = "Camera unavailable"; *detail = C.detail ? C.detail : ""; break;
    }
    return 1;
}
