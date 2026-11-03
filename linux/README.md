# YouCam for Linux

A C port of the macOS app: a floating camera without a window frame, with
selectable cameras, shapes and microphone input, plus a screen recorder.
Built on GTK 3 and GStreamer, which GNOME desktops already ship. See
[CHANGELOG.md](CHANGELOG.md) for what the port adds and fixes.

## Build and run

Requires GTK 3, GStreamer 1.20+ with the good, ugly (x264) and libav (AAC)
plugins, the PipeWire GStreamer plugin and `xdg-desktop-portal` (GNOME,
KDE or another desktop with a ScreenCast portal). Optional: the bad plugins
(`webrtcdsp` for Noise Reduction, `vah264enc`/`nvh264enc` for GPU encoding)
and WirePlumber's `wpctl` for Automatic Microphone Level; without them those
features are skipped.

Building also needs a C compiler, `make`, `pkg-config`, `xxd` and the
development files of GTK 3 and GStreamer (with gst-plugins-base). For
example:

```sh
sudo apt install build-essential pkgconf xxd libgtk-3-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev     # Debian, Ubuntu
sudo dnf install gcc make pkgconf-pkg-config /usr/bin/xxd gtk3-devel \
    gstreamer1-devel gstreamer1-plugins-base-devel           # Fedora
sudo pacman -S --needed base-devel xxd gtk3 gst-plugins-base # Arch
```

```sh
make
make check        # logic and segmentation tests; make test adds the media pipeline
make install      # ~/.local: binary, man page, icons, desktop entry, Shell extension
youcam
```

The desktop entry gives the portal an app id, which the global shortcuts
need. `make uninstall` removes everything `make install` added.
`make install PREFIX=/usr DESTDIR=<dir>` installs into a staging directory,
as packages do. `man youcam` lists options, environment and files.

## Packages

The Makefile builds a native package for the distribution it runs on
(`make rpm` and `make arch` package the committed sources through `make
dist`, so commit changes first):

```sh
make deb          # Debian, Ubuntu: ../youcam_<version>_<arch>.deb (dpkg-buildpackage)
make rpm          # Fedora: rpmbuild/RPMS/<arch>/youcam-<version>-1.<dist>.<arch>.rpm
make arch         # Arch: youcam-<version>-1-<arch>.pkg.tar.zst (makepkg)
make dist         # youcam-<version>.tar.gz, the source archive the rpm and arch targets use
```

Each needs that distribution's tools (`dpkg-dev` and `debhelper`,
`rpm-build`, or `base-devel`) and the build dependencies above;
`apt build-dep ./` and `dnf builddep packaging/youcam.spec` install them.
The recipes are in `debian/` and `packaging/`; packages run `make check`
while building. Install the result with the system package manager, for
example `sudo apt install ../youcam_1.0~beta1_amd64.deb`. On Fedora the
H.264 and AAC encoders (gstreamer1-plugins-ugly, gstreamer1-plugin-libav)
come from RPM Fusion.

## Use

- **Right-click the camera** (or Control-click) for controls. Drag to move
  it; scroll to resize. Choose a circle, square, or rectangle.
- **Recording Area → Select Area…** selects a region. Drag its corner dots to
  adjust it before recording, or choose **Whole Screen**. With several
  displays, **Whole Screen — <display>** records a specific one. Your area
  choice is remembered.
- **Start Recording**, **Pause / Resume**, and **Stop Recording** control
  capture. The first time, the system asks which screen to share; the choice
  is remembered. Movies save automatically to
  `~/Desktop/YouCam-Recording-<date>-<time>.mov` (or the chosen
  Recording Format).
- **Audio Input** selects a microphone and provides a gain slider and live
  level meter. System/app audio is not recorded. **Noise Reduction** (Linux
  only: Off, Low, Moderate, High, Very High; default Moderate) removes hum and
  room noise from the recorded microphone with WebRTC noise suppression.
  **Automatic Microphone Level** (Linux only, on by default) adjusts the
  system level of the microphone (via `wpctl`) only while YouCam uses it:
  6 dB lower at once on clipping, lower for peaks above -6 dBFS, higher for
  quiet speech. The original level is put back when YouCam stops using the
  microphone, quits, or restarts after a crash. Other apps see the changed
  level meanwhile.
