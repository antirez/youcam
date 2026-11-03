/* Logic checks ported from the Swift tests in Tests/ (the parts that are not AppKit UI).
 * Coordinates are GDK's (top-left origin): the macOS cases are mirrored
 * vertically inside a 1440x900 display. */

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>
#include <glib/gstdio.h>
#include "youcam.h"

static int Checks;

#define EXPECT(cond, msg) do { \
    Checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } \
} while (0)

static int near(double a, double b) { return fabs(a - b) < 0.001; }

static void test_overlay_geometry(void) {
    rect screen = {0, 0, 1920, 1080};
    double w, h;
    overlay_size(10, SHAPE_CIRCLE, screen, &w, &h);
    EXPECT(w == 120 && h == 120, "The camera has a usable minimum size");
    overlay_size(5000, SHAPE_RECTANGLE, screen, &w, &h);
    EXPECT(w == 960 && near(h, 540), "The camera has a maximum size and keeps 16:9");
    overlay_size(NAN, SHAPE_SQUARE, screen, &w, &h);
    EXPECT(w == 280, "A corrupt saved width falls back to the default");
    rect small = {0, 0, 300, 200};
    overlay_size(400, SHAPE_SQUARE, small, &w, &h);
    EXPECT(w == 200, "The camera never exceeds the screen");
    rect r = overlay_initial(280, SHAPE_CIRCLE, screen);
    EXPECT(rect_equal(r, (rect){1920 - 280 - 32, 1080 - 280 - 32, 280, 280}),
           "The camera starts in the bottom right corner");
    r = overlay_resized((rect){100, 100, 280, 280}, 400, SHAPE_RECTANGLE, screen);
    EXPECT(r.x == 100 && r.y == 100 && r.w == 400 && near(r.h, 225), "Resizing keeps the top-left corner still");
    r = overlay_constrain((rect){1900, -50, 200, 200}, screen);
    EXPECT(rect_equal(r, (rect){1720, 0, 200, 200}), "The camera is kept on screen");
    rect offset = {-1920, 0, 1920, 1080};
    r = overlay_resized((rect){-100, 900, 280, 280}, 280, SHAPE_CIRCLE, offset);
    EXPECT(r.x == -280 && r.y == 800, "Constraining works on displays left of the primary one");
    EXPECT(rect_intersection_area((rect){0, 0, 10, 10}, (rect){5, 5, 10, 10}) == 25, "Intersection area");
    EXPECT(rect_intersection_area((rect){0, 0, 10, 10}, (rect){20, 0, 10, 10}) == 0, "Disjoint frames do not intersect");
}

/* RecordingAreaTests.swift, first block, mirrored to a top-left origin. */
static void test_area_corners(void) {
    struct { area_corner corner; double px, py; rect expected; area_corner opposite; } cases[] = {
        {CORNER_TOP_LEFT, 80, 550, {80, 550, 340, 230}, CORNER_BOTTOM_RIGHT},
        {CORNER_TOP_RIGHT, 500, 550, {100, 550, 400, 230}, CORNER_BOTTOM_LEFT},
        {CORNER_BOTTOM_LEFT, 80, 810, {80, 600, 340, 210}, CORNER_TOP_RIGHT},
        {CORNER_BOTTOM_RIGHT, 500, 810, {100, 600, 400, 210}, CORNER_TOP_LEFT},
    };
    double origins[][2] = {{0, 0}, {-1920, -400}, {200, 1080}};
    for (int o = 0; o < 3; o++) {
        double ox = origins[o][0], oy = origins[o][1];
        rect display = {ox, oy, 1440, 900};
        rect r = {ox + 100, oy + 600, 320, 180};
        for (int i = 0; i < 4; i++) {
            area_corner c = cases[i].corner, opp = cases[i].opposite;
            double px = ox + cases[i].px, py = oy + cases[i].py;
            rect e = cases[i].expected;
            e.x += ox; e.y += oy;
            EXPECT(rect_equal(corner_resized(c, r, px, py, display, 2), e),
                   "Each corner adjusts width and height independently on offset displays");

            double ax, ay, cx, cy;
            corner_point(opp, r, &ax, &ay);
            corner_point(c, r, &cx, &cy);
            double outx = cx + (cx < ax ? -10000 : 10000), outy = cy + (cy < ay ? -10000 : 10000);
            rect expanded = corner_resized(c, r, outx, outy, display, 1);
            double ex, ey;
            corner_point(opp, expanded, &ex, &ey);
            EXPECT(rect_contains(display, expanded), "Dragging beyond the display keeps the region on that display");
            EXPECT(ex == ax && ey == ay, "Clamping keeps the opposite corner fixed");

            double crossx = ax + (cx < ax ? 100 : -100), crossy = ay + (cy < ay ? 100 : -100);
            rect minimum = corner_resized(c, r, crossx, crossy, display, 2);
            corner_point(opp, minimum, &ex, &ey);
            EXPECT(minimum.w == AREA_MIN_SIZE && minimum.h == AREA_MIN_SIZE,
                   "Dragging across the opposite corner retains a usable minimum");
            EXPECT(ex == ax && ey == ay, "Shrinking keeps the opposite corner fixed");

            rect aligned = corner_resized(c, r, px + 0.26, py + 0.26, display, 2);
            corner_point(c, aligned, &ex, &ey);
            EXPECT(ex == px + 0.5 && ey == py + 0.5, "HiDPI dragging aligns to physical pixels");
        }
    }
}

