/* Recording controller (RecordingController.swift): phases, status text,
 * starting through the portal, pausing, stopping and failure handling.
 *
 * The portal lets the user pick which monitor to share and then remembers it.
 * YouCam knows which monitor the user means (the one under the camera, a
 * chosen display, or the one holding the selected area): when the remembered
 * stream sits elsewhere, the restore token is dropped and the portal asks
 * once more, so menu choices like "Whole Screen — HDMI-1" are respected. */

#include <string.h>
#include <unistd.h>
#include "youcam.h"

#define TICK_MS 250
#define LATER_UNAVAILABLE "Choose After Recording needs the FFV1 codec of gst-libav. " \
                          "This recording keeps the camera opaque."
#define LATER_FAILED "The camera could not be recorded on its own track. This recording keeps the camera opaque."
#define LATER_KEPT "The recording is kept: open it with Adjust Last Recording Transparency…"

static struct {
    recording_phase phase;
    writer *movie;
    char *session;
    int mutter;               /* The session is Mutter's (capture exclusion), not the portal's. */
    int later;                /* A project: the camera transparency is chosen after recording. */
    char *project;            /* The project being written, until the recording stops. */
    char *monitor;            /* Name of the monitor being recorded. */
    rect monitor_geometry;
    rect crop;
    int has_crop;
    int cancel_start;
    int retried;
    char *last_path;
    char *last_error;
    char *pending_error;      /* Reported once the movie is safely finished. */
    guint timer;
    char status[128];
    void (*on_change)(void);
    void (*on_error)(const char *);
    void (*on_project)(const char *path);
    void (*on_notice)(const char *message);
} R;

static void changed(void) { if (R.on_change) R.on_change(); }

/* Not a failure: the recording goes on, the user should know why. */
static void notice(const char *message) {
    g_printerr("youcam: %s\n", message);
    if (R.on_notice) R.on_notice(message);
}

/* Restored unconditionally: begin() hid the title with the Mutter state of
 * that attempt, and the retry may have read another one. */
static void leave_later(const char *why) {
    overlay_set_capture_hidden(0);
    R.later = 0;
    notice(why);
}

recording_phase recording_phase_now(void) { return R.phase; }
const char *recording_last_path(void) { return R.last_path; }

/* A movie finished elsewhere (the Choose After Recording export): any
 * earlier failure is over, the status says "Saved to Desktop". */
void recording_set_last_path(const char *path) {
    prefs_set_string(&R.last_path, path);
    prefs_set_string(&R.last_error, NULL);
    changed();
}
const char *recording_target_monitor(void) { return R.monitor; }

/* The menu and the dialog must never offer this file: Discard would delete
 * it mid-recording, Save would export half a movie. */
const char *recording_active_project(void) { return R.project; }

const char *recording_status(void) {
    char t[16];
    recording_time_label(R.movie ? writer_elapsed(R.movie) : 0, t, sizeof(t));
    switch (R.phase) {
    case PHASE_IDLE:
        return R.last_error ? "Recording stopped" : R.last_path ? "Saved to Desktop" : "Ready to record";
    case PHASE_STARTING: return "Starting recording…";
    case PHASE_RECORDING: snprintf(R.status, sizeof(R.status), "Recording · %s", t); return R.status;
    case PHASE_PAUSED: snprintf(R.status, sizeof(R.status), "Paused · %s", t); return R.status;
    case PHASE_FINISHING: return "Saving recording…";
    }
    return "";
}

static gboolean tick(gpointer data) {
    (void)data;
    changed();
    return G_SOURCE_CONTINUE;
}

static void close_session(void) {
    if (R.mutter) mutter_cast_stop(R.session);
    else portal_close_session(R.session);
    g_free(R.session);
    R.session = NULL;
}

static void close_fd(int fd) { if (fd >= 0) close(fd); }   /* Mutter streams come without one. */

static void complete_stop(void) {
    if (R.timer) g_source_remove(R.timer);
    R.timer = 0;
    close_session();
    R.movie = NULL;
    if (R.later) overlay_set_capture_hidden(0);
    R.later = 0;
    prefs_set_string(&R.project, NULL);
    R.phase = PHASE_IDLE;
    audio_monitor("recording", 0);
    changed();
}

static void report(const char *message) {
    prefs_set_string(&R.last_error, message);
    complete_stop();
    if (R.on_error) R.on_error(message);
}

