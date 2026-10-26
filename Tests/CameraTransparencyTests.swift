import AppKit
import CoreImage

@MainActor
func runCameraTransparencyTests(_ delegate: AppDelegate, defaults: UserDefaults) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        guard condition() else { fatalError("FAIL: \(message)") }
    }
    expect(delegate.panel.alphaValue == 1 && delegate.cameraTransparency == 0,
           "Preview transparency is optional and off by default")
    let menu = delegate.makeMenu().item(withTitle: "Transparency")!.submenu!
    let control = menu.items.compactMap { $0.view as? CameraTransparencyControl }.first!
    control.slider.doubleValue = 55
    expect(NSApp.sendAction(control.slider.action!, to: control.slider.target, from: control.slider),
           "The transparency slider is connected")
    expect(abs(delegate.panel.alphaValue - 0.45) < 0.001,
           "The slider changes preview window opacity immediately")
    expect(defaults.double(forKey: CameraTransparency.preferenceKey) == 0.55,
           "The selected preview transparency is saved")
    let save = menu.item(withTitle: "Apply Transparency to Saved Video")!.view as! PersistentMenuToggle
    expect(save.state == .off && !delegate.saveCameraTransparency && delegate.recordingCameraOpacity == 1,
           "Saved video stays opaque by default even with a translucent preview")
    save.performClick(nil)
    expect(save.state == .on && delegate.saveCameraTransparency
           && defaults.bool(forKey: CameraTransparency.recordingPreferenceKey),
           "Checking the option updates its checkmark immediately and saves the choice")
    expect(abs(delegate.recordingCameraOpacity - 0.45) < 0.001 && abs(delegate.panel.alphaValue - 0.45) < 0.001,
           "Checked recording opacity matches the preview")
    control.slider.doubleValue = 35
    _ = NSApp.sendAction(control.slider.action!, to: control.slider.target, from: control.slider)
    expect(abs(delegate.recordingCameraOpacity - 0.65) < 0.001,
           "The selected recording mode follows later slider changes")
    save.performClick(nil)
    expect(save.state == .off && !delegate.saveCameraTransparency
           && !defaults.bool(forKey: CameraTransparency.recordingPreferenceKey)
           && delegate.recordingCameraOpacity == 1 && abs(delegate.panel.alphaValue - 0.65) < 0.001,
           "Unchecking restores opaque video without changing preview transparency")
    control.slider.doubleValue = 55
    _ = NSApp.sendAction(control.slider.action!, to: control.slider.target, from: control.slider)
    expect(delegate.preview.hitTest(NSPoint(x: delegate.preview.bounds.midX, y: delegate.preview.bounds.midY)) === delegate.preview,
           "A translucent camera can still be dragged and right-clicked")
    delegate.toggleVisibility()
    delegate.toggleVisibility()
    expect(abs(delegate.panel.alphaValue - 0.45) < 0.001,
           "Hiding and showing the camera preserves transparency")
    let opaque = menu.item(withTitle: "Reset Transparency to 0%")!
    expect(NSApp.sendAction(opaque.action!, to: opaque.target, from: opaque), "The opaque-preview command is connected")
    expect(delegate.panel.alphaValue == 1 && defaults.double(forKey: CameraTransparency.preferenceKey) == 0,
           "Reset Transparency to 0% disables transparency and saves the choice")
    expect(opaque.state == .off, "Reset is a one-time command without a persistent checkmark")
    control.slider.doubleValue = 55
    _ = NSApp.sendAction(control.slider.action!, to: control.slider.target, from: control.slider)
    expect(opaque.state == .off && abs(delegate.panel.alphaValue - 0.45) < 0.001,
           "Adjusting the slider after Reset cannot leave a stale checkmark in the open menu")
    for (input, expected) in [(-1.0, 0.0), (2.0, 0.8), (Double.nan, 0), (Double.infinity, 0)] {
        delegate.setCameraTransparency(input)
        expect(delegate.cameraTransparency == expected && abs(delegate.panel.alphaValue - (1 - expected)) < 0.001,
               "Invalid values cannot make the preview invisible")
    }
    delegate.setCameraTransparency(0)
    checks += runCameraCompositorTests()
    return checks
}