/* RecordingTests.swift: gain, clipping, file naming. */
static void test_audio_gain(void) {
    float quiet[] = {0, 0.1f, -0.25f, 0.5f};
    audio_level l = audio_gain_apply(quiet, 4, 0.5f);
    EXPECT(quiet[0] == 0 && quiet[1] == 0.05f && quiet[2] == -0.125f && quiet[3] == 0.25f,
           "The input slider changes actual recorded samples");
    EXPECT(!l.clipped && near(l.peak, 0.25), "The meter reports the gain-adjusted signal");
    EXPECT(near(l.source_peak, 0.5), "The source peak is measured before the gain");
    float loud[] = {-0.8f, 0.8f};
    l = audio_gain_apply(loud, 2, 2);
    EXPECT(loud[0] == -1 && loud[1] == 1 && l.clipped, "Gain cannot wrap or overflow audio samples");
    float hot[] = {1};
    EXPECT(audio_gain_apply(hot, 1, 0.1f).clipped, "The meter warns about source clipping even after reducing gain");
    float invalid[] = {NAN, INFINITY};
    audio_gain_apply(invalid, 2, 1);
    EXPECT(invalid[0] == 0 && invalid[1] == 0, "Invalid input samples do not corrupt the audio encoder");
    EXPECT(audio_level_db((audio_level){0, 0, 0}) == -60, "Silence reads -60 dB");
    EXPECT(near(audio_level_db((audio_level){1, 0, 0}), 0), "Full scale reads 0 dB");
    EXPECT(gain_db_clamp(100) == GAIN_DB_MAX && gain_db_clamp(-100) == GAIN_DB_MIN && gain_db_clamp(NAN) == 0,
           "Gain stays within -36...+12 dB");
    EXPECT(near(gain_from_db(0), 1), "0 dB is unity gain");
    EXPECT(noise_reduction_clamp(0) == 0 && noise_reduction_clamp(4) == 4,
           "Noise reduction keeps Off...Very High");
    EXPECT(noise_reduction_clamp(-1) == NOISE_REDUCTION_DEFAULT && noise_reduction_clamp(9) == NOISE_REDUCTION_DEFAULT,
           "A corrupt noise reduction preference falls back to Moderate");
}

static void test_files_and_labels(void) {
    char *dir = g_dir_make_tmp("youcam-tests-XXXXXX", NULL);
    time_t when = 1790066001;
    char *first = recording_next_path(dir, when, NULL);
    g_file_set_contents(first, "existing movie", -1, NULL);
    char *second = recording_next_path(dir, when, "mov");
    EXPECT(strcmp(first, second) != 0 && g_str_has_suffix(second, "-2.mov"),
           "Automatic file naming never overwrites an existing recording");
    EXPECT(!strchr(strrchr(second, '/'), ':'), "Recording filenames are safe in file managers");
    char *mkv = recording_next_path(dir, when, "mkv");
    EXPECT(g_str_has_suffix(mkv, ".mkv") && !g_str_has_suffix(mkv, "-2.mkv"),
           "The chosen container sets the extension; another format does not collide");
    g_free(mkv);
    EXPECT(strcmp(container_for(NULL)->ext, "mov") == 0 && strcmp(container_for("webm")->ext, "mov") == 0,
           "A missing or unknown container preference records MOV, like macOS");
    EXPECT(strcmp(container_for("avi")->muxer, "avimux") == 0, "AVI uses the AVI muxer");
    EXPECT(strstr(first, "/YouCam-Recording-") != NULL, "Movies use the macOS naming scheme");
    g_unlink(first);
    g_rmdir(dir);
    g_free(first); g_free(second); g_free(dir);

    char label[16];
    recording_time_label(3725.9, label, sizeof(label));
    EXPECT(strcmp(label, "01:02:05") == 0, "The timer shows hours, minutes and seconds");
    recording_time_label(-3, label, sizeof(label));
    EXPECT(strcmp(label, "00:00:00") == 0, "The timer never goes negative");
}

static void test_crop(void) {
    int l, r, t, b;
    crop_pixels((rect){0.25, 0.25, 0.5, 0.5}, 3840, 2160, &l, &r, &t, &b);
    EXPECT(l == 960 && r == 960 && t == 540 && b == 540, "HiDPI streams crop the same logical area");
    crop_pixels((rect){0.1, 0.1, 0.333, 0.333}, 1001, 999, &l, &r, &t, &b);
    EXPECT((1001 - l - r) % 2 == 0 && (999 - t - b) % 2 == 0, "Large displays produce encoder-compatible dimensions");
    crop_pixels((rect){0.9, 0.9, 0.1, 0.1}, 100, 100, &l, &r, &t, &b);
    EXPECT(l == 90 && r == 0 && t == 90 && b == 0, "Crops at the edge stay inside the frame");
}

/* RecordingPanelTests.swift: transport buttons offer only valid actions. */
static void test_panel_buttons(void) {
    struct { recording_phase phase; int play, pause, stop; } cases[] = {
        {PHASE_IDLE, 1, 0, 0}, {PHASE_STARTING, 0, 0, 1}, {PHASE_RECORDING, 0, 1, 1},
        {PHASE_PAUSED, 1, 0, 1}, {PHASE_FINISHING, 0, 0, 0},
    };
    for (int i = 0; i < 5; i++) {
        panel_buttons b = panel_buttons_for(cases[i].phase, 0);
        EXPECT(b.play == cases[i].play && b.pause == cases[i].pause && b.stop == cases[i].stop,
               "Transport buttons offer only valid actions");
        EXPECT(b.area == !PHASE_BUSY(cases[i].phase), "The area stays locked while busy");
    }
    panel_buttons b = panel_buttons_for(PHASE_IDLE, 1);
    EXPECT(!b.play && !b.area, "Selecting a region prevents recording before the selection is complete");
}

