/* Small pure helpers shared by recording, audio and panel code
 * (RecordingSupport.swift). Covered by tests.c. */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include "youcam.h"

/* Software gain that works with any microphone. Samples are clamped instead
 * of wrapping; clipping is reported both for the source and the result, so
 * the meter warns about a hot input even after the gain was lowered. */
audio_level audio_gain_apply(float *samples, size_t count, float gain) {
    audio_level level = {0, 0, 0};
    for (size_t i = 0; i < count; i++) {
        float in = isfinite(samples[i]) ? samples[i] : 0;
        if (fabsf(in) > level.source_peak) level.source_peak = fabsf(in);
        float out = in * gain;
        float mag = fabsf(out);
        if (mag > level.peak) level.peak = mag;
        if (fabsf(in) >= 0.999f || mag >= 0.999f) level.clipped = 1;
        samples[i] = fmaxf(-1, fminf(1, out));
    }
    return level;
}

float audio_level_db(audio_level level) {
    return level.peak > 0 ? fmaxf(-60, 20 * log10f(level.peak)) : -60;
}

double gain_db_clamp(double db) {
    if (!isfinite(db)) return 0;
    return fmin(GAIN_DB_MAX, fmax(GAIN_DB_MIN, db));
}

int noise_reduction_clamp(int level) {
    return level < 0 || level >= NOISE_REDUCTION_LEVELS ? NOISE_REDUCTION_DEFAULT : level;
}

float gain_from_db(double db) { return powf(10, gain_db_clamp(db) / 20); }

/* MOV first: the macOS default. MKV stays playable if YouCam is killed
 * mid-recording; MOV and MP4 need their index written at the end. */
const container Containers[CONTAINER_COUNT] = {
    {"mov", "qtmux", "MOV (QuickTime)"},
    {"mp4", "mp4mux", "MP4"},
    {"mkv", "matroskamux", "MKV (Matroska)"},
    {"avi", "avimux", "AVI"},
};

const container *container_for(const char *ext) {
    for (int i = 0; ext && i < CONTAINER_COUNT; i++)
        if (strcmp(ext, Containers[i].ext) == 0) return &Containers[i];
    return &Containers[0];
}

/* YouCam-Recording-<ddMMM-HH.mm.ss>. Month names are always English
 * (en_US_POSIX on macOS). Caller frees. */
char *recording_base_name(time_t when) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;
    localtime_r(&when, &tm);
    return g_strdup_printf("YouCam-Recording-%02d%s-%02d.%02d.%02d",
                           tm.tm_mday, months[tm.tm_mon], tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* <dir>/<base name>.<ext>, never overwriting: -2, -3, ... Caller frees. */
char *recording_next_path(const char *dir, time_t when, const char *ext) {
    char *base = recording_base_name(when);
    ext = container_for(ext)->ext;
    char *path = g_strdup_printf("%s/%s.%s", dir, base, ext);
    for (int suffix = 2; g_file_test(path, G_FILE_TEST_EXISTS); suffix++) {
        g_free(path);
        path = g_strdup_printf("%s/%s-%d.%s", dir, base, suffix, ext);
    }
    g_free(base);
    return path;
}

void recording_time_label(double seconds, char *buf, size_t len) {
    int s = seconds > 0 ? (int)seconds : 0;
    snprintf(buf, len, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

/* Fractions of the monitor -> videocrop sides. The output size is kept even,
 * as H.264 with 4:2:0 chroma requires. Fractions survive HiDPI: the portal
 * stream can have any pixel size. */
void crop_pixels(rect f, int fw, int fh, int *left, int *right, int *top, int *bottom) {
    int l = (int)lround(f.x * fw), t = (int)lround(f.y * fh);
    int w = (int)(f.w * fw) / 2 * 2, h = (int)(f.h * fh) / 2 * 2;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (l + w > fw) w = (fw - l) / 2 * 2;
    if (t + h > fh) h = (fh - t) / 2 * 2;
    *left = l; *top = t;
    *right = fw - l - w; *bottom = fh - t - h;
}

panel_buttons panel_buttons_for(recording_phase phase, int selecting) {
    panel_buttons b;
    b.play = (phase == PHASE_IDLE || phase == PHASE_PAUSED) && !selecting;
    b.pause = phase == PHASE_RECORDING;
    b.stop = phase == PHASE_STARTING || phase == PHASE_RECORDING || phase == PHASE_PAUSED;
    b.area = !PHASE_BUSY(phase) && !selecting;
    return b;
}
