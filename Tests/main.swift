import AppKit

@MainActor
func runTests() async {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        guard condition() else { fatalError("FAIL: \(message)") }
    }
    func near(_ first: CGFloat, _ second: CGFloat) -> Bool { abs(first - second) < 0.01 }

    let screen = CGRect(x: 0, y: 25, width: 1440, height: 850)
    for shape in CameraShape.allCases {
        let initial = OverlayGeometry.initial(width: 280, shape: shape, within: screen)
        expect(near(initial.width / initial.height, shape.aspectRatio), "\(shape) has its expected aspect ratio")
        expect(screen.contains(initial), "\(shape) starts within the visible screen")
        let bigger = OverlayGeometry.resized(initial, width: 400, shape: shape, within: screen)
        expect(screen.contains(bigger), "\(shape) stays on screen when enlarged")
        expect(near(bigger.width, 400), "\(shape) honors requested size")
        let maximum = OverlayGeometry.resized(initial, width: 10_000, shape: shape, within: screen)
        expect(screen.contains(maximum), "\(shape) maximum size fits on screen")
        expect(maximum.width <= OverlayGeometry.maximumWidth, "\(shape) has a maximum size")
        let minimum = OverlayGeometry.size(width: -50, shape: shape, within: screen)
        expect(near(minimum.width, OverlayGeometry.minimumWidth), "\(shape) has a minimum size")
        let invalid = OverlayGeometry.size(width: .nan, shape: shape, within: screen)
        expect(invalid.width.isFinite && invalid.height.isFinite, "\(shape) handles invalid saved width")
    }

    let secondary = CGRect(x: -1920, y: -400, width: 1920, height: 1050)
    let offscreen = CGRect(x: 50, y: 1800, width: 280, height: 280)
    let recovered = OverlayGeometry.resized(offscreen, width: 280, shape: .circle, within: secondary)
    expect(secondary.contains(recovered), "Disconnected display positions are constrained onto a remaining display")
    let tinyScreen = CGRect(x: 0, y: 0, width: 90, height: 80)
    expect(tinyScreen.contains(OverlayGeometry.initial(width: 280, shape: .circle, within: tinyScreen)),
           "Small displays take precedence over minimum size")
    let anchored = CGRect(x: 100, y: 300, width: 280, height: 280)
    let rectangle = OverlayGeometry.resized(anchored, width: 280, shape: .rectangle, within: screen)
    expect(near(rectangle.minX, anchored.minX) && near(rectangle.maxY, anchored.maxY),
           "Changing shape preserves the top-left anchor")

    let domain = "app.youcam.tests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: domain)!
    defer { defaults.removePersistentDomain(forName: domain) }
    defaults.set("unrecognized-shape", forKey: "shape")
    defaults.set(1_000_000, forKey: "originX")
    defaults.set(1_000_000, forKey: "originY")
    defaults.set(280, forKey: "width")

    let app = NSApplication.shared
    let delegate = AppDelegate(previewMode: true, defaults: defaults)
    app.delegate = delegate
    delegate.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    defer {
        delegate.applicationWillTerminate(Notification(name: NSApplication.willTerminateNotification))
        delegate.panel.orderOut(nil)
    }

    expect(delegate.shape == .circle, "Unknown saved shapes fall back to circle")
    expect(NSScreen.screens.contains { $0.visibleFrame.contains(delegate.panel.frame) },
           "An off-screen saved position recovers to an attached screen")
    expect(app.activationPolicy() == .regular, "App appears in the Dock")
    expect(!delegate.panel.styleMask.contains(.titled), "Overlay has no title bar")
    expect(!delegate.panel.isOpaque && delegate.panel.backgroundColor == .clear, "Overlay background is transparent")
    expect(!delegate.panel.hasShadow, "Overlay has no window shadow")
    expect(delegate.panel.level == .floating, "Overlay floats above ordinary windows")
    expect(delegate.panel.collectionBehavior.contains(.canJoinAllSpaces), "Overlay follows desktops")
    expect(delegate.panel.collectionBehavior.contains(.fullScreenAuxiliary), "Overlay supports full-screen spaces")
    expect(!delegate.panel.canBecomeKey && !delegate.panel.canBecomeMain, "Overlay does not take keyboard focus")
    checks += runCameraTransparencyTests(delegate, defaults: defaults)

    expect(delegate.recordingArea == .wholeScreen, "Recording area defaults to Whole Screen")
    let initialScreen = delegate.panel.screen ?? NSScreen.main!
    expect(delegate.selectedRecordingTarget?.rect == CGRect(origin: .zero, size: initialScreen.frame.size),
           "Default recording target covers the entire display")
    expect(delegate.areaOverlay.window == nil, "Whole-screen mode has no dimming overlay")
    let areaMenu = delegate.makeMenu().item(withTitle: "Recording Area")!.submenu!
    expect(areaMenu.item(withTitle: "Whole Screen")?.state == .on, "The area menu shows the default selection")
    let selectArea = areaMenu.item(withTitle: "Select Area…")!
    expect(app.sendAction(selectArea.action!, to: selectArea.target, from: selectArea), "Area selection is available from the camera menu")
    expect(delegate.regionPicker.isSelecting, "Choosing Select Area opens the rectangle picker")
    let pickerWindow = delegate.regionPicker.windows.first!
    let pickerView = pickerWindow.contentView!
    let pickedRect = CGRect(x: 90, y: 100, width: 320, height: 180)
    func mouse(_ type: NSEvent.EventType, _ point: NSPoint) -> NSEvent {
        NSEvent.mouseEvent(with: type, location: point, modifierFlags: [], timestamp: 0,
                          windowNumber: pickerWindow.windowNumber, context: nil, eventNumber: 0,
                          clickCount: 1, pressure: 1)!
    }
    pickerView.mouseDown(with: mouse(.leftMouseDown, pickedRect.origin))
    pickerView.mouseDragged(with: mouse(.leftMouseDragged, NSPoint(x: pickedRect.maxX, y: pickedRect.maxY)))
    pickerView.mouseUp(with: mouse(.leftMouseUp, NSPoint(x: pickedRect.maxX, y: pickedRect.maxY)))
    try? await Task.sleep(for: .milliseconds(20))
    expect(!delegate.regionPicker.isSelecting, "Releasing the drag completes area selection")
    expect(delegate.recorder.phase == .idle, "Choosing an area does not start a recording")
    expect(!delegate.recordingPanel.isVisible, "Choosing an area does not force the recording panel open")
    guard case .region(let selectedTarget) = delegate.recordingArea else { fatalError("The dragged area was not selected") }
    expect(selectedTarget.rect.size == pickedRect.size, "The recorder uses the selected area size")
    expect(near(selectedTarget.rect.minY, pickerWindow.frame.height - pickedRect.maxY), "Selection converts to the recording coordinate system")
    let guide = delegate.areaOverlay.window!
    expect(guide.isVisible, "The dimming remains visible after the picker closes")
    expect(guide.ignoresMouseEvents, "The persistent dimming lets clicks reach applications underneath")
    expect(guide.level < delegate.panel.level, "Camera and recording controls stay above the dimming")
    expect((guide.contentView as? RecordingAreaShadeView)?.selection == pickedRect, "The clear guide matches the exact recording region")
    expect(delegate.makeRecordingAreaMenu().item(withTitle: "Select Area…")?.state == .on, "The menu marks the selected-region mode")
    delegate.selectRecordingArea()
    expect(delegate.areaOverlay.window == nil, "Reselecting temporarily removes the previous guide")
    delegate.regionPicker.cancel()
    try? await Task.sleep(for: .milliseconds(20))
    expect(delegate.recordingArea == .region(selectedTarget), "Canceling a new selection preserves the previous area")
    expect(delegate.areaOverlay.window?.isVisible == true, "Canceling selection restores the previous dimming")
    checks += runRecordingAreaTests(delegate, defaults: defaults)

    // Launch with a fresh defaults instance, restoring the selected display,
    // crop and guide without starting capture or opening the recording panel.
    let savedArea = defaults.data(forKey: "recordingArea")!
    let restoredArea = AppDelegate(previewMode: true, defaults: UserDefaults(suiteName: domain)!)
    restoredArea.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    expect(restoredArea.selectedRecordingTarget == delegate.selectedRecordingTarget,
           "A new launch restores the selected area and current display scale")
    expect(restoredArea.areaOverlay.window?.isVisible == true && restoredArea.areaOverlay.handles.count == 4,
           "A new launch restores the dimming and editable corners")
    expect(restoredArea.recorder.phase == .idle && !restoredArea.recordingPanel.isVisible,
           "Restoring the area does not start recording or open the panel")
    expect(restoredArea.makeRecordingAreaMenu().item(withTitle: "Select Area…")?.state == .on,
           "A new launch marks the restored area in the menu")
    restoredArea.applicationWillTerminate(Notification(name: NSApplication.willTerminateNotification))
    restoredArea.panel.orderOut(nil)
    expect(RecordingAreaPreferences.load(from: defaults, screens: []) == .wholeScreen,
           "A disconnected saved display falls back to Whole Screen")
    expect(defaults.data(forKey: "recordingArea") == savedArea,
           "A temporary missing display does not erase the saved selection")
    delegate.setRecordingArea(.display(initialScreen.displayID))
    expect(RecordingAreaPreferences.load(from: defaults) == .display(initialScreen.displayID),
           "An explicit whole-display choice is remembered")
    RecordingAreaPreferences.save(.region(RecordingTarget(displayID: initialScreen.displayID,
        rect: CGRect(x: 0, y: 0, width: initialScreen.frame.width + 1, height: 100),
        scale: 1, name: "Out of bounds")), to: defaults)
    expect(RecordingAreaPreferences.load(from: defaults) == .wholeScreen,
           "An area that no longer fits the display falls back to Whole Screen")
    defaults.set(Data("invalid preferences".utf8), forKey: "recordingArea")
    expect(RecordingAreaPreferences.load(from: defaults) == .wholeScreen,
           "Corrupt saved areas do not prevent launching")
    delegate.setRecordingArea(.region(selectedTarget))

    // Render only our own guide, never the desktop beneath it.
    let shade = RecordingAreaShadeView(frame: CGRect(x: 0, y: 0, width: 800, height: 500))
    shade.selection = CGRect(x: 170, y: 120, width: 460, height: 260)
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 800, pixelsHigh: 500,
                                  bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                  isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    shade.draw(shade.bounds)
    NSGraphicsContext.restoreGraphicsState()
    expect(bitmap.colorAt(x: 400, y: 250)!.alphaComponent < 0.01, "The selected region remains completely clear")
    expect(abs(bitmap.colorAt(x: 10, y: 10)!.alphaComponent - 0.4) < 0.02, "The surrounding display is visibly dimmed")
    try! bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: ".build/recording-area.png"))
    let whole = delegate.makeRecordingAreaMenu().item(withTitle: "Whole Screen")!
    _ = app.sendAction(whole.action!, to: whole.target, from: whole)
    expect(delegate.recordingArea == .wholeScreen && delegate.areaOverlay.window == nil,
           "Choosing Whole Screen clears the region and removes all dimming")
    expect(RecordingAreaPreferences.load(from: defaults) == .wholeScreen,
           "Choosing Whole Screen replaces the saved region")

    var menu = delegate.makeMenu()
    let cameraMenu = menu.item(withTitle: "Camera")!.submenu!
    expect(cameraMenu.numberOfItems > 0, "Camera menu offers devices or a meaningful empty state")
    let cameraChoices = cameraMenu.items.filter { $0.representedObject is String }
    if let choice = cameraChoices.last {
        expect(app.sendAction(choice.action!, to: choice.target, from: choice), "Camera selection action is connected")
        expect(defaults.string(forKey: "cameraID") == choice.representedObject as? String, "Camera selection is saved")
        let updated = delegate.makeMenu().item(withTitle: "Camera")!.submenu!
        expect(updated.items.filter { $0.state == .on }.count == 1, "Exactly one camera is checked")
    }
    let shapeMenu = menu.item(withTitle: "Shape")!.submenu!
    expect(shapeMenu.numberOfItems == 3, "All three shapes are offered")

    for shape in CameraShape.allCases {
        let choice = shapeMenu.items.first { $0.representedObject as? String == shape.rawValue }!
        expect(app.sendAction(choice.action!, to: choice.target, from: choice), "\(shape) menu action is connected")
        delegate.preview.layoutSubtreeIfNeeded()
        expect(delegate.shape == shape, "\(shape) action changes the overlay")
        expect(near(delegate.panel.frame.width / delegate.panel.frame.height, shape.aspectRatio), "\(shape) action preserves aspect ratio")
        expect(defaults.string(forKey: "shape") == shape.rawValue, "\(shape) selection is saved")
        let updated = delegate.makeMenu().item(withTitle: "Shape")!.submenu!
        expect(updated.items.filter { $0.state == .on }.count == 1, "Exactly one shape is checked")
    }

    let sizeMenu = delegate.makeMenu().item(withTitle: "Size")!.submenu!
    for (name, width) in [("Small", 180), ("Medium", 280), ("Large", 400), ("Extra Large", 560)] {
        let choice = sizeMenu.item(withTitle: name)!
        expect(app.sendAction(choice.action!, to: choice.target, from: choice), "\(name) size action is connected")
        expect(near(delegate.panel.frame.width, CGFloat(width)), "\(name) size is applied")
        expect(near(defaults.double(forKey: "width"), CGFloat(width)), "\(name) size is saved")
    }
    let previousWidth = delegate.panel.frame.width
    let increase = sizeMenu.item(withTitle: "Increase Size")!
    _ = app.sendAction(increase.action!, to: increase.target, from: increase)
    expect(delegate.panel.frame.width > previousWidth, "Increase Size enlarges the overlay")
    let decrease = sizeMenu.item(withTitle: "Decrease Size")!
    _ = app.sendAction(decrease.action!, to: decrease.target, from: decrease)
    expect(near(delegate.panel.frame.width, previousWidth), "Decrease Size reverses the increase")

    expect(delegate.preview.mirrored, "Camera is mirrored by default")
    delegate.toggleMirror()
    expect(!delegate.preview.mirrored && !defaults.bool(forKey: "mirrored"), "Mirror toggle changes and saves preference")
    delegate.toggleVisibility()
    expect(!delegate.isVisible && !delegate.panel.isVisible, "Hide Camera hides the overlay")
    expect(delegate.makeMenu().item(withTitle: "Show Camera") != nil, "Hidden camera can be restored from the menu")
    delegate.toggleVisibility()
    expect(delegate.isVisible && delegate.panel.isVisible, "Show Camera restores the overlay")

    expect(!delegate.recordingPanel.isVisible, "The recording panel is hidden at startup")
    checks += runRecordingPanelTests(audio: delegate.audio)
    expect(delegate.makeMenu().item(withTitle: "Show Recording Panel") != nil, "The camera menu can show the recording panel")
    delegate.toggleRecordingPanel()
    expect(delegate.recordingPanel.isVisible, "The recording panel can be shown")
    let panelMenu = delegate.recordingPanel.makeMenu!()
    expect(panelMenu.item(withTitle: "Recording Area")?.submenu != nil, "Recording area choices are also in the panel context menu")
    expect(delegate.recordingPanel.controls.makeAreaMenu?().item(withTitle: "Whole Screen")?.state == .on,
           "The recording panel area button uses the same selection")
    expect(panelMenu.item(withTitle: "Audio Input")?.submenu != nil, "The panel offers the same microphone controls")
    let hideCamera = panelMenu.item(withTitle: "Hide Camera")!
    _ = app.sendAction(hideCamera.action!, to: hideCamera.target, from: hideCamera)
    expect(!delegate.panel.isVisible && delegate.recordingPanel.isVisible, "The panel can hide the camera independently")
    delegate.toggleVisibility()
    delegate.recordingPanel.controls.layoutSubtreeIfNeeded()
    delegate.recordingPanel.controls.displayIfNeeded()
    if let bitmap = delegate.recordingPanel.controls.bitmapImageRepForCachingDisplay(in: delegate.recordingPanel.controls.bounds) {
        delegate.recordingPanel.controls.cacheDisplay(in: delegate.recordingPanel.controls.bounds, to: bitmap)
        try! bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: ".build/recording-panel.png"))
    }
    delegate.toggleRecordingPanel()
    expect(!delegate.recordingPanel.isVisible, "The panel can be hidden independently")
    let audioMenu = delegate.makeMenu().item(withTitle: "Audio Input")!.submenu!
    let silent = audioMenu.item(withTitle: "No Audio")!
    _ = app.sendAction(silent.action!, to: silent.target, from: silent)
    expect(!delegate.audio.hasAudio && defaults.string(forKey: "audioID") == "none", "Microphone selection is persisted")
    delegate.audio.setGain(-12)
    expect(defaults.double(forKey: "audioGainDB") == -12, "Input level is persisted")

    // Render each shape locally to check transparency without screen-recording
    // permission, camera access, or capturing any other application's content.
    delegate.resize(to: 280)
    for shape in CameraShape.allCases {
        let choice = NSMenuItem()
        choice.representedObject = shape.rawValue
        delegate.selectShape(choice)
        delegate.preview.display(.noCamera)
        delegate.preview.layoutSubtreeIfNeeded()
        delegate.preview.displayIfNeeded()
        CATransaction.flush()
        let bounds = delegate.preview.bounds
        expect(delegate.preview.hitTest(NSPoint(x: bounds.midX, y: bounds.midY)) === delegate.preview,
               "\(shape) center accepts dragging")
        expect(delegate.preview.hitTest(NSPoint(x: 0, y: 0)) == nil, "\(shape) transparent corner is not an interaction target")
        let pixelsWide = Int(bounds.width * 2)
        let pixelsHigh = Int(bounds.height * 2)
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixelsWide, pixelsHigh: pixelsHigh,
                                      bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                      isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        let context = NSGraphicsContext(bitmapImageRep: bitmap)!
        context.cgContext.scaleBy(x: 2, y: 2)
        delegate.preview.layer!.render(in: context.cgContext)
        expect(bitmap.colorAt(x: 0, y: 0)!.alphaComponent < 0.01, "\(shape) corner renders transparent")
        expect(bitmap.colorAt(x: pixelsWide / 2, y: pixelsHigh / 2)!.alphaComponent > 0.99,
               "\(shape) center renders opaque")
        let output = URL(fileURLWithPath: ".build/preview-\(shape.rawValue).png")
        try! bitmap.representation(using: .png, properties: [:])!.write(to: output)
    }

    // A second launch should restore preferences using the saved frame.
    let savedFrame = delegate.panel.frame
    delegate.setCameraTransparency(0.45)
    let saveTransparency = delegate.makeMenu().item(withTitle: "Transparency")!.submenu!
        .item(withTitle: "Apply Transparency to Saved Video")!.view as! PersistentMenuToggle
    saveTransparency.performClick(nil)
    let restored = AppDelegate(previewMode: true, defaults: defaults)
    restored.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    expect(restored.shape == .rectangle, "Shape is restored on the next launch")
    expect(!restored.preview.mirrored, "Mirroring is restored on the next launch")
    expect(restored.panel.frame == savedFrame, "Size and position are restored on the next launch")
    expect(abs(restored.panel.alphaValue - 0.55) < 0.001 && restored.cameraTransparency == 0.45,
           "Preview transparency is restored on the next launch")
    expect(restored.saveCameraTransparency && abs(restored.recordingCameraOpacity - 0.55) < 0.001,
           "The option to record transparency is restored on the next launch")
    expect((restored.makeMenu().item(withTitle: "Transparency")!.submenu!
        .item(withTitle: "Apply Transparency to Saved Video")!.view as! PersistentMenuToggle).state == .on,
           "The saved-video checkmark matches the restored preference")
    restored.applicationWillTerminate(Notification(name: NSApplication.willTerminateNotification))
    restored.panel.orderOut(nil)

    menu = delegate.makeMenu()
    expect(menu.item(withTitle: "Quit YouCam")?.action != nil, "Menu always provides a quit action")
    print("Passed \(checks) checks: geometry, overlay, menu actions, rendering, and saved preferences.")
}

await runTests()
try await runRecordingTests()