/* RecordingAreaPreferences: corrupt or impossible areas fall back to the
 * whole screen. Needs a display, so it is skipped when there is none. */
static void test_prefs(void) {
    char *dir = g_dir_make_tmp("youcam-prefs-XXXXXX", NULL);
    char *path = g_build_filename(dir, "prefs.ini", NULL);
    g_setenv("YOUCAM_CONFIG", path, TRUE);
    prefs_load();
    EXPECT(Prefs.shape == SHAPE_CIRCLE && Prefs.mirrored && Prefs.width == OVERLAY_DEFAULT_WIDTH,
           "Defaults: circle, mirrored, 280 points");
    EXPECT(Prefs.center_stage, "Center Stage is enabled by default");
    EXPECT(Prefs.auto_mic_level, "The automatic microphone level is on by default");
    EXPECT(Prefs.background_mode == BG_OFF, "Background removal is off by default");
    Prefs.shape = SHAPE_RECTANGLE;
    Prefs.gain_db = -6;
    Prefs.has_origin = 1; Prefs.origin_x = 12; Prefs.origin_y = 34;
    Prefs.area = (recording_area){AREA_REGION, "HDMI-1", {10, 20, 300, 200}};
    prefs_set_string(&Prefs.restore_token, "token");
    Prefs.transparency = 0.55;
    Prefs.saved_transparency = SAVED_LATER;
    Prefs.center_stage = 0;
    Prefs.background_mode = BG_BLUR;
    prefs_set_string(&Prefs.background_color, "#102030");
    prefs_save();
    memset(&Prefs, 0, sizeof(Prefs));
    prefs_load();
    EXPECT(Prefs.shape == SHAPE_RECTANGLE && Prefs.gain_db == -6 && Prefs.origin_y == 34,
           "Preferences survive a reload");
    EXPECT(Prefs.area.mode == AREA_REGION && strcmp(Prefs.area.monitor, "HDMI-1") == 0 &&
           rect_equal(Prefs.area.local, (rect){10, 20, 300, 200}), "The recording area survives a reload");
    EXPECT(strcmp(Prefs.restore_token, "token") == 0, "The shared monitor is remembered");
    EXPECT(Prefs.transparency == 0.55 && Prefs.saved_transparency == SAVED_LATER, "Both transparency choices are remembered");
    EXPECT(!Prefs.center_stage, "Turning Center Stage off is remembered");
    EXPECT(Prefs.background_mode == BG_BLUR && strcmp(Prefs.background_color, "#102030") == 0,
           "The background choice is remembered");
    recording_area missing = prefs_area_validate(Prefs.area, NULL);
    EXPECT(missing.mode == AREA_WHOLE_SCREEN, "An area on a disconnected display falls back to the whole screen");
    g_file_set_contents(path, "[youcam]\nwidth=banana\ncameraPreviewTransparency=5\n", -1, NULL);
    prefs_load();
    EXPECT(Prefs.width == OVERLAY_DEFAULT_WIDTH, "Corrupt values fall back to defaults");
    EXPECT(Prefs.transparency == TRANSPARENCY_MAX && Prefs.saved_transparency == SAVED_OPAQUE,
           "Saved transparency is normalized; the saved video stays opaque by default");
    /* An old prefs.ini migrates, and the old key is gone after the next save. */
    g_file_set_contents(path, "[youcam]\nrecordCameraTransparency=false\n", -1, NULL);
    prefs_load();
    EXPECT(Prefs.saved_transparency == SAVED_OPAQUE, "An unchecked Apply Transparency to Saved Video becomes Opaque");
    prefs_save();
    char *saved = NULL;
    g_file_get_contents(path, &saved, NULL, NULL);
    EXPECT(saved && strstr(saved, "savedTransparency=opaque") && !strstr(saved, "recordCameraTransparency"),
           "The old key is replaced by savedTransparency on the next save");
    g_free(saved);
    g_unlink(path);
    g_rmdir(dir);
    g_free(path); g_free(dir);
}

