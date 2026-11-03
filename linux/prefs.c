/* Preferences (UserDefaults + RecordingAreaPreferences.swift).
 *
 * A key file in ~/.config/youcam/prefs.ini. The recording area stores the
 * monitor connector name rather than an index, since indexes change when
 * displays are plugged in a different order. */

#include <math.h>
#include <string.h>
#include "youcam.h"

prefs Prefs;
static char *PrefsPath;

static const char *AreaModes[] = {"wholeScreen", "display", "region"};

const char *const SavedTransparencyNames[SAVED_COUNT] = {"preview", "opaque", "later"};

/* savedTransparency replaced the check item "Apply Transparency to Saved
 * Video" (recordCameraTransparency): checked kept the preview transparency,
 * unchecked an opaque camera. A fresh install and unknown values keep
 * today's default, an opaque camera. */
saved_transparency saved_transparency_parse(const char *value, int has_legacy, int legacy) {
    if (value) {
        for (int i = 0; i < SAVED_COUNT; i++)
            if (strcmp(value, SavedTransparencyNames[i]) == 0) return (saved_transparency)i;
        return SAVED_OPAQUE;
    }
    if (has_legacy) return legacy ? SAVED_PREVIEW : SAVED_OPAQUE;
    return SAVED_OPAQUE;
}

void prefs_set_string(char **field, const char *value) {
    char *copy = value ? g_strdup(value) : NULL;
    g_free(*field);
    *field = copy;
}

static double key_double(GKeyFile *kf, const char *group, const char *key, double fallback, int *found) {
    GError *err = NULL;
    double v = g_key_file_get_double(kf, group, key, &err);
    if (err) { g_error_free(err); if (found) *found = 0; return fallback; }
    if (found) *found = 1;
    return v;
}

void prefs_load(void) {
    if (!PrefsPath) {
        const char *env = g_getenv("YOUCAM_CONFIG");   /* Tests and smoke runs. */
        PrefsPath = env ? g_strdup(env)
                        : g_build_filename(g_get_user_config_dir(), "youcam", "prefs.ini", NULL);
    }
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(kf, PrefsPath, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("youcam: ignoring unreadable preferences: %s\n", err->message);
        g_clear_error(&err);
    }
    char *shape = g_key_file_get_string(kf, "youcam", "shape", NULL);
    Prefs.shape = shape_from_name(shape);
    g_free(shape);

    int found = 0;
    Prefs.mirrored = (int)key_double(kf, "youcam", "mirrored", 1, NULL);
    Prefs.width = key_double(kf, "youcam", "width", OVERLAY_DEFAULT_WIDTH, NULL);
    double ox = key_double(kf, "youcam", "originX", 0, &found);
    int has_x = found;
    double oy = key_double(kf, "youcam", "originY", 0, &found);
    Prefs.has_origin = has_x && found && isfinite(ox) && isfinite(oy);
    Prefs.origin_x = ox;
    Prefs.origin_y = oy;
    Prefs.camera = g_key_file_get_string(kf, "youcam", "camera", NULL);
    Prefs.audio = g_key_file_get_string(kf, "youcam", "audio", NULL);
    Prefs.gain_db = gain_db_clamp(key_double(kf, "youcam", "audioGainDB", 0, NULL));
    Prefs.restore_token = g_key_file_get_string(kf, "youcam", "restoreToken", NULL);
    Prefs.transparency = transparency_normalize(key_double(kf, "youcam", "cameraPreviewTransparency", 0, NULL));
    char *saved = g_key_file_get_string(kf, "youcam", "savedTransparency", NULL);
    GError *legacy_err = NULL;
    gboolean legacy = g_key_file_get_boolean(kf, "youcam", "recordCameraTransparency", &legacy_err);
    Prefs.saved_transparency = saved_transparency_parse(saved, legacy_err == NULL, legacy);
    g_clear_error(&legacy_err);
    g_free(saved);
    Prefs.center_stage = (int)key_double(kf, "youcam", "centerStage", 1, NULL);
    Prefs.container = g_key_file_get_string(kf, "youcam", "container", NULL);
    Prefs.auto_mic_level = (int)key_double(kf, "youcam", "autoMicLevel", 1, NULL);
    Prefs.mic_restore = g_key_file_get_string(kf, "youcam", "micVolumeRestore", NULL);
    char *background = g_key_file_get_string(kf, "youcam", "backgroundMode", NULL);
    Prefs.background_mode = background_mode_parse(background);
    g_free(background);
    Prefs.background_image = g_key_file_get_string(kf, "youcam", "backgroundImage", NULL);
    Prefs.background_color = g_key_file_get_string(kf, "youcam", "backgroundColor", NULL);
    Prefs.noise_reduction = noise_reduction_clamp((int)key_double(kf, "youcam", "noiseReduction",
                                                                 NOISE_REDUCTION_DEFAULT, NULL));

    char *mode = g_key_file_get_string(kf, "area", "mode", NULL);
    char *monitor = g_key_file_get_string(kf, "area", "monitor", NULL);
    memset(&Prefs.area, 0, sizeof(Prefs.area));
    for (int i = 0; mode && i < 3; i++)
        if (strcmp(mode, AreaModes[i]) == 0) Prefs.area.mode = i;
    if (monitor) g_strlcpy(Prefs.area.monitor, monitor, sizeof(Prefs.area.monitor));
    Prefs.area.local = (rect){key_double(kf, "area", "x", 0, NULL), key_double(kf, "area", "y", 0, NULL),
                              key_double(kf, "area", "w", 0, NULL), key_double(kf, "area", "h", 0, NULL)};
    g_free(mode);
    g_free(monitor);
    g_key_file_free(kf);
}