static void movie_finished(const char *path, void *ud) {
    (void)ud;
    /* A project is not a movie yet: the dialog finishes it and sets the last path. */
    char *project = R.later ? g_strdup(path) : NULL;
    prefs_set_string(&R.last_path, project ? NULL : path);
    g_print("youcam: %s %s\n", project ? "project" : "saved", path);
    if (R.pending_error) {
        char *message = R.pending_error;
        R.pending_error = NULL;
        report(message);
        g_free(message);
    } else {
        complete_stop();
    }
    if (project && R.on_project) R.on_project(project);
    g_free(project);
}

/* The writer leaves a failed project in pending/: say where it went. */
static void movie_failed(const char *message, void *ud) {
    (void)ud;
    g_free(R.pending_error);
    R.pending_error = NULL;
    char *text = R.later ? g_strdup_printf("%s\n" LATER_KEPT, message) : g_strdup(message);
    report(text);
    g_free(text);
}

const char *recording_output_dir(void) {
    const char *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    return desktop && g_file_test(desktop, G_FILE_TEST_IS_DIR) ? desktop : g_get_home_dir();
}

/* A project when Choose After Recording is on; if its layer track cannot
 * start, an opaque movie as before: a recording is never lost to it. */
static writer *start_movie(const char *video, GstElement *mic, GError **err) {
    writer_callbacks cb = {movie_finished, movie_failed, NULL};
    const rect *crop = R.has_crop ? &R.crop : NULL;
    if (R.later) {
        double p = transparency_normalize(Prefs.transparency);
        project_info project = {R.mutter ? 1 : p, p, "", mic != NULL};
        g_strlcpy(project.container, container_for(Prefs.container)->ext, sizeof(project.container));
        char *dir = project_dir();
        char *path = g_mkdir_with_parents(dir, 0700) == 0 ? project_new_path(dir, time(NULL)) : NULL;
        writer *w = path ? writer_start(path, video, mic, crop, WRITER_LAYER_TRACK, &project, cb, err) : NULL;
        if (w) prefs_set_string(&R.project, path);
        g_free(path);
        g_free(dir);
        if (w) return w;
        g_printerr("youcam: project recording failed: %s\n", err && *err ? (*err)->message : "no pending folder");
        g_clear_error(err);
        leave_later(LATER_FAILED);
    }
    char *path = recording_next_path(recording_output_dir(), time(NULL), Prefs.container);
    writer *w = writer_start(path, video, mic, crop, WRITER_LAYER_COMPOSITE, NULL, cb, err);
    g_free(path);
    return w;
}

static void begin(void);

static void screen_failed(const char *message, void *ud) {
    (void)ud;
    if (R.cancel_start) {
        R.cancel_start = 0;
        complete_stop();
        return;
    }
    report(message);
}

static void screen_ready(int fd, guint32 node, const char *token, const char *session,
                         int has_pos, int x, int y, void *ud) {
    (void)ud;
    R.session = g_strdup(session);
    if (R.cancel_start) {
        close_fd(fd);
        R.cancel_start = 0;
        complete_stop();
        return;
    }
    GdkDisplay *display = gdk_display_get_default();
    int wrong_monitor = has_pos && gdk_display_get_n_monitors(display) > 1 &&
                        (x != (int)R.monitor_geometry.x || y != (int)R.monitor_geometry.y);
    if (wrong_monitor && !R.retried) {
        /* The remembered monitor is not the one asked for: ask again. */
        close_fd(fd);
        close_session();
        R.retried = 1;
        prefs_set_string(&Prefs.restore_token, NULL);
        begin();
        return;
    }
    if (token) {   /* Only the portal hands one back. */
        prefs_set_string(&Prefs.restore_token, token);
        prefs_save();
    }

    GstElement *mic = NULL;
    if (audio_has_audio()) {
        mic = audio_create_source();
        if (!mic) {
            close_fd(fd);
            report("The selected microphone is not available. Select another microphone or No Audio.");
            return;
        }
    }
    char *video = fd >= 0
        ? g_strdup_printf("pipewiresrc fd=%d path=%u do-timestamp=true always-copy=true keepalive-time=1000", fd, node)
        : g_strdup_printf("pipewiresrc path=%u do-timestamp=true always-copy=true keepalive-time=1000", node);
    GError *err = NULL;
    audio_monitor("recording", 1);
    /* Our own reference: a failed first attempt (start_movie) disposes its
     * pipeline, the microphone inside included, and the second needs it. */
    if (mic) gst_object_ref_sink(mic);
    R.movie = start_movie(video, mic, &err);
    if (mic) gst_object_unref(mic);
    g_free(video);
    if (!R.movie) {
        close_fd(fd);
        char *msg = g_strdup(err ? err->message : "The recording could not start.");
        g_clear_error(&err);
        report(msg);
        g_free(msg);
        return;
    }
    R.phase = PHASE_RECORDING;
    R.timer = g_timeout_add(TICK_MS, tick, NULL);
    changed();
}