/* CameraTransparencyTests.swift: preferences and the opaque camera copy. */
static void test_transparency(void) {
    double inputs[][2] = {{-1, 0}, {2, 0.8}, {NAN, 0}, {INFINITY, 0}, {0.55, 0.55}};
    for (int i = 0; i < 5; i++)
        EXPECT(transparency_normalize(inputs[i][0]) == inputs[i][1], "Invalid values cannot make the preview invisible");
    EXPECT(!camera_layer_needed(0, 0, 1, 1), "An opaque preview is recorded as captured");
    EXPECT(camera_layer_needed(0.55, 0, 1, 1), "An opaque saved video gets the camera copy over a translucent preview");
    EXPECT(!camera_layer_needed(0.55, 1, 1, 1), "Applying transparency to the video records the capture as is");
    EXPECT(!camera_layer_needed(0.55, 0, 0, 1) && !camera_layer_needed(0.55, 0, 1, 0),
           "A hidden or stopped camera adds nothing to the movie");

    /* Mapping: negative display origin, region offset, 2x pixels. */
    rect monitor = {-1920, -300, 1920, 1080};
    camera_layer l = camera_layer_map((rect){-1800, 0, 200, 200}, monitor, (rect){100, 200, 640, 360},
                                      1280, 720, SHAPE_CIRCLE);
    EXPECT(rect_equal(l.frame, (rect){40, 200, 400, 400}) && l.radius == 200,
           "Camera mapping handles negative display coordinates, region offsets and HiDPI scale");
    int x, y, w, h;
    camera_layer edge = camera_layer_map((rect){-60, -60, 120, 120}, (rect){0, 0, 320, 180},
                                         (rect){0, 0, 320, 180}, 320, 180, SHAPE_CIRCLE);
    EXPECT(camera_layer_clip(edge, 320, 180, &x, &y, &w, &h) && x == 0 && y == 0 && w == 61 && h == 61,
           "A camera crossing the recording boundary is clipped to the output");
    camera_layer away = camera_layer_map((rect){500, 500, 120, 120}, (rect){0, 0, 320, 180},
                                         (rect){0, 0, 320, 180}, 320, 180, SHAPE_CIRCLE);
    EXPECT(!camera_layer_clip(away, 320, 180, &x, &y, &w, &h), "A camera outside the area is not drawn");

    /* Render: left half red, right half blue camera. */
    camera_frame f = {320, 180, 320 * 4, g_malloc0(320 * 180 * 4)};
    for (int py = 0; py < 180; py++)
        for (int px = 0; px < 320; px++) f.data[py * f.stride + px * 4 + (px < 160 ? 2 : 0)] = 255;
    rect screen = {0, 0, 320, 180};
    for (int s = 0; s < SHAPE_COUNT; s++) {
        double height = 120 / shape_aspect(s);
        for (int mirrored = 0; mirrored < 2; mirrored++) {
            camera_layer cl = camera_layer_map((rect){100, 30, 120, height}, screen, screen, 320, 180, s);
            EXPECT(camera_layer_clip(cl, 320, 180, &x, &y, &w, &h), "The layer is inside the movie");
            cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *cr = cairo_create(img);
            camera_layer_draw(cr, cl, x, y, &f, mirrored, STAGE_IDENTITY, NULL);
            cairo_destroy(cr);
            cairo_surface_flush(img);
            guint8 *p = cairo_image_surface_get_data(img);
            int stride = cairo_image_surface_get_stride(img), mid = (int)(30 + height / 2) - y;
            guint8 *left = p + mid * stride + (140 - x) * 4, *right = p + mid * stride + (180 - x) * 4;
            EXPECT(left[mirrored ? 0 : 2] > 250 && left[1] < 5 && left[3] == 255,
                   "Camera pixels remain fully opaque with correct mirroring");
            EXPECT(right[mirrored ? 2 : 0] > 250 && right[1] < 5, "The camera is cropped and mirrored like the preview");
            EXPECT(p[(30 - y) * stride + (100 - x) * 4 + 3] == 0, "Clipped corners reveal the recorded desktop");
            cairo_surface_destroy(img);
        }
    }
    g_free(f.data);
}

/* savedTransparency and its migration from recordCameraTransparency. */
static void test_saved_transparency(void) {
    EXPECT(saved_transparency_parse("later", 1, 0) == SAVED_LATER, "The new key wins over the old one");
    EXPECT(saved_transparency_parse("opaque", 0, 0) == SAVED_OPAQUE, "Opaque is read back");
    EXPECT(saved_transparency_parse(NULL, 1, 1) == SAVED_PREVIEW, "A checked old option keeps the preview transparency");
    EXPECT(saved_transparency_parse(NULL, 1, 0) == SAVED_OPAQUE, "An unchecked old option stays opaque");
    EXPECT(saved_transparency_parse(NULL, 0, 0) == SAVED_OPAQUE, "A fresh install keeps today's opaque default");
    EXPECT(saved_transparency_parse("sometimes", 0, 0) == SAVED_OPAQUE, "Unknown values fall back to opaque");
}

/* Center Stage (PR antirez/youcam#1): soft clamp, zoom, smoothing, the
 * transform, no black bars at the extremes, face loss, and pico. */
