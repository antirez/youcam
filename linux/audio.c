/* Microphone selection, software gain and live level (AudioController.swift).
 *
 * Audio is always converted to mono 48 kHz float before it reaches the
 * encoder, whatever the device delivers. A pad probe applies the gain with
 * audio_gain_apply() so the slider changes the recorded samples and the meter
 * shows exactly what is recorded, including clipping.
 *
 * The microphone is opened only while something needs it: the open menu, the
 * recording panel, or a recording ("reasons", as in the Swift code). */

#include <string.h>
#include "youcam.h"

#define MAX_MICS 32
#define METER_INTERVAL_US 50000   /* 20 meter updates per second. */

static struct {
    GstDeviceMonitor *monitor;
    GstDevice *devices[MAX_MICS];
    char *names[MAX_MICS];
    int count;
    char *selected;               /* Display name, or "none". */
    char *status;
    GHashTable *reasons;
    GstElement *meter;            /* Monitoring pipeline for menu/panel. */
    audio_level level;
    void (*on_change)(void);
    void (*on_device_lost)(void);
    /* Shared with streaming threads. */
    GMutex lock;
    float gain;
    audio_level pending;
    gint64 last_post;
} A = {.gain = 1.0f};

static void changed(void) { if (A.on_change) A.on_change(); }

static void set_status(const char *s) { prefs_set_string(&A.status, s); }

int audio_has_audio(void) { return A.selected && strcmp(A.selected, "none") != 0; }

static int find_mic(const char *name) {
    for (int i = 0; name && i < A.count; i++)
        if (strcmp(A.names[i], name) == 0) return i;
    return -1;
}

const char *audio_name(void) {
    if (!audio_has_audio()) return "No Audio";
    return find_mic(A.selected) >= 0 ? A.selected : "Microphone unavailable";
}

const char *audio_status(void) { return A.status ? A.status : ""; }
audio_level audio_current_level(void) { return A.level; }
double audio_gain_db(void) { return Prefs.gain_db; }
int audio_device_count(void) { return A.count; }
const char *audio_device_name(int i) { return A.names[i]; }
const char *audio_selected_name(void) { return A.selected; }

/* ------------------------------------------------------------ gain probe */

static gboolean deliver_level(gpointer data) {
    (void)data;
    g_mutex_lock(&A.lock);
    audio_level l = A.pending;
    A.pending = (audio_level){0, 0, 0};
    g_mutex_unlock(&A.lock);
    if (A.reasons && g_hash_table_size(A.reasons) && audio_has_audio()) {
        A.level = l;
        mic_level_feed(l.source_peak);
        changed();
    }
    return G_SOURCE_REMOVE;
}

void audio_post_level(audio_level l) {
    g_mutex_lock(&A.lock);
    if (l.peak > A.pending.peak) A.pending.peak = l.peak;
    A.pending.clipped |= l.clipped;
    if (l.source_peak > A.pending.source_peak) A.pending.source_peak = l.source_peak;
    gint64 now = g_get_monotonic_time();
    int post = now - A.last_post >= METER_INTERVAL_US;
    if (post) A.last_post = now;
    g_mutex_unlock(&A.lock);
    if (post) g_idle_add(deliver_level, NULL);
}

static GstPadProbeReturn gain_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad; (void)data;
    GstBuffer *buf = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;
    g_mutex_lock(&A.lock);
    float gain = A.gain;
    g_mutex_unlock(&A.lock);
    audio_post_level(audio_gain_apply((float *)map.data, map.size / sizeof(float), gain));
    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

/* 'element' must output mono F32LE; its src pad gets the gain probe. */
void audio_install_gain_probe(GstElement *element) {
    GstPad *pad = gst_element_get_static_pad(element, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, gain_probe, NULL, NULL);
    gst_object_unref(pad);
}

