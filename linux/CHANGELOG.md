# Changelog — YouCam for Linux

## Unreleased

First Linux version: a C port (GTK 3 + GStreamer) of the macOS app, kept in
`linux/` next to the untouched Swift sources.

### Ported from macOS

- Floating camera without a window frame: circle, square or rectangle;
  drag to move, scroll to resize, position and size remembered.
- Camera, microphone and shape selection from the right-click menu and the
  tray icon (StatusNotifierItem).
- Screen recorder: whole screen, a chosen display, or a selected area with
  draggable corner dots; Start, Pause / Resume, Stop; recording panel.
- Microphone gain slider and live level meter with clipping warning.
- Camera transparency (upstream PR #2): 0–80% slider; the saved video is
  Same as Preview or Opaque (the default). The old "Apply Transparency
  to Saved Video" setting is migrated.
- Center Stage face tracking (upstream PR #1): pans and zooms to keep the
  face centred, in the preview and in recordings, using vendored pico.
- Global shortcuts Control–Alt–Super–R (start/stop) and
  Control–Alt–Super–P (pause/resume) through the GlobalShortcuts portal.
- Movies saved to the Desktop as `YouCam-Recording-<date>-<time>`.

### Linux additions

- **Recording Format**: MOV (default), MP4, MKV or AVI, all H.264 + AAC.
- **GPU encoding**: Intel VA-API, then NVIDIA NVENC, then x264;
  `YOUCAM_ENCODER=va|nvenc|x264` forces one.
- **Noise Reduction**: Off, Low, Moderate (default), High, Very High, with
  WebRTC noise suppression on the recorded microphone.
- **Automatic Microphone Level** (on by default): adjusts the system level
  of the microphone while YouCam uses it (6 dB lower at once on clipping,
  lower for peaks above −6 dBFS, higher for quiet speech) and puts the
  original level back on stop, quit, or the next start after a crash.
- **Capture exclusion on GNOME**: the YouCam Capture Exclusion Shell
  extension keeps the panel and area guides on screen but out of the
  recording, as macOS does. With it active YouCam records through Mutter's
  screen cast; without it, through the ScreenCast portal.
  `YOUCAM_CAPTURE=portal|mutter` forces either path.
- **Background** (Off, Transparent, Blur, Image, Color): CPU segmentation
  with MediaPipe Selfie Segmentation converted to plain C, no new runtime
  dependency; applies to the preview and to recordings.
  **Learn Empty Room…** removes furniture the model mistakes for you by
  comparing with a picture of the room without you, kept per camera.
- **Choose After Recording** (Transparency → Saved Video): the camera layer
  is recorded on its own lossless track (FFV1) in a project MKV, and when
  the recording stops a window previews the camera transparency and saves
  the movie in the chosen format. With the Capture Exclusion extension any
  value up to 80%, without it up to the preview transparency. **Adjust Last
  Recording Transparency…** reopens a recording kept for later.
- `make install` / `make uninstall` (to `~/.local`, or `PREFIX`/`DESTDIR`),
  `make check`, `make test`, `make smoke-test`, and `--record-test N` for a
  real N-second recording.
- Packages: `make deb`, `make rpm` and `make arch` build native Debian/Ubuntu,
  Fedora and Arch packages (recipes in `debian/` and `packaging/`), checked
  with lintian and rpmlint; a `youcam(1)` man page.

### Fixes since the first build

- Recordings came out 16x16 with no visible picture: the movie size is now
  pinned to the recorded screen area.
- Menu sliders (transparency, gain) could not be dragged.
- Black frames at the start of a recording.
- The panel showed in the first frame of a recording with capture exclusion.
- Microphone clipping with a loud voice (automatic level).

### Known differences from macOS

- Windows run through XWayland on GNOME Wayland (to stay on top and keep
  their position).
- The tray icon needs the AppIndicator extension on GNOME.
- Capture exclusion needs the Shell extension, enabled once after a
  re-login.
- No menu keyboard equivalents (Command-H, Command-1…): the camera never
  takes keyboard focus.