static void test_center_stage(void) {
    EXPECT(stage_soft_clamp(10, 0.4) == 0, "No pan room means no pan");
    EXPECT(near(stage_soft_clamp(0, 50), 0) && fabs(stage_soft_clamp(1000, 50)) < 50 &&
           stage_soft_clamp(-1000, 50) > -50, "The soft clamp never passes the limit");
    EXPECT(near(stage_soft_clamp(10, 50), 50 * tanh(10 / 50.0 * 0.95)), "The soft clamp follows tanh");

    stage_face far = {0.5, 0.5, 0.2, 0.3}, close = {0.5, 0.5, 0.6, 0.9};
    EXPECT(near(stage_zoom(&far, SHAPE_RECTANGLE), 1.28) && near(stage_zoom(&far, SHAPE_CIRCLE), 1.10),
           "Base zoom: 1.28 in a rectangle, 1.10 otherwise");
    EXPECT(near(stage_zoom(&close, SHAPE_CIRCLE), 1.02), "Zoom backs off to 1.02 when close");

    stage_face s, f = {0.5, 0.5, 0.2, 0.3};
    EXPECT(stage_smooth(&s, 0, f) && near(s.cx, 0.5), "The first face is taken as is");
    EXPECT(!stage_smooth(&s, 1, (stage_face){0.505, 0.505, 0.21, 0.3}), "Small movements are ignored");
    EXPECT(stage_smooth(&s, 1, (stage_face){0.6, 0.5, 0.2, 0.3}) && near(s.cx, 0.5 + 0.1 * 0.22),
           "Larger movements are followed smoothly");

    stage_transform t = stage_transform_for(NULL, 280, 280, SHAPE_CIRCLE, 1);
    EXPECT(t.zoom == 1 && t.x == 0 && t.y == 0, "No face: identity");
    stage_face left = {0.3, 0.5, 0.2, 0.3};
    t = stage_transform_for(&left, 280, 280, SHAPE_SQUARE, 0);
    EXPECT(t.x > 0 && t.y > 0, "A face left of centre moves the picture right; head room moves it down");
    stage_transform m = stage_transform_for(&left, 280, 280, SHAPE_SQUARE, 1);
    EXPECT(near(m.x, -t.x) && near(m.y, t.y), "Mirroring flips the horizontal pan");

    double masks[SHAPE_COUNT][2] = {{280, 280}, {280, 280}, {400, 225}};
    stage_face extremes[] = {{0.01, 0.01, 0.25, 0.35}, {0.99, 0.99, 0.25, 0.35},
                             {0.01, 0.99, 0.25, 0.35}, {0.99, 0.01, 0.25, 0.35}};
    for (int shape = 0; shape < SHAPE_COUNT; shape++) {
        double w = masks[shape][0], h = masks[shape][1], cw, ch;
        stage_canvas(w, h, &cw, &ch);
        for (int mirrored = 0; mirrored < 2; mirrored++) {
            for (size_t i = 0; i < G_N_ELEMENTS(extremes); i++) {
                t = stage_transform_for(&extremes[i], w, h, shape, mirrored);
                double left_edge = w / 2 + t.x * w - cw * t.zoom / 2, top_edge = h / 2 + t.y * h - ch * t.zoom / 2;
                EXPECT(left_edge <= 0.5 && left_edge + cw * t.zoom >= w - 0.5,
                       "Horizontal pan at the extremes never exposes black bars");
                EXPECT(top_edge <= 0.5 && top_edge + ch * t.zoom >= h - 0.5,
                       "Vertical pan at the extremes never exposes black bars");
            }
        }
    }

    center_stage_update(&left);
    EXPECT(center_stage_has_face(), "A detected face is followed");
    for (int i = 0; i < 4; i++) center_stage_update(NULL);
    EXPECT(center_stage_has_face(), "A few missed frames keep the face");
    center_stage_update(NULL);
    EXPECT(!center_stage_has_face(), "Losing the face goes back to identity");
    g_usleep(300000);
    t = center_stage_now(280, 280, SHAPE_CIRCLE, 1);
    EXPECT(near(t.zoom, 1) && near(t.x, 0) && near(t.y, 0), "After the animation the picture is identity");
    center_stage_update(&left);
    center_stage_reset();
    EXPECT(!center_stage_has_face(), "Turning Center Stage off forgets the face");

    int w = 640, h = 360;
    guint8 *gray = g_malloc(w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) gray[y * w + x] = (guint8)((x / 40 + y / 40) % 2 ? 200 : 40);
    stage_face none;
    EXPECT(!stage_detect(gray, w, h, &none), "pico finds no face in a checkerboard");
    memset(gray, 128, w * h);
    EXPECT(!stage_detect(gray, w, h, &none), "pico finds no face in a flat image");
    g_free(gray);
}

/* Linux addition: where Mutter's RecordArea must look for a monitor. */
static GVariant *display_state(guint32 layout, guint32 transform, const char *mode_flags) {
    char *text = g_strdup_printf(
        "(uint32 1,"
        " [(('eDP-1', 'BOE', 'X', '0'), [('2560x1600', 2560, 1600, 60.0, 1.25, [1.0, 1.25], {%s}),"
        "                               ('1920x1200', 1920, 1200, 60.0, 1.0, [1.0], @a{sv} {})], @a{sv} {}),"
        "  (('HDMI-1', 'X', 'Y', '1'), [('1920x1080', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], @a{sv} {})],"
        " [(0, 0, 1.25, uint32 0, true, [('eDP-1', 'BOE', 'X', '0')], @a{sv} {}),"
        "  (2048, 0, 1.0, uint32 %u, false, [('HDMI-1', 'X', 'Y', '1')], @a{sv} {})],"
        " {'layout-mode': <uint32 %u>})", mode_flags, transform, layout);
    GVariant *v = g_variant_ref_sink(g_variant_new_parsed(text));
    g_free(text);
    return v;
}

static void test_mutter_monitor_rect(void) {
    int r[4];
    GVariant *s = display_state(1, 0, "'is-current': <true>");
    EXPECT(mutter_monitor_rect(s, "eDP-1", r) && r[0] == 0 && r[1] == 0 && r[2] == 2048 && r[3] == 1280,
           "A scaled monitor is recorded in logical pixels");
    EXPECT(mutter_monitor_rect(s, "HDMI-1", r) && r[0] == 2048 && r[2] == 1920 && r[3] == 1080,
           "A second monitor keeps its stage origin");
    EXPECT(!mutter_monitor_rect(s, "DP-9", r), "An unknown connector is not recorded");
    g_variant_unref(s);
    s = display_state(1, 1, "'is-current': <true>");
    EXPECT(mutter_monitor_rect(s, "HDMI-1", r) && r[2] == 1080 && r[3] == 1920, "A rotated monitor swaps its size");
    g_variant_unref(s);
    s = display_state(2, 0, "'is-current': <true>");
    EXPECT(mutter_monitor_rect(s, "eDP-1", r) && r[2] == 2560 && r[3] == 1600, "Physical layouts are not scaled");
    g_variant_unref(s);
    s = display_state(1, 0, "'is-preferred': <true>");
    EXPECT(!mutter_monitor_rect(s, "eDP-1", r), "A monitor without a current mode is not recorded");
    g_variant_unref(s);
}