void audio_set_gain(double db) {
    Prefs.gain_db = gain_db_clamp(db);
    prefs_save();
    g_mutex_lock(&A.lock);
    A.gain = gain_from_db(Prefs.gain_db);
    g_mutex_unlock(&A.lock);
    changed();
}

/* --------------------------------------------------------------- devices */

GstElement *audio_create_source(void) {
    int i = find_mic(A.selected);
    return i >= 0 ? gst_device_create_element(A.devices[i], NULL) : NULL;
}

static void stop_meter(void) {
    if (!A.meter) return;
    gst_element_set_state(A.meter, GST_STATE_NULL);
    gst_object_unref(A.meter);
    A.meter = NULL;
}

static void meter_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    GError *err = NULL;
    gst_message_parse_error(msg, &err, NULL);
    GstElement *p = A.meter;
    if (p) {
        A.meter = NULL;
        pipeline_stop_async(p);
        gst_object_unref(p);
    }
    set_status(err ? err->message : "The microphone stopped unexpectedly.");
    g_clear_error(&err);
    A.level = (audio_level){0, 0, 0};
    changed();
}

static void start_meter(void) {
    if (A.meter || !audio_has_audio()) return;
    GstElement *src = audio_create_source();
    if (!src) return;
    GError *err = NULL;
    GstElement *p = gst_parse_launch(
        "audioconvert name=convert ! audioresample ! "
        "audio/x-raw,format=F32LE,channels=1,rate=48000 ! identity name=gain ! "
        "fakesink sync=false", &err);
    GstElement *convert = p ? gst_bin_get_by_name(GST_BIN(p), "convert") : NULL;
    if (!p || !gst_bin_add(GST_BIN(p), src) || !gst_element_link(src, convert)) {
        set_status(err ? err->message : "The microphone could not be opened.");
        g_clear_error(&err);
        if (convert) gst_object_unref(convert);
        if (p) gst_object_unref(p);
        return;
    }
    gst_object_unref(convert);
    GstElement *gain = gst_bin_get_by_name(GST_BIN(p), "gain");
    audio_install_gain_probe(gain);
    gst_object_unref(gain);
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(meter_error), NULL);
    gst_object_unref(bus);
    A.meter = p;
    gst_element_set_state(p, GST_STATE_PLAYING);
}

/* The PipeWire object id of the selected microphone, for wpctl. */
static char *mic_node(void) {
    int i = find_mic(A.selected);
    GstStructure *p = i >= 0 ? gst_device_get_properties(A.devices[i]) : NULL;
    if (!p) return NULL;
    const GValue *v = gst_structure_get_value(p, "object.id");
    char *node = !v ? NULL : G_VALUE_HOLDS_STRING(v) ? g_value_dup_string(v)
               : G_VALUE_HOLDS_INT(v) ? g_strdup_printf("%d", g_value_get_int(v)) : NULL;
    gst_structure_free(p);
    return node;
}

/* The meter pipeline serves the menu and panel; a recording opens its own
 * source (PipeWire lets several clients share a microphone). */
static void update_monitoring(void) {
    int wants_meter = g_hash_table_contains(A.reasons, "menu") ||
                      g_hash_table_contains(A.reasons, "panel");
    if (wants_meter && audio_has_audio()) start_meter();
    else stop_meter();
    if (!g_hash_table_size(A.reasons) || !audio_has_audio()) {
        A.level = (audio_level){0, 0, 0};
        if (!A.status || strcmp(A.status, "Microphone disconnected") != 0)
            set_status(audio_has_audio() ? "Microphone idle" : "Audio off");
    } else if (find_mic(A.selected) >= 0) {
        set_status(A.selected);
    }
    char *node = g_hash_table_size(A.reasons) && audio_has_audio() ? mic_node() : NULL;
    mic_level_use(node);
    g_free(node);
    changed();
}

void audio_set_auto_mic_level(int on) {
    mic_level_use(NULL);               /* Put the level back before it is left alone. */
    Prefs.auto_mic_level = on;
    prefs_save();
    update_monitoring();
}

