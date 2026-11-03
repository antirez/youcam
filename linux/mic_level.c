/* Automatic microphone level (Linux addition).
 *
 * YouCam's gain only scales what the microphone already delivered: a
 * capture level set too high clips before YouCam sees it, one set too low
 * buries speech in noise. While YouCam uses the microphone, this adjusts
 * the system level of that source (through wpctl) from the peaks it sees,
 * and puts the original level back when it stops. The original is kept in
 * the preferences too, so a crash does not leave the microphone changed. */

#include <math.h>
#include <string.h>
#include "youcam.h"

#define MIC_AGC_CLIP_LEVEL 0.999f
#define MIC_AGC_HIGH_PEAK 0.501f      /* -6 dBFS: too close to clipping. */
#define MIC_AGC_LOW_PEAK 0.126f       /* -18 dBFS: speech too quiet. */
#define MIC_AGC_GATE_PEAK 0.0056f     /* -45 dBFS: silence, never raise for it. */
#define MIC_AGC_WINDOW_US (2 * G_USEC_PER_SEC)
#define MIC_AGC_HOLD_US (G_USEC_PER_SEC / 2)   /* Let a change reach the meter. */
#define WPCTL_CUBIC_DB 60.0           /* wpctl volumes are cubic: dB = 60 log10(v). */

/* Returns the change in dB to apply now, 0 for none. Clipping lowers the
 * level at once; otherwise the loudest peak of each 2 s window decides. */
double mic_agc_step(mic_agc *a, float peak, gint64 now) {
    if (now < a->hold_until) return 0;
    if (peak >= MIC_AGC_CLIP_LEVEL) {
        *a = (mic_agc){now + MIC_AGC_HOLD_US, now + MIC_AGC_HOLD_US, 0};
        return MIC_AGC_CLIP_STEP_DB;
    }
    if (!a->window_start) a->window_start = now;
    if (peak > a->window_peak) a->window_peak = peak;
    if (now - a->window_start < MIC_AGC_WINDOW_US) return 0;
    float p = a->window_peak;
    double db = p > MIC_AGC_HIGH_PEAK ? -MIC_AGC_STEP_DB
              : p < MIC_AGC_LOW_PEAK && p > MIC_AGC_GATE_PEAK ? MIC_AGC_STEP_DB : 0;
    *a = (mic_agc){now, db ? now + MIC_AGC_HOLD_US : 0, 0};
    return db;
}

double mic_volume_after(double v, double db) {
    double r = v * pow(10, db / WPCTL_CUBIC_DB);
    return r < MIC_VOLUME_MIN ? MIC_VOLUME_MIN : r > MIC_VOLUME_MAX ? MIC_VOLUME_MAX : r;
}

/* "Volume: 0.22 [MUTED]". Not sscanf: the decimal point must not follow the locale. */
double mic_volume_parse(const char *out) {
    const char *prefix = "Volume: ";
    if (!out || !g_str_has_prefix(out, prefix)) return -1;
    char *end;
    double v = g_ascii_strtod(out + strlen(prefix), &end);
    return end != out + strlen(prefix) && isfinite(v) && v >= 0 ? v : -1;
}

static struct { char *node; double original, volume; mic_agc agc; } M;

static double get_volume(const char *node) {
    char *argv[] = {"wpctl", "get-volume", (char *)node, NULL}, *out = NULL;
    double v = -1;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, NULL, NULL))
        v = mic_volume_parse(out);
    g_free(out);
    return v;
}

static void set_volume(const char *node, double v, int wait) {
    char vol[32];
    g_ascii_formatd(vol, sizeof(vol), "%.4f", v);
    char *argv[] = {"wpctl", "set-volume", (char *)node, vol, NULL};
    GError *err = NULL;
    int ok = wait ? g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, &err)
                  : g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    if (!ok) g_printerr("youcam: could not set the microphone level: %s\n", err->message);
    g_clear_error(&err);
}

static void remember(const char *node, double v) {
    char vol[G_ASCII_DTOSTR_BUF_SIZE];
    char *r = node ? g_strconcat(node, ":", g_ascii_formatd(vol, sizeof(vol), "%.4f", v), NULL) : NULL;
    prefs_set_string(&Prefs.mic_restore, r);
    g_free(r);
    prefs_save();
}

/* Starts adjusting the source 'node' (a PipeWire object id), or with NULL
 * stops and puts back the level it had. */
void mic_level_use(const char *node) {
    if (!Prefs.auto_mic_level) node = NULL;
    if (g_strcmp0(node, M.node) == 0) return;
    if (M.node) {
        set_volume(M.node, M.original, 1);
        g_clear_pointer(&M.node, g_free);
        remember(NULL, 0);
    }
    double v = node ? get_volume(node) : -1;
    if (v <= 0) return;               /* Unknown, or turned all the way down on purpose. */
    M.node = g_strdup(node);
    M.original = M.volume = v;
    M.agc = (mic_agc){0};
    remember(node, v);
}

void mic_level_feed(float source_peak) {
    if (!M.node) return;
    double db = mic_agc_step(&M.agc, source_peak, g_get_monotonic_time());
    double v = db ? mic_volume_after(M.volume, db) : M.volume;
    if (v == M.volume) return;
    M.volume = v;
    set_volume(M.node, v, 0);
}

/* A level left changed by a crash goes back at the next start. */
void mic_level_recover(void) {
    const char *colon = Prefs.mic_restore ? strrchr(Prefs.mic_restore, ':') : NULL;
    if (!colon) return;
    char *node = g_strndup(Prefs.mic_restore, colon - Prefs.mic_restore);
    double v = g_ascii_strtod(colon + 1, NULL);
    if (v > 0 && v <= MIC_VOLUME_MAX) set_volume(node, v, 1);
    g_free(node);
    remember(NULL, 0);
}