/* Linux addition: the automatic microphone level. */
static void test_mic_agc(void) {
    const gint64 s = G_USEC_PER_SEC;
    mic_agc a = {0};
    EXPECT(mic_agc_step(&a, 1.0f, 10 * s) == MIC_AGC_CLIP_STEP_DB, "A clipped sample lowers the level at once");
    EXPECT(mic_agc_step(&a, 1.0f, 10 * s + s / 10) == 0, "It then waits for the change to take effect");
    a = (mic_agc){0};
    mic_agc_step(&a, 0.95f, 20 * s);
    EXPECT(mic_agc_step(&a, 0.5f, 22 * s + 1) == -MIC_AGC_STEP_DB, "Peaks close to clipping lower the level");
    a = (mic_agc){0};
    mic_agc_step(&a, 0.05f, 30 * s);
    EXPECT(mic_agc_step(&a, 0.05f, 32 * s + 1) == MIC_AGC_STEP_DB, "Quiet speech raises the level");
    a = (mic_agc){0};
    mic_agc_step(&a, 0.001f, 40 * s);
    EXPECT(mic_agc_step(&a, 0.001f, 42 * s + 1) == 0, "Silence does not raise the level");
    a = (mic_agc){0};
    mic_agc_step(&a, 0.3f, 50 * s);
    EXPECT(mic_agc_step(&a, 0.3f, 52 * s + 1) == 0, "A good level is left alone");
    EXPECT(near(mic_volume_after(0.22, -18.06), 0.11), "Volumes are cubic: -18 dB halves them");
    EXPECT(mic_volume_after(0.9, 12) == MIC_VOLUME_MAX && mic_volume_after(0.06, -40) == MIC_VOLUME_MIN,
           "The microphone volume stays within limits");
    setlocale(LC_NUMERIC, "it_IT.UTF-8");   /* A comma locale must not break the parse. */
    double parsed = mic_volume_parse("Volume: 0.22 [MUTED]");
    setlocale(LC_NUMERIC, "C");
    EXPECT(parsed == 0.22 && mic_volume_parse("Error") < 0,
           "wpctl volumes are parsed, errors rejected");
}

/* Background removal: mode names, colors and the pure image helpers. */
static void test_background(void) {
    EXPECT(background_mode_parse("blur") == BG_BLUR && background_mode_parse("transparent") == BG_TRANSPARENT,
           "Background modes are read by name");
    EXPECT(background_mode_parse("banana") == BG_OFF && background_mode_parse(NULL) == BG_OFF,
           "An unknown background mode is Off");
    double rgb[3];
    EXPECT(background_color_parse("#ff8000", rgb) && rgb[0] == 1 && near(rgb[1], 128 / 255.0) && rgb[2] == 0,
           "Colors are #rrggbb");
    EXPECT(!background_color_parse("red", rgb) && !background_color_parse("#12345", rgb) &&
           !background_color_parse(NULL, rgb), "Invalid colors are rejected");

    guint8 bgrx[4 * 2 * 4];   /* 4x2: left half red, right half blue. */
    for (int i = 0; i < 8; i++) {
        guint8 *p = bgrx + i * 4;
        int left = i % 4 < 2;
        p[0] = left ? 0 : 255; p[1] = 0; p[2] = left ? 255 : 0; p[3] = 255;
    }
    guint8 rgb2[6];
    frame_to_model(bgrx, 4, 2, 16, rgb2, 2, 1);
    EXPECT(rgb2[0] == 255 && rgb2[2] == 0 && rgb2[3] == 0 && rgb2[5] == 255,
           "Frames shrink to packed RGB by averaging");

    float raw[2] = {1, 0};
    guint8 mask[2];
    mask_smooth(mask, raw, 2, 0);
    EXPECT(mask[0] == 255 && mask[1] == 0, "The first mask is taken as is");
    float flipped[2] = {0, 1};
    mask_smooth(mask, flipped, 2, 1);
    EXPECT(mask[0] > 0 && mask[0] < 255 && mask[1] > 0 && mask[1] < 255, "Later masks are smoothed over time");

    guint8 white[4 * 4];
    memset(white, 255, sizeof(white));
    camera_frame f = {4, 1, 16, white}, person = {0, 0, 0, NULL};
    guint8 half[2] = {255, 0};
    mask_compose(&f, half, 2, 1, &person);
    EXPECT(person.width == 4 && person.data[3] == 255 && person.data[15] == 0, "The mask becomes alpha");
    EXPECT(person.data[7] == 255 && person.data[11] == 0 && person.data[4] == 255,
           "Interpolated alpha goes through the edge curve, colors premultiplied");
    EXPECT(mask_edge(0) == 0 && mask_edge(80) == 0 && mask_edge(200) == 255 && mask_edge(255) == 255 &&
           mask_edge(140) > 80 && mask_edge(140) < 200 && mask_edge(130) < mask_edge(150),
           "The edge curve clears a faint halo, keeps the person and stays smooth in between");

    /* Pixels: 0 sure room, 1 cabinet the model calls person, 2 the person. The camera is 20% brighter than when learned. */
    guint8 room[9] = {100, 100, 100, 200, 200, 200, 50, 50, 50};
    guint8 now[9] = {120, 120, 120, 240, 240, 240, 200, 150, 120};
    float alpha[3] = {0, 0.99f, 0.99f};
    mask_remove_room(alpha, now, room, 3);
    EXPECT(alpha[0] == 0 && alpha[1] == 0 && near(alpha[2], 0.99f),
           "Furniture matching the empty room is cleared, the person kept, exposure changes allowed for");
    g_free(person.data);

    guint8 gray[64 * 32 * 4];
    memset(gray, 128, sizeof(gray));
    camera_frame g = {64, 32, 256, gray}, blurred = {0, 0, 0, NULL};
    blur_small(&g, &blurred);
    EXPECT(blurred.width == 4 && blurred.height == 2 && blurred.data[0] == 128 && blurred.data[5] == 128,
           "A uniform frame stays uniform when blurred");
    g_free(blurred.data);

    GdkPixbuf *photo = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 4000, 3000);
    GdkPixbuf *fitted = background_image_fit(photo);
    EXPECT(gdk_pixbuf_get_width(fitted) == 1280 && gdk_pixbuf_get_height(fitted) == 960,
           "A large background image is reduced once, keeping its aspect");
    g_object_unref(fitted);
    g_object_unref(photo);
    GdkPixbuf *tiny = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8, 6);
    fitted = background_image_fit(tiny);
    EXPECT(gdk_pixbuf_get_width(fitted) == 8 && gdk_pixbuf_get_height(fitted) == 6,
           "A small background image keeps its size");
    g_object_unref(fitted);
    g_object_unref(tiny);
    GdkPixbuf *portrait = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 3000, 2000);
    gdk_pixbuf_set_option(portrait, "orientation", "6");   /* A phone photo taken upright. */
    fitted = background_image_fit(portrait);
    EXPECT(gdk_pixbuf_get_width(fitted) == 853 && gdk_pixbuf_get_height(fitted) == 1280,
           "A background image is turned upright from its EXIF orientation");
    g_object_unref(fitted);
    g_object_unref(portrait);
}

