# YouCam

A native Mac camera overlay and screen recorder. A floating camera without a window frame, with selectable cameras, shapes, and microphone input.

## Build and run

Requires macOS 14+ and Xcode Command Line Tools with Swift 6. No external dependencies.

```sh
make
open build/YouCam.app
```

You can copy `build/YouCam.app` into Applications. Grant camera, microphone, and screen recording permissions when prompted.

## Use

- **Right-click the camera** or use the menu bar icon for controls. Drag to move it; scroll to resize. Choose a circle, square, or rectangle.
- **Transparency** optionally makes the camera see-through so you can see what it covers. Adjust the slider from 0% to 80%, or choose **Reset Transparency to 0%** to turn it off. Check **Apply Transparency to Saved Video** to record the same transparency; leave it unchecked to keep the recorded camera opaque. Both choices are remembered, and changes apply immediately, including during recording.
- **Recording Area → Select Area…** selects a region. Drag its corner dots to adjust it before recording, or choose **Whole Screen**. Your area choice is remembered.
- **Start Recording**, **Pause / Resume**, and **Stop Recording** control capture. Movies save automatically to `~/Desktop/YouCam-Recording-<date>-<time>.mov`.
- **Audio Input** selects a microphone and provides a gain slider and live level meter. System/app audio is not recorded.
- **Show Recording Panel** opens optional controls. **Hide Camera** lets you record just the screen. The panel and area guides stay out of YouCam's recordings.

Global shortcuts: **Control–Option–Command–R** starts/stops; **Control–Option–Command–P** pauses/resumes.

Run checks with `make test` and `make smoke-test`.

[MIT license](LICENSE).
