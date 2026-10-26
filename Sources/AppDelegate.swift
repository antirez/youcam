import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate, NSWindowDelegate {
    private let defaults: UserDefaults
    private let previewMode: Bool
    private let camera: CameraController
    let audio: AudioController
    let recorder: RecordingController
    let regionPicker = RegionPicker()
    let areaOverlay = RecordingAreaOverlay()
    private var hotKeys: GlobalHotKeys?
    private var sleepObserver: NSObjectProtocol?
    private(set) var recordingArea: RecordingArea = .wholeScreen
    private var isQuitting = false
    private(set) var recordingPanel: RecordingPanel!
    private var statusItem: NSStatusItem?
    private var screenObserver: NSObjectProtocol?
    private(set) var panel: CameraPanel!
    private(set) var preview: CameraPreviewView!
    private(set) var shape: CameraShape
    private(set) var isVisible = true
    private var mirrored: Bool
    private(set) var cameraTransparency: Double
    private(set) var saveCameraTransparency: Bool
    var recordingCameraOpacity: CGFloat { saveCameraTransparency ? 1 - cameraTransparency : 1 }

    init(previewMode: Bool = false, defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.previewMode = previewMode
        let movie = RecordingWriter()
        camera = CameraController(defaults: defaults, movie: movie)
        let audio = AudioController(defaults: defaults, movie: movie, previewMode: previewMode)
        self.audio = audio
        recorder = RecordingController(movie: movie, audio: audio)
        shape = CameraShape(rawValue: defaults.string(forKey: "shape") ?? "") ?? .circle
        mirrored = defaults.object(forKey: "mirrored") as? Bool ?? true
        cameraTransparency = CameraTransparency.normalized(defaults.double(forKey: CameraTransparency.preferenceKey))
        saveCameraTransparency = defaults.bool(forKey: CameraTransparency.recordingPreferenceKey)
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        recordingArea = RecordingAreaPreferences.load(from: defaults)

        panel = CameraPanel(frame: restoredFrame())
        panel.alphaValue = 1 - cameraTransparency
        panel.delegate = self
        preview = CameraPreviewView(frame: NSRect(origin: .zero, size: panel.frame.size))
        preview.shape = shape
        preview.mirrored = mirrored
        preview.previewLayer.session = camera.session
        preview.autoresizingMask = [.width, .height]
        preview.onResize = { [weak self] delta in
            guard let self else { return }
            self.resize(to: self.panel.frame.width + delta)
        }
        preview.makeMenu = { [weak self] in self?.makeMenu() ?? NSMenu() }
        panel.contentView = preview

        recordingPanel = RecordingPanel(audio: audio)
        recordingPanel.makeMenu = { [weak self] in self?.makeMenu() ?? NSMenu() }
        recordingPanel.controls.makeAreaMenu = { [weak self] in self?.makeRecordingAreaMenu() ?? NSMenu() }
        recordingPanel.controls.onPlay = { [weak self] in
            guard let self else { return }
            if self.recorder.phase == .paused { self.recorder.togglePause() }
            else if self.recorder.phase == .idle { self.toggleRecording() }
        }
        recordingPanel.controls.onPause = { [weak self] in
            guard let self, self.recorder.phase == .recording else { return }
            self.recorder.togglePause()
        }
        recordingPanel.controls.onStop = { [weak self] in
            Task { await self?.recorder.stop() }
        }
        recordingPanel.controls.onHide = { [weak self] in self?.toggleRecordingPanel() }
        recorder.onChange = { [weak self] in self?.updateRecordingUI() }
        recorder.onError = { [weak self] message in self?.showRecordingError(message) }
        areaOverlay.onChange = { [weak self] target in
            guard let self, !self.recorder.phase.busy, !self.isQuitting else { return }
            self.recordingArea = .region(target)
            RecordingAreaPreferences.save(self.recordingArea, to: self.defaults)
            self.updateRecordingUI()
        }
        if !previewMode {
            let keys = GlobalHotKeys()
            keys.onRecord = { [weak self] in self?.toggleRecording() }
            keys.onPause = { [weak self] in self?.recorder.togglePause() }
            hotKeys = keys
        }

        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = item.button {
            button.image = NSImage(systemSymbolName: "video.fill", accessibilityDescription: "YouCam")
            button.image?.isTemplate = true
            button.toolTip = "YouCam — camera overlay"
            button.setAccessibilityLabel("YouCam camera controls")
        }
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        statusItem = item

        let mainMenu = NSMenu()
        let appMenuItem = NSMenuItem(title: "YouCam", action: nil, keyEquivalent: "")
        let appMenu = makeMenu()
        appMenu.delegate = self
        appMenuItem.submenu = appMenu
        mainMenu.addItem(appMenuItem)
        NSApp.mainMenu = mainMenu

        camera.onChange = { [weak self] in
            guard let self else { return }
            self.preview.display(self.camera.state)
            self.updateRecordingUI()
        }
        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.screenParametersChanged() }
        }
        sleepObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.willSleepNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in await self?.recorder.stop() }
        }

        panel.orderFrontRegardless()
        refreshAreaOverlay()
        updateRecordingUI()
        if previewMode {
            camera.refreshDevices()
            preview.display(.idle)
        }
        else { camera.show() }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        if !previewMode { camera.checkAccess() }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard recorder.phase.busy else { return .terminateNow }
        isQuitting = true
        Task {
            await recorder.stop()
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        guard let panel else { return false }
        if recorder.phase.busy || recordingPanel.isVisible {
            if !recordingPanel.isVisible { toggleRecordingPanel() }
            recordingPanel.orderFrontRegardless()
            return false
        }
        if !isVisible { toggleVisibility() }
        fitOnScreen()
        panel.orderFrontRegardless()
        return false
    }

    // performDrag hands dragging to WindowServer and returns immediately.
    // Save actual movement notifications, rather than the initial mouse-down.
    func windowDidMove(_ notification: Notification) { saveGeometry() }

    func applicationWillTerminate(_ notification: Notification) {
        isQuitting = true
        saveGeometry()
        if let screenObserver { NotificationCenter.default.removeObserver(screenObserver) }
        if let sleepObserver { NSWorkspace.shared.notificationCenter.removeObserver(sleepObserver) }
        regionPicker.cancel()
        areaOverlay.hide()
        hotKeys?.shutdown()
        audio.shutdown()
        recordingPanel?.orderOut(nil)
        camera.shutdown()
    }

    private func restoredFrame() -> NSRect {
        let screen = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let width = defaults.object(forKey: "width") == nil ? 280 : defaults.double(forKey: "width")
        guard defaults.object(forKey: "originX") != nil, defaults.object(forKey: "originY") != nil else {
            return OverlayGeometry.initial(width: width, shape: shape, within: screen)
        }
        let x = defaults.double(forKey: "originX")
        let y = defaults.double(forKey: "originY")
        guard x.isFinite, y.isFinite else {
            return OverlayGeometry.initial(width: width, shape: shape, within: screen)
        }
        let size = OverlayGeometry.size(width: width, shape: shape, within: screen)
        let frame = NSRect(origin: NSPoint(x: x, y: y), size: size)
        let destination = NSScreen.screens.max {
            intersectionArea(frame, $0.visibleFrame) < intersectionArea(frame, $1.visibleFrame)
        }
        guard let destination, intersectionArea(frame, destination.visibleFrame) > 0 else {
            return OverlayGeometry.initial(width: width, shape: shape, within: screen)
        }
        return OverlayGeometry.resized(frame, width: width, shape: shape, within: destination.visibleFrame)
    }

    private func intersectionArea(_ first: NSRect, _ second: NSRect) -> CGFloat {
        let intersection = first.intersection(second)
        return intersection.isNull ? 0 : intersection.width * intersection.height
    }

    private var visibleScreen: NSRect {
        panel.screen?.visibleFrame ?? NSScreen.main?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
    }

    private func saveGeometry() {
        guard let panel else { return }
        defaults.set(panel.frame.width, forKey: "width")
        defaults.set(panel.frame.minX, forKey: "originX")
        defaults.set(panel.frame.minY, forKey: "originY")
        updateCameraOverlay()
    }

    func resize(to width: CGFloat) {
        panel.setFrame(OverlayGeometry.resized(panel.frame, width: width, shape: shape, within: visibleScreen), display: true)
        preview.needsLayout = true
        saveGeometry()
    }

    private func fitOnScreen() { resize(to: panel.frame.width) }

    var selectedRecordingTarget: RecordingTarget? {
        if recorder.phase.busy { return recorder.target }
        switch recordingArea {
        case .wholeScreen:
            return (panel.screen ?? NSScreen.main).map { .fullScreen($0) }
        case .display(let id):
            return NSScreen.screens.first(where: { $0.displayID == id }).map { .fullScreen($0) }
        case .region(let target):
            guard let screen = NSScreen.screens.first(where: { $0.displayID == target.displayID }) else { return nil }
            return RecordingTarget(displayID: target.displayID, rect: target.rect,
                                   scale: screen.backingScaleFactor, name: target.name)
        }
    }

    private var recordingAreaName: String {
        switch recordingArea {
        case .wholeScreen: "Whole Screen"
        case .display(let id): NSScreen.screens.first(where: { $0.displayID == id })?.localizedName ?? "Disconnected Display"
        case .region(let target): "Selected Area (\(Int(target.rect.width)) × \(Int(target.rect.height)))"
        }
    }

    func setRecordingArea(_ area: RecordingArea) {
        guard !recorder.phase.busy, !isQuitting else { return }
        recordingArea = area
        RecordingAreaPreferences.save(area, to: defaults)
        refreshAreaOverlay()
        updateRecordingUI()
    }

    private func refreshAreaOverlay() {
        guard !regionPicker.isSelecting, !isQuitting,
              case .region(let target) = recordingArea,
              let screen = NSScreen.screens.first(where: { $0.displayID == target.displayID }) else {
            areaOverlay.hide()
            return
        }
        areaOverlay.show(target, on: screen)
    }

    private func screenParametersChanged() {
        regionPicker.cancel()
        fitOnScreen()
        let valid: Bool
        switch recordingArea {
        case .wholeScreen: valid = true
        case .display(let id): valid = NSScreen.screens.contains { $0.displayID == id }
        case .region(let target):
            valid = NSScreen.screens.contains {
                $0.displayID == target.displayID && CGRect(origin: .zero, size: $0.frame.size).contains(target.rect)
            }
        }
        if !valid {
            recordingArea = .wholeScreen
            if recorder.phase.busy { Task { await recorder.stop() } }
        }
        refreshAreaOverlay()
        updateRecordingUI()
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        populate(menu)
    }

    func makeMenu() -> NSMenu {
        let menu = NSMenu()
        populate(menu)
        return menu
    }

    private func item(_ title: String, action: Selector? = nil, key: String = "") -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.target = self
        return item
    }

    private func populate(_ menu: NSMenu) {
        menu.autoenablesItems = false
        let title = item("YouCam")
        title.isEnabled = false
        menu.addItem(title)
        let state = item(camera.state.menuTitle)
        state.isEnabled = false
        menu.addItem(state)
        let recordingState = item(recorder.status)
        recordingState.isEnabled = false
        menu.addItem(recordingState)
        let area = item("Area: \(recordingAreaName)")
        area.isEnabled = false
        menu.addItem(area)
        menu.addItem(.separator())

        let startStop = item(recorder.phase.busy ? "Stop Recording" : "Start Recording", action: #selector(toggleRecording), key: "r")
        startStop.keyEquivalentModifierMask = [.control, .option, .command]
        startStop.isEnabled = recorder.phase != .finishing && !regionPicker.isSelecting
        menu.addItem(startStop)
        if recorder.phase == .recording || recorder.phase == .paused {
            let pause = item(recorder.phase == .paused ? "Resume Recording" : "Pause Recording", action: #selector(togglePause), key: "p")
            pause.keyEquivalentModifierMask = [.control, .option, .command]
            menu.addItem(pause)
        }
        let areaItem = item("Recording Area")
        areaItem.submenu = makeRecordingAreaMenu()
        areaItem.isEnabled = !recorder.phase.busy && !regionPicker.isSelecting
        menu.addItem(areaItem)
        menu.addItem(item(recordingPanel?.isVisible == true ? "Hide Recording Panel" : "Show Recording Panel",
                          action: #selector(toggleRecordingPanel)))
        if hotKeys?.available == false {
            let unavailable = item("Global shortcuts unavailable")
            unavailable.isEnabled = false
            menu.addItem(unavailable)
        }
        if recorder.lastURL != nil { menu.addItem(item("Show Last Recording in Finder", action: #selector(revealRecording))) }
        menu.addItem(.separator())

        menu.addItem(item(isVisible ? "Hide Camera" : "Show Camera", action: #selector(toggleVisibility), key: "h"))

        let cameras = NSMenu(title: "Camera")
        cameras.autoenablesItems = false
        if camera.devices.isEmpty {
            let empty = item("No cameras available")
            empty.isEnabled = false
            cameras.addItem(empty)
        } else {
            for device in camera.devices {
                let choice = item(device.localizedName, action: #selector(selectCamera(_:)))
                choice.representedObject = device.uniqueID
                choice.state = device.uniqueID == camera.selectedDeviceID ? .on : .off
                cameras.addItem(choice)
            }
        }
        let cameraItem = item("Camera")
        cameraItem.submenu = cameras
        menu.addItem(cameraItem)

        let audioMenu = NSMenu(title: "Audio Input")
        audioMenu.autoenablesItems = false
        audioMenu.delegate = audio
        let silent = item("No Audio", action: #selector(selectAudio(_:)))
        silent.representedObject = "none"
        silent.state = audio.hasAudio ? .off : .on
        silent.isEnabled = !recorder.phase.busy
        audioMenu.addItem(silent)
        for device in audio.devices {
            let choice = item(device.localizedName, action: #selector(selectAudio(_:)))
            choice.representedObject = device.uniqueID
            choice.state = device.uniqueID == audio.selectedID ? .on : .off
            choice.isEnabled = !recorder.phase.busy
            audioMenu.addItem(choice)
        }
        audioMenu.addItem(.separator())
        let level = NSMenuItem()
        level.view = AudioControlView(audio: audio)
        audioMenu.addItem(level)
        audioMenu.addItem(.separator())
        audioMenu.addItem(item("Microphone Settings…", action: #selector(openMicrophoneSettings)))
        let audioItem = item("Audio Input")
        audioItem.submenu = audioMenu
        menu.addItem(audioItem)

        let shapes = NSMenu(title: "Shape")
        for (index, shape) in CameraShape.allCases.enumerated() {
            let choice = item(shape.title, action: #selector(selectShape(_:)), key: "\(index + 1)")
            choice.representedObject = shape.rawValue
            choice.state = shape == self.shape ? .on : .off
            shapes.addItem(choice)
        }
        let shapeItem = item("Shape")
        shapeItem.submenu = shapes
        menu.addItem(shapeItem)

        let sizes = NSMenu(title: "Size")
        for (name, width) in [("Small", 180), ("Medium", 280), ("Large", 400), ("Extra Large", 560)] {
            let choice = item(name, action: #selector(selectSize(_:)))
            choice.tag = width
            choice.state = abs(panel.frame.width - CGFloat(width)) < 1 ? .on : .off
            sizes.addItem(choice)
        }
        sizes.addItem(.separator())
        sizes.addItem(item("Increase Size", action: #selector(increaseSize), key: "+"))
        sizes.addItem(item("Decrease Size", action: #selector(decreaseSize), key: "-"))
        let sizeItem = item("Size")
        sizeItem.submenu = sizes
        menu.addItem(sizeItem)

        let transparency = item("Transparency")
        let transparencyMenu = NSMenu(title: "Transparency")
        transparencyMenu.autoenablesItems = false
        let control = NSMenuItem()
        control.view = CameraTransparencyControl(transparency: cameraTransparency) { [weak self] value in
            self?.setCameraTransparency(value)
        }
        transparencyMenu.addItem(control)
        transparencyMenu.addItem(.separator())
        let saveTransparency = item("Apply Transparency to Saved Video")
        saveTransparency.view = PersistentMenuToggle(title: saveTransparency.title, enabled: saveCameraTransparency) { [weak self] enabled in
            self?.setSaveCameraTransparency(enabled)
        }
        transparencyMenu.addItem(saveTransparency)
        transparencyMenu.addItem(item("Reset Transparency to 0%", action: #selector(resetCameraTransparency)))
        transparency.submenu = transparencyMenu
        menu.addItem(transparency)

        let mirror = item("Mirror Camera", action: #selector(toggleMirror))
        mirror.state = mirrored ? .on : .off
        menu.addItem(mirror)
        menu.addItem(item("Move to Bottom Right", action: #selector(resetPosition)))
        menu.addItem(.separator())

        if camera.state == .permissionDenied {
            menu.addItem(item("Open Camera Settings…", action: #selector(openCameraSettings)))
        }
        menu.addItem(item("Retry Camera", action: #selector(retryCamera)))
        menu.addItem(item("How to Use YouCam", action: #selector(showHelp)))
        menu.addItem(.separator())
        menu.addItem(item("Quit YouCam", action: #selector(quit), key: "q"))
    }

    @objc func toggleVisibility() {
        isVisible.toggle()
        if isVisible {
            panel.orderFrontRegardless()
            if !previewMode { camera.show() }
        } else {
            panel.orderOut(nil)
            camera.hide()
        }
        updateRecordingUI()
    }

    private func updateRecordingUI() {
        updateCameraOverlay()
        areaOverlay.isEditable = !recorder.phase.busy && !regionPicker.isSelecting && !isQuitting
        recordingPanel?.controls.update(phase: recorder.phase, status: recorder.status,
                                        areaName: recordingAreaName, selectingArea: regionPicker.isSelecting)
        let symbol = recorder.phase == .recording ? "record.circle" : recorder.phase == .paused ? "pause.circle" : isVisible ? "video.fill" : "video.slash"
        statusItem?.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "YouCam")
        statusItem?.button?.image?.isTemplate = true
        statusItem?.button?.toolTip = "YouCam — \(recorder.phase.busy ? recorder.status : camera.state.menuTitle)"
    }

    @objc func toggleRecordingPanel() {
        if recordingPanel.isVisible {
            recordingPanel.orderOut(nil)
            audio.monitor("panel", enabled: false)
        } else {
            let screen = panel.screen?.visibleFrame ?? NSScreen.main!.visibleFrame
            recordingPanel.setFrameOrigin(NSPoint(x: screen.midX - recordingPanel.frame.width / 2, y: screen.minY + 40))
            recordingPanel.orderFrontRegardless()
            audio.monitor("panel", enabled: true)
            updateRecordingUI()
        }
    }

    @objc func toggleRecording() {
        guard !previewMode else { return }
        if recorder.phase.busy { Task { await recorder.stop() } }
        else if !regionPicker.isSelecting {
            guard let target = selectedRecordingTarget else { return }
            beginRecording(target)
        }
    }

    @objc private func togglePause() { recorder.togglePause() }

    func makeRecordingAreaMenu() -> NSMenu {
        let menu = NSMenu(title: "Recording Area")
        menu.autoenablesItems = false
        let enabled = !recorder.phase.busy && !regionPicker.isSelecting
        let whole = item("Whole Screen", action: #selector(selectWholeScreen))
        whole.state = recordingArea == .wholeScreen ? .on : .off
        whole.isEnabled = enabled
        menu.addItem(whole)
        let region = item("Select Area…", action: #selector(selectRecordingArea))
        if case .region = recordingArea { region.state = .on }
        region.isEnabled = enabled
        menu.addItem(region)
        if NSScreen.screens.count > 1 {
            menu.addItem(.separator())
            for screen in NSScreen.screens {
                let display = item("Whole Screen — \(screen.localizedName)", action: #selector(selectRecordingDisplay(_:)))
                display.representedObject = screen.displayID
                display.state = recordingArea == .display(screen.displayID) ? .on : .off
                display.isEnabled = enabled
                menu.addItem(display)
            }
        }
        return menu
    }

    @objc func selectWholeScreen() { setRecordingArea(.wholeScreen) }

    @objc func selectRecordingArea() {
        guard !recorder.phase.busy, !regionPicker.isSelecting else { return }
        areaOverlay.hide()
        regionPicker.select { [weak self] target in
            guard let self, !self.isQuitting else { return }
            if let target { self.setRecordingArea(.region(target)) }
            else {
                self.refreshAreaOverlay()
                self.updateRecordingUI()
            }
        }
        updateRecordingUI()
    }

    @objc private func selectRecordingDisplay(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? CGDirectDisplayID,
              NSScreen.screens.contains(where: { $0.displayID == id }) else { return }
        setRecordingArea(.display(id))
    }

    private func beginRecording(_ target: RecordingTarget) {
        guard !previewMode, !recorder.phase.busy else { return }
        Task {
            await recorder.start(target: target)
        }
    }

    @objc private func selectAudio(_ sender: NSMenuItem) {
        guard !recorder.phase.busy, let id = sender.representedObject as? String else { return }
        audio.select(id)
    }

    @objc private func openMicrophoneSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func revealRecording() {
        if let url = recorder.lastURL { NSWorkspace.shared.activateFileViewerSelecting([url]) }
    }

    private func showRecordingError(_ message: String) {
        guard !isQuitting, !previewMode else { return }
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Recording stopped"
        alert.informativeText = message
        alert.addButton(withTitle: "OK")
        let screenPermission = message.localizedCaseInsensitiveContains("screen recording")
        if screenPermission { alert.addButton(withTitle: "Screen Recording Settings…") }
        if alert.runModal() == .alertSecondButtonReturn, screenPermission,
           let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func selectCamera(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        camera.selectCamera(id: id)
    }

    @objc func selectShape(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String, let shape = CameraShape(rawValue: raw) else { return }
        self.shape = shape
        defaults.set(shape.rawValue, forKey: "shape")
        preview.shape = shape
        resize(to: panel.frame.width)
    }

    @objc private func selectSize(_ sender: NSMenuItem) { resize(to: CGFloat(sender.tag)) }
    @objc private func increaseSize() { resize(to: panel.frame.width * 1.15) }
    @objc private func decreaseSize() { resize(to: panel.frame.width / 1.15) }

    @objc func toggleMirror() {
        mirrored.toggle()
        defaults.set(mirrored, forKey: "mirrored")
        preview.mirrored = mirrored
        updateCameraOverlay()
    }

    func setCameraTransparency(_ value: Double) {
        cameraTransparency = CameraTransparency.normalized(value)
        defaults.set(cameraTransparency, forKey: CameraTransparency.preferenceKey)
        panel.alphaValue = 1 - cameraTransparency
        updateCameraOverlay()
    }

    @objc private func resetCameraTransparency() { setCameraTransparency(0) }

    private func setSaveCameraTransparency(_ enabled: Bool) {
        saveCameraTransparency = enabled
        defaults.set(saveCameraTransparency, forKey: CameraTransparency.recordingPreferenceKey)
        updateCameraOverlay()
    }

    private func updateCameraOverlay() {
        guard let panel, recorder.phase.busy, isVisible, case .running = camera.state,
              let target = recorder.target,
              let screen = NSScreen.screens.first(where: { $0.displayID == target.displayID }) else {
            recorder.movie.setCameraOverlay(nil)
            return
        }
        let overlay = RecordingCameraOverlay(windowFrame: panel.frame, screenFrame: screen.frame,
                                             target: target, shape: shape, mirrored: mirrored,
                                             opacity: recordingCameraOpacity)
        let output = CGRect(origin: .zero, size: target.pixelSize)
        recorder.movie.setCameraOverlay(overlay.frame.intersects(output) ? overlay : nil)
    }

    @objc private func resetPosition() {
        let screen = NSScreen.main?.visibleFrame ?? visibleScreen
        panel.setFrame(OverlayGeometry.initial(width: panel.frame.width, shape: shape, within: screen), display: true)
        saveGeometry()
    }

    @objc private func retryCamera() {
        if !isVisible { toggleVisibility() }
        else if !previewMode { camera.retry() }
    }

    @objc private func openCameraSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Camera") else { return }
        NSWorkspace.shared.open(url)
    }

    @objc private func showHelp() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Your camera, without the window."
        alert.informativeText = "Drag the camera to move it. Scroll to resize. Right-click the camera or recording panel for controls.\n\nYour recording area is remembered across restarts. Choose Select Area and drag a rectangle to prepare a region. The surrounding screen stays dimmed; your selected area stays clear. Drag the white corner dots to fine-tune the area before recording. Choose Whole Screen to remove the dimming.\n\nStart Recording when ready. Movies are saved automatically to your Desktop. Control–Option–Command–R starts/stops; Control–Option–Command–P pauses/resumes.\n\nAudio Input selects the microphone and offers an input-level slider and live meter.\n\nShow Recording Panel displays optional controls. The controls and dimming stay out of your recordings. Hide Camera lets you record just the screen."
        alert.informativeText += "\n\nTransparency optionally lets you see through the camera while you work. Adjust it from the camera menu; Reset Transparency to 0% turns it off. Check Apply Transparency to Saved Video to use the same transparency in your recordings, or leave it unchecked to keep the recorded camera opaque. Both choices are remembered."
        alert.addButton(withTitle: "Got It")
        alert.runModal()
    }

    @objc private func quit() { NSApp.terminate(nil) }
}