/* Renders a 64x36 frame (16:9, so the canvas is the frame) whose left half is
 * the person, and returns the ARGB32 pixel at x of row 18. */
static guint32 render_pixel(background_mode mode, int x) {
    static guint8 white[64 * 36 * 4];
    memset(white, 255, sizeof(white));
    camera_frame f = {64, 36, 256, white};
    guint8 mask[2] = {255, 0};
    background_layer bg = {mode, {0, 0, 0, NULL}, {0, 0, 0, NULL}, NULL, {1, 0, 0}};
    mask_compose(&f, mask, 2, 1, &bg.person);
    guint8 blue[4 * 2 * 4];
    for (int i = 0; i < 8; i++) { blue[i * 4] = 255; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 0; blue[i * 4 + 3] = 255; }
    bg.blurred = (camera_frame){4, 2, 16, g_memdup2(blue, sizeof(blue))};
    bg.image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 8, 8);
    cairo_t *green = cairo_create(bg.image);
    cairo_set_source_rgb(green, 0, 1, 0);
    cairo_paint(green);
    cairo_destroy(green);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 36);
    cairo_t *cr = cairo_create(s);
    camera_paint_frame(cr, &f, 64, 36, 0, STAGE_IDENTITY, &bg);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    guint32 px = *(guint32 *)(cairo_image_surface_get_data(s) + 18 * cairo_image_surface_get_stride(s) + x * 4);
    cairo_surface_destroy(s);
    g_free(bg.person.data);
    g_free(bg.blurred.data);
    cairo_surface_destroy(bg.image);
    return px;
}

/* A small blurred frame whose aspect is not the camera's (64/16 x 36/16 =
 * 4 x 2.25, stored as 4 x 1) must still cover exactly the frame: its last
 * pixel, blue, belongs at x = 56, where the camera's last quarter is. */
static guint32 render_blur_column(int x) {
    static guint8 white[64 * 36 * 4];
    memset(white, 255, sizeof(white));
    camera_frame f = {64, 36, 256, white};
    guint8 mask[2] = {255, 0};
    background_layer bg = {BG_BLUR, {0, 0, 0, NULL}, {0, 0, 0, NULL}, NULL, {0, 0, 0}};
    mask_compose(&f, mask, 2, 1, &bg.person);
    guint8 small[4 * 4] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255};   /* BGRx: red, red, red, blue. */
    bg.blurred = (camera_frame){4, 1, 16, small};
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 36);
    cairo_t *cr = cairo_create(s);
    camera_paint_frame(cr, &f, 64, 36, 0, STAGE_IDENTITY, &bg);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    guint32 px = *(guint32 *)(cairo_image_surface_get_data(s) + 18 * cairo_image_surface_get_stride(s) + x * 4);
    cairo_surface_destroy(s);
    g_free(bg.person.data);
    return px;
}

static void test_background_paint(void) {
    guint32 edge = render_blur_column(56);
    EXPECT((edge & 0xff) > ((edge >> 16) & 0xff), "The blurred frame lines up with the camera frame");
    EXPECT(render_pixel(BG_COLOR, 2) == 0xffffffff, "The person stays in front");
    EXPECT(render_pixel(BG_COLOR, 61) == 0xffff0000, "Color fills the background");
    EXPECT(render_pixel(BG_IMAGE, 61) == 0xff00ff00, "The image fills the background");
    EXPECT(render_pixel(BG_BLUR, 61) == 0xff0000ff, "The blurred frame fills the background");
    EXPECT(render_pixel(BG_TRANSPARENT, 61) >> 24 == 0, "Transparent leaves the background empty");
}

/* Choose After Recording (export.c): layer opacity, slider limit, the
 * project settings in the MKV header, and file names. */