static void begin(void) {
    R.mutter = mutter_cast_available();
    /* With the extension the camera stays on screen but out of the screen
     * track: any transparency can be chosen later (T_A = 1). */
    overlay_set_capture_hidden(R.later && R.mutter);   /* The retry may see another Mutter state. */
    if (R.mutter) mutter_cast_start(R.monitor, screen_ready, screen_failed, NULL);
    else portal_screencast_start(Prefs.restore_token, screen_ready, screen_failed, NULL);
}

void recording_start(GdkMonitor *monitor, const rect *local) {
    if (R.phase != PHASE_IDLE || !monitor) return;
    R.phase = PHASE_STARTING;
    R.cancel_start = 0;
    R.retried = 0;
    R.later = Prefs.saved_transparency == SAVED_LATER;
    if (R.later && !export_available()) {
        R.later = 0;
        notice(LATER_UNAVAILABLE);
    }
    prefs_set_string(&R.last_error, NULL);
    prefs_set_string(&R.monitor, monitor_name(monitor));
    R.monitor_geometry = monitor_rect(monitor);
    R.has_crop = local != NULL;
    if (local) {
        rect m = R.monitor_geometry;
        R.crop = (rect){local->x / m.w, local->y / m.h, local->w / m.w, local->h / m.h};
    }
    changed();
    begin();
}

void recording_toggle_pause(void) {
    if (R.phase == PHASE_RECORDING) {
        writer_pause(R.movie);
        R.phase = PHASE_PAUSED;
    } else if (R.phase == PHASE_PAUSED) {
        writer_resume(R.movie);
        R.phase = PHASE_RECORDING;
    } else {
        return;
    }
    changed();
}

void recording_stop(void) {
    if (R.phase == PHASE_STARTING) {
        R.cancel_start = 1;   /* The portal callback finishes the cancellation. */
        return;
    }
    if (R.phase != PHASE_RECORDING && R.phase != PHASE_PAUSED) return;
    if (R.later && R.mutter != mutter_cast_available())
        g_printerr("youcam: the Capture Exclusion extension changed during the recording; "
                   "the project keeps the state it had at the start\n");
    R.phase = PHASE_FINISHING;
    if (R.timer) g_source_remove(R.timer);
    R.timer = 0;
    changed();
    writer_finish(R.movie);
}

/* Something outside the pipeline went wrong (microphone unplugged, display
 * removed): keep what was recorded, then tell the user. */
void recording_fail(const char *message) {
    if (R.phase == PHASE_RECORDING || R.phase == PHASE_PAUSED) {
        prefs_set_string(&R.pending_error, message);
        recording_stop();
    }
}

/* What the camera layer needs: the writer, the recorded monitor (global)
 * and the recorded part of it (monitor-local). NULL when not recording. */
writer *recording_layer_target(rect *monitor, rect *region) {
    if (R.phase != PHASE_RECORDING || !R.movie) return NULL;
    rect m = R.monitor_geometry;
    *monitor = m;
    *region = R.has_crop ? (rect){R.crop.x * m.w, R.crop.y * m.h, R.crop.w * m.w, R.crop.h * m.h}
                         : (rect){0, 0, m.w, m.h};
    return R.movie;
}

void recording_init(void (*on_change)(void), void (*on_error)(const char *)) {
    R.on_change = on_change;
    R.on_error = on_error;
}

void recording_set_project_handlers(void (*on_project)(const char *path), void (*on_notice)(const char *message)) {
    R.on_project = on_project;
    R.on_notice = on_notice;
}

/* Without the extension the screen track holds the translucent preview:
 * the export needs it to stay at the T_A recorded at the start. */
int recording_transparency_locked(void) { return R.later && !R.mutter && PHASE_BUSY(R.phase); }