void audio_monitor(const char *reason, int enabled) {
    if (enabled) g_hash_table_add(A.reasons, g_strdup(reason));
    else g_hash_table_remove(A.reasons, reason);
    update_monitoring();
}

void audio_select(const char *name) {
    prefs_set_string(&A.selected, name);
    prefs_set_string(&Prefs.audio, name);
    prefs_save();
    stop_meter();
    set_status(audio_has_audio() ? "Microphone idle" : "Audio off");
    update_monitoring();
}

static int is_monitor_source(GstDevice *d) {
    GstStructure *p = gst_device_get_properties(d);
    int monitor = 0;
    if (p) {
        const char *cls = gst_structure_get_string(p, "device.class");
        monitor = cls && strcmp(cls, "monitor") == 0;
        gst_structure_free(p);
    }
    return monitor;
}

static int is_default(GstDevice *d) {
    GstStructure *p = gst_device_get_properties(d);
    gboolean def = FALSE;
    if (p) {
        gst_structure_get_boolean(p, "is-default", &def);
        gst_structure_free(p);
    }
    return def;
}

static void refresh_devices(void) {
    for (int i = 0; i < A.count; i++) {
        gst_object_unref(A.devices[i]);
        g_free(A.names[i]);
    }
    A.count = 0;
    char *fallback = NULL;
    GList *list = gst_device_monitor_get_devices(A.monitor);
    for (GList *l = list; l && A.count < MAX_MICS; l = l->next) {
        GstDevice *d = l->data;
        if (!gst_device_has_classes(d, "Audio/Source") || is_monitor_source(d)) continue;
        char *name = gst_device_get_display_name(d);
        if (find_mic(name) >= 0) {
            char *n = g_strdup_printf("%s (%d)", name, A.count + 1);
            g_free(name);
            name = n;
        }
        if (!fallback || is_default(d)) prefs_set_string(&fallback, name);
        A.devices[A.count] = gst_object_ref(d);
        A.names[A.count++] = name;
    }
    g_list_free_full(list, gst_object_unref);

    /* First run: the system default microphone, like macOS. */
    if (!A.selected) prefs_set_string(&A.selected, Prefs.audio ? Prefs.audio : fallback ? fallback : "none");
    g_free(fallback);

    int was_disconnected = A.status && strcmp(A.status, "Microphone disconnected") == 0;
    if (audio_has_audio() && find_mic(A.selected) < 0) {
        if (!was_disconnected) {
            stop_meter();
            mic_level_use(NULL);
            A.level = (audio_level){0, 0, 0};
            set_status("Microphone disconnected");
            if (A.on_device_lost) A.on_device_lost();
        }
    } else if (was_disconnected) {
        set_status("Microphone idle");
        update_monitoring();
    }
    changed();
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_ADDED ||
        GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_REMOVED) refresh_devices();
    return G_SOURCE_CONTINUE;
}

void audio_init(void (*on_change)(void), void (*on_device_lost)(void)) {
    mic_level_recover();
    g_mutex_init(&A.lock);
    A.on_change = on_change;
    A.on_device_lost = on_device_lost;
    A.reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    A.gain = gain_from_db(Prefs.gain_db);
    A.monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(A.monitor, "Audio/Source", NULL);
    GstBus *bus = gst_device_monitor_get_bus(A.monitor);
    gst_bus_add_watch(bus, on_bus, NULL);
    gst_object_unref(bus);
    gst_device_monitor_start(A.monitor);
    refresh_devices();
    set_status(audio_has_audio() ? "Microphone idle" : "Audio off");
}

void audio_shutdown(void) {
    g_hash_table_remove_all(A.reasons);
    stop_meter();
    mic_level_use(NULL);
    if (A.monitor) gst_device_monitor_stop(A.monitor);
}