static void set_string(GKeyFile *kf, const char *group, const char *key, const char *value) {
    if (value) g_key_file_set_string(kf, group, key, value);
}

void prefs_save(void) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "youcam", "shape", shape_name(Prefs.shape));
    g_key_file_set_double(kf, "youcam", "mirrored", Prefs.mirrored);
    g_key_file_set_double(kf, "youcam", "width", Prefs.width);
    if (Prefs.has_origin) {
        g_key_file_set_double(kf, "youcam", "originX", Prefs.origin_x);
        g_key_file_set_double(kf, "youcam", "originY", Prefs.origin_y);
    }
    set_string(kf, "youcam", "camera", Prefs.camera);
    set_string(kf, "youcam", "audio", Prefs.audio);
    g_key_file_set_double(kf, "youcam", "audioGainDB", Prefs.gain_db);
    set_string(kf, "youcam", "restoreToken", Prefs.restore_token);
    g_key_file_set_double(kf, "youcam", "cameraPreviewTransparency", Prefs.transparency);
    g_key_file_set_string(kf, "youcam", "savedTransparency", SavedTransparencyNames[Prefs.saved_transparency]);
    g_key_file_set_integer(kf, "youcam", "noiseReduction", Prefs.noise_reduction);
    g_key_file_set_string(kf, "youcam", "container", container_for(Prefs.container)->ext);
    g_key_file_set_double(kf, "youcam", "centerStage", Prefs.center_stage);
    g_key_file_set_double(kf, "youcam", "autoMicLevel", Prefs.auto_mic_level);
    set_string(kf, "youcam", "micVolumeRestore", Prefs.mic_restore);
    g_key_file_set_string(kf, "youcam", "backgroundMode", BackgroundNames[Prefs.background_mode]);
    set_string(kf, "youcam", "backgroundImage", Prefs.background_image);
    set_string(kf, "youcam", "backgroundColor", Prefs.background_color);
    g_key_file_set_string(kf, "area", "mode", AreaModes[Prefs.area.mode]);
    if (Prefs.area.mode != AREA_WHOLE_SCREEN) {
        g_key_file_set_string(kf, "area", "monitor", Prefs.area.monitor);
        g_key_file_set_double(kf, "area", "x", Prefs.area.local.x);
        g_key_file_set_double(kf, "area", "y", Prefs.area.local.y);
        g_key_file_set_double(kf, "area", "w", Prefs.area.local.w);
        g_key_file_set_double(kf, "area", "h", Prefs.area.local.h);
    }
    char *dir = g_path_get_dirname(PrefsPath);
    GError *err = NULL;
    if (g_mkdir_with_parents(dir, 0700) != 0 || !g_key_file_save_to_file(kf, PrefsPath, &err)) {
        g_printerr("youcam: could not save preferences: %s\n", err ? err->message : "no directory");
        g_clear_error(&err);
    }
    g_free(dir);
    g_key_file_free(kf);
}

/* A saved area is used only if its monitor is still connected and the rect
 * still fits on it; anything else falls back to the whole screen. */
recording_area prefs_area_validate(recording_area a, GdkDisplay *display) {
    recording_area whole = {AREA_WHOLE_SCREEN, "", {0, 0, 0, 0}};
    if (a.mode == AREA_WHOLE_SCREEN || !display) return whole;
    GdkMonitor *m = monitor_by_name(a.monitor);
    if (!m) return whole;
    if (a.mode == AREA_DISPLAY) return a;
    rect mr = monitor_rect(m);
    rect bounds = {0, 0, mr.w, mr.h};
    rect r = a.local;
    if (!isfinite(r.x) || !isfinite(r.y) || !isfinite(r.w) || !isfinite(r.h) ||
        r.w < AREA_MIN_SIZE || r.h < AREA_MIN_SIZE || !rect_contains(bounds, r))
        return whole;
    return a;
}

/* ---------------------------------------------------------- monitors */

const char *monitor_name(GdkMonitor *m) {
    const char *model = gdk_monitor_get_model(m);
    return model && *model ? model : "Display";
}

GdkMonitor *monitor_by_name(const char *name) {
    GdkDisplay *d = gdk_display_get_default();
    for (int i = 0; d && name && i < gdk_display_get_n_monitors(d); i++) {
        GdkMonitor *m = gdk_display_get_monitor(d, i);
        if (strcmp(monitor_name(m), name) == 0) return m;
    }
    return NULL;
}

rect monitor_rect(GdkMonitor *m) {
    GdkRectangle g;
    gdk_monitor_get_geometry(m, &g);
    return (rect){g.x, g.y, g.width, g.height};
}

rect monitor_workarea(GdkMonitor *m) {
    GdkRectangle g;
    gdk_monitor_get_workarea(m, &g);
    return (rect){g.x, g.y, g.width, g.height};
}