static void test_later(void) {
    EXPECT(near(later_alpha(0, 1), 1) && near(later_alpha(0.4, 1), 0.6) && near(later_alpha(0.8, 1), 0.2),
           "With the extension the camera layer opacity is 1 - t");
    EXPECT(near(later_alpha(0, 0.3), 1) && near(later_alpha(0.15, 0.3), 0.5) && near(later_alpha(0.3, 0.3), 0),
           "Without it the layer fades out at the preview transparency");
    EXPECT(later_alpha(0, 0) == 1 && later_alpha(0.5, 0) == 1, "An opaque preview without the extension keeps the camera opaque");
    EXPECT(later_alpha(0.9, 0.3) == 0 && later_alpha(-1, 0.3) == 1 && later_alpha(NAN, 0.3) == 1,
           "Choices outside the range are clamped");
    double t_a = 0.6, t = 0.25, a = later_alpha(t, t_a);
    EXPECT(near(1 - (a + (1 - a) * (1 - t_a)), t), "The exported camera has exactly the chosen transparency");
    EXPECT(near(later_max(1), TRANSPARENCY_MAX) && near(later_max(0.3), 0.3) && later_max(0) == 0 && later_max(NAN) == 0,
           "The slider reaches 80% with the extension, the preview transparency without");

    project_info in = {1, 0.3, "mp4", 1}, out = {0};
    char text[PROJECT_INFO_MAX];
    EXPECT(project_info_format(&in, text, sizeof(text)), "Project settings fit their header field");
    guint8 header[512];
    for (size_t i = 0; i < sizeof(header); i++) header[i] = (guint8)(i * 37);   /* Binary MKV bytes around it. */
    memcpy(header + 288, text, strlen(text));
    EXPECT(project_info_parse(header, sizeof(header), &out) && out.t_a == 1 && near(out.preview, 0.3) &&
           strcmp(out.container, "mp4") == 0 && out.has_audio, "Project settings are found inside the MKV header");
    EXPECT(!project_info_parse(header, 288, &out), "A file without them is not a project");
    EXPECT(!project_info_parse(header, 288 + 20, &out), "Truncated settings are rejected");
    const char *bad = "YouCam project;YOUCAM_T_A=banana;YOUCAM_PREVIEW_TRANSPARENCY=0.3;YOUCAM_CONTAINER=mov;YOUCAM_AUDIO=1;";
    EXPECT(!project_info_parse((const guint8 *)bad, strlen(bad), &out), "Corrupt settings are rejected");
    const char *range = "YouCam project;YOUCAM_T_A=2;YOUCAM_PREVIEW_TRANSPARENCY=0.3;YOUCAM_CONTAINER=mov;YOUCAM_AUDIO=1;";
    EXPECT(!project_info_parse((const guint8 *)range, strlen(range), &out), "Out-of-range settings are rejected");
    const char *odd = "YouCam project;YOUCAM_T_A=1;YOUCAM_PREVIEW_TRANSPARENCY=0;YOUCAM_CONTAINER=webm;YOUCAM_AUDIO=0;";
    EXPECT(project_info_parse((const guint8 *)odd, strlen(odd), &out) && strcmp(out.container, "mov") == 0 &&
           !out.has_audio, "An unknown container falls back to MOV");

    char *dir = g_dir_make_tmp("youcam-later-XXXXXX", NULL);
    const char *project = "/x/pending/YouCam-Recording-01Nov-10.00.00" PROJECT_SUFFIX;
    char *first = export_output_path(dir, project, "mov");
    char *expected = g_build_filename(dir, "YouCam-Recording-01Nov-10.00.00.mov", NULL);
    EXPECT(strcmp(first, expected) == 0, "The movie takes the project's name and the chosen format");
    g_file_set_contents(first, "", 0, NULL);
    char *second = export_output_path(dir, project, "mov");
    EXPECT(g_str_has_suffix(second, "/YouCam-Recording-01Nov-10.00.00 (2).mov"), "An existing movie is never overwritten");
    g_file_set_contents(second, "", 0, NULL);
    char *third = export_output_path(dir, project, "mov");
    EXPECT(g_str_has_suffix(third, " (3).mov"), "The number keeps counting");
    char *fresh = project_new_path(dir, 0), *base = recording_base_name(0);
    EXPECT(g_str_has_suffix(fresh, PROJECT_SUFFIX) && strstr(fresh, base), "A project is named like the movie it becomes");
    char *old = g_build_filename(dir, "a" PROJECT_SUFFIX, NULL), *recent = g_build_filename(dir, "b" PROJECT_SUFFIX, NULL);
    char *part = g_build_filename(dir, "c.mov.part", NULL);
    g_file_set_contents(old, "", 0, NULL);
    g_file_set_contents(recent, "", 0, NULL);
    g_file_set_contents(part, "", 0, NULL);
    struct utimbuf t_old = {1000, 1000}, t_recent = {2000, 2000};
    g_utime(old, &t_old);
    g_utime(recent, &t_recent);
    char *latest = project_latest(dir, NULL), *earlier = project_latest(dir, recent);
    EXPECT(latest && strcmp(latest, recent) == 0, "The most recent project is the one opened");
    EXPECT(earlier && strcmp(earlier, old) == 0, "The project being recorded is skipped, older ones stay reachable");
    project_clean_parts(dir);
    EXPECT(!g_file_test(part, G_FILE_TEST_EXISTS) && g_file_test(old, G_FILE_TEST_EXISTS),
           "Stale .part files go, projects stay");
    EXPECT(!project_latest("/nonexistent-youcam-pending", NULL), "No pending folder, no project");
    g_unlink(first); g_unlink(second); g_unlink(old); g_unlink(recent);
    g_rmdir(dir);
    g_free(first); g_free(expected); g_free(second); g_free(third); g_free(fresh); g_free(base);
    g_free(old); g_free(recent); g_free(part); g_free(latest); g_free(earlier); g_free(dir);
}

int main(void) {
    test_overlay_geometry();
    test_area_corners();
    test_audio_gain();
    test_files_and_labels();
    test_crop();
    test_panel_buttons();
    test_prefs();
    test_transparency();
    test_saved_transparency();
    test_later();
    test_center_stage();
    test_background();
    test_background_paint();
    test_mutter_monitor_rect();
    test_mic_agc();
    printf("Passed %d logic checks.\n", Checks);
    return 0;
}