- **Recording Format** (Linux only) picks the container: MOV (default, like
  macOS), MP4, MKV or AVI, all H.264 + AAC. MKV stays playable even if
  YouCam is killed mid-recording.
- Video is encoded on the GPU when possible (Intel VA-API, then NVIDIA NVENC,
  then x264 in software). `YOUCAM_ENCODER=va|nvenc|x264` forces one.
- **Transparency** optionally makes the camera see-through so you can see
  what it covers. Adjust the slider from 0% to 80%, or choose **Reset
  Transparency to 0%** to turn it off. **Saved Video** chooses what
  recordings show: **Same as Preview**, **Opaque** (default), or (Linux
  only) **Choose After Recording**: when you stop, a window shows a frame of
  the recording and a slider for the camera transparency of the saved
  movie; **Save** writes it to the Desktop in the Recording Format,
  **Later** keeps it for **Adjust Last Recording Transparency…**,
  **Discard…** deletes it. With the YouCam Capture Exclusion extension any
  value up to 80% is possible (the camera stays on screen but out of the
  screen recording); without it, up to the preview transparency used while
  recording, which then stays fixed until you stop. Unfinished recordings
  wait in `~/.local/share/youcam/pending/` (playable MKV files, also after
  a crash). It needs the FFV1 codec from gst-libav. Both choices are
  remembered, and slider changes apply immediately, also during recording,
  except in a Choose After Recording recording without the extension, where
  the transparency stays fixed as described above.
- **Center Stage (Follow Face)** gently pans and zooms the camera to keep
  your face centred, in the preview and in recordings. It is on by default
  and remembered.
- **Background** (Linux only) separates you from the room: **Transparent**
  shows only you (the shape no longer applies), **Blur** softens the
  background, **Image** puts a picture of your choice behind you, **Color** a
  solid color. **Choose Image…** and **Choose Color…** pick them. If
  furniture next to you stays visible, **Learn Empty Room…** shows YouCam
  the room without you (step out during the countdown); it is kept per
  camera until **Forget Empty Room**. It runs on
  the CPU with no extra software, in the preview and in recordings, and is
  off by default. `make bench-segment` shows how fast your CPU runs it.
- **Show Recording Panel** opens optional controls. **Hide Camera** lets you
  record just the screen; launch YouCam again to bring the camera back.

Global shortcuts: **Control–Alt–Super–R** starts/stops; **Control–Alt–Super–P**
pauses/resumes (GNOME asks to confirm them once).

Run checks with `make test` and `make smoke-test`. `./youcam --record-test 6` makes a real 6-second
recording through the portal or, with the capture exclusion extension active, through Mutter
(after one portal recording has saved the monitor choice, no dialog appears) and
quits; inspect the movie on the Desktop with `ffprobe`.
`YOUCAM_SNAPSHOT_PROJECT=<project>.youcam.mkv ./youcam --snapshot DIR` also renders the
Choose After Recording window to `DIR/transparency.png`.

Face detection uses [pico](https://github.com/nenadmarkus/pico) by Nenad
Markus (MIT license), vendored in `vendor/pico/` with its license.

Background removal uses MediaPipe Selfie Segmentation by Google (Apache
License 2.0), converted by `tools/convert_selfie.py` and vendored in
`vendor/selfie/` with its license and provenance. Test portraits in
`testdata/selfie/` are public domain (NASA).

## Differences from macOS

- Windows run through XWayland, the only way on GNOME Wayland to keep the
  camera above other windows and remember where it was.
- The menu bar icon is a StatusNotifierItem: GNOME shows it only with the
  AppIndicator extension.
- macOS excludes YouCam's panel and guides from recordings. On GNOME this
  needs the small **YouCam Capture Exclusion** Shell extension, which `make
  install` copies: log out and in once, then run `gnome-extensions enable
  youcam-capture-exclusion@youcam.app`. While it is active YouCam records
  through Mutter's screen cast instead of the portal (no screen-sharing
  dialog) and the panel and guides stay on screen but out of the movie.
  Without it, a whole-screen recording shows the panel if it is open.
  `YOUCAM_CAPTURE=portal` or `=mutter` forces either path.
- Center Stage finds faces with pico instead of Apple's Vision framework, and
  never uses a camera's own Center Stage (macOS's Continuity Camera path).
- Menu keyboard equivalents (Command-H, Command-1…) are not available: the
  camera never takes keyboard focus. The global shortcuts work everywhere.