func runCameraCompositorTests() -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        guard condition() else { fatalError("FAIL: \(message)") }
    }
    let target = RecordingTarget(displayID: 1, rect: CGRect(x: 0, y: 0, width: 320, height: 180), scale: 1, name: "Test")
    let screenFrame = target.rect
    let desktop = CIImage(color: CIColor(red: 0, green: 1, blue: 0)).cropped(to: screenFrame)
    let red = CIImage(color: CIColor(red: 1, green: 0, blue: 0))
    let blue = CIImage(color: CIColor(red: 0, green: 0, blue: 1))
    let camera = red.cropped(to: CGRect(x: 0, y: 0, width: 160, height: 180))
        .composited(over: blue.cropped(to: screenFrame))
    let context = CIContext()
    func pixel(_ image: CIImage, x: Int, y: Int) -> [UInt8] {
        var rgba = [UInt8](repeating: 0, count: 4)
        context.render(image, toBitmap: &rgba, rowBytes: 4,
                       bounds: CGRect(x: x, y: y, width: 1, height: 1), format: .RGBA8,
                       colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
        return rgba
    }
    for shape in CameraShape.allCases {
        let height: CGFloat = 120 / shape.aspectRatio
        for mirrored in [false, true] {
            let overlay = RecordingCameraOverlay(windowFrame: CGRect(x: 100, y: 30, width: 120, height: height),
                screenFrame: screenFrame, target: target, shape: shape, mirrored: mirrored)
            let result = RecordingCameraCompositor.composite(screen: desktop, camera: camera, overlay: overlay)
            let left = pixel(result, x: 140, y: Int(30 + height / 2))
            let right = pixel(result, x: 180, y: Int(30 + height / 2))
            expect(left[mirrored ? 2 : 0] > 250 && left[1] < 5 && left[3] == 255,
                   "\(shape) camera pixels remain fully opaque with correct mirroring")
            expect(right[mirrored ? 0 : 2] > 250 && right[1] < 5,
                   "\(shape) crops and mirrors the original camera independently of preview opacity")
            expect(pixel(result, x: 100, y: 30)[1] > 250,
                   "\(shape) clipped corners reveal the recorded desktop")
            expect(pixel(result, x: 20, y: 20)[1] > 250,
                   "Compositing preserves the screen outside the camera")
        }
        let overlay = RecordingCameraOverlay(windowFrame: CGRect(x: 100, y: 30, width: 120, height: height),
            screenFrame: screenFrame, target: target, shape: shape, mirrored: false, opacity: 0.4)
        let result = RecordingCameraCompositor.composite(screen: desktop, camera: camera, overlay: overlay)
        let blended = pixel(result, x: 140, y: Int(30 + height / 2))
        expect(blended[0] > 100 && blended[0] < 220 && blended[1] > 150 && blended[1] < 240 && blended[3] == 255,
               "\(shape) can record the chosen camera transparency over an opaque desktop")
        expect(pixel(result, x: 100, y: 30)[1] > 250 && pixel(result, x: 20, y: 20)[1] > 250,
               "Recording transparency preserves clipped corners and the surrounding desktop")
    }
    let secondary = CGRect(x: -1920, y: -300, width: 1920, height: 1080)
    let region = RecordingTarget(displayID: 2, rect: CGRect(x: 100, y: 200, width: 640, height: 360), scale: 2, name: "Region")
    let mapped = RecordingCameraOverlay(windowFrame: CGRect(x: -1800, y: 280, width: 200, height: 200),
        screenFrame: secondary, target: region, shape: .circle, mirrored: false)
    expect(mapped.frame == CGRect(x: 40, y: 120, width: 400, height: 400),
           "Camera mapping handles negative display coordinates, region offsets, Retina scale, and vertical origin")
    let clipped = RecordingCameraOverlay(windowFrame: CGRect(x: -60, y: -60, width: 120, height: 120),
        screenFrame: screenFrame, target: target, shape: .circle, mirrored: false)
    let partial = RecordingCameraCompositor.composite(screen: desktop, camera: red.cropped(to: screenFrame), overlay: clipped)
    expect(partial.extent == desktop.extent && pixel(partial, x: 10, y: 10)[0] > 250,
           "A camera crossing the recording boundary is clipped to the output")
    return checks
}
