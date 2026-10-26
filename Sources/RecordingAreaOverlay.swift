import AppKit

// A persistent, click-through guide. The recording filter excludes every
// YouCam window, so the shading and handles never enter the movie. The camera
// is composed separately from its original frames.
@MainActor
final class RecordingAreaOverlay {
    private(set) var window: NSPanel?
    private(set) var handles: [RecordingAreaCorner: NSPanel] = [:]
    private var target: RecordingTarget?
    private var screen: NSScreen?
    var onChange: ((RecordingTarget) -> Void)?
    var isEditable = true {
        didSet {
            guard isEditable != oldValue else { return }
            updateHandleVisibility()
        }
    }

    func show(_ target: RecordingTarget, on screen: NSScreen) {
        self.target = target
        self.screen = screen
        if window == nil {
            let panel = NSPanel(contentRect: screen.frame, styleMask: [.borderless, .nonactivatingPanel],
                                backing: .buffered, defer: false)
            panel.title = "YouCam Recording Area"
            panel.isOpaque = false
            panel.backgroundColor = .clear
            panel.hasShadow = false
            panel.ignoresMouseEvents = true
            // Leave the camera and recording controls above the dimmed desktop.
            panel.level = NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue - 1)
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
            panel.hidesOnDeactivate = false
            panel.isReleasedWhenClosed = false
            panel.contentView = RecordingAreaShadeView(frame: CGRect(origin: .zero, size: screen.frame.size))
            window = panel
        }
        if handles.isEmpty {
            for corner in RecordingAreaCorner.allCases {
                let panel = RecordingAreaHandlePanel(contentRect: CGRect(x: 0, y: 0, width: 24, height: 24),
                    styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
                panel.title = "YouCam Recording Area — \(corner.rawValue)"
                panel.isOpaque = false
                panel.backgroundColor = .clear
                panel.hasShadow = false
                // Keep edge handles reachable even over the menu bar or Dock.
                panel.level = NSWindow.Level(rawValue: NSWindow.Level.statusBar.rawValue + 1)
                panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
                panel.hidesOnDeactivate = false
                panel.isReleasedWhenClosed = false
                let handle = RecordingAreaHandleView(frame: CGRect(origin: .zero, size: panel.frame.size))
                handle.toolTip = "Drag to resize the recording area"
                handle.setAccessibilityElement(true)
                handle.setAccessibilityRole(.handle)
                handle.setAccessibilityLabel("\(corner.rawValue) recording area corner")
                handle.onDrag = { [weak self] point in self?.resize(corner, to: point) }
                panel.contentView = handle
                handles[corner] = panel
            }
        }
        updateFrames()
        window?.orderFrontRegardless()
        updateHandleVisibility()
    }

    private var selection: CGRect? {
        guard let target, let screen else { return nil }
        return CGRect(x: screen.frame.minX + target.rect.minX, y: screen.frame.maxY - target.rect.maxY,
                      width: target.rect.width, height: target.rect.height)
    }

    private func updateFrames() {
        guard let screen, let selection, let window else { return }
        window.setFrame(screen.frame, display: true)
        (window.contentView as? RecordingAreaShadeView)?.selection = selection.offsetBy(
            dx: -screen.frame.minX, dy: -screen.frame.minY)
        for (corner, handle) in handles {
            let point = corner.point(in: selection)
            handle.setFrameOrigin(CGPoint(x: point.x - handle.frame.width / 2,
                                          y: point.y - handle.frame.height / 2))
        }
    }

    private func updateHandleVisibility() {
        for handle in handles.values {
            if isEditable {
                handle.orderFrontRegardless()
            } else {
                (handle.contentView as? RecordingAreaHandleView)?.cancelDrag()
                handle.orderOut(nil)
            }
        }
    }

    private func resize(_ corner: RecordingAreaCorner, to point: CGPoint) {
        guard isEditable, let target, let screen, let selection else { return }
        let rect = corner.resized(selection, to: point, within: screen.frame, scale: screen.backingScaleFactor)
        let updated = RecordingTarget(displayID: target.displayID,
            rect: RecordingTarget.localRect(rect, screen: screen.frame), scale: screen.backingScaleFactor,
            name: "\(Int(rect.width)) × \(Int(rect.height)) on \(screen.localizedName)")
        guard updated.rect != target.rect else { return }
        self.target = updated
        updateFrames()
        onChange?(updated)
    }

    func hide() {
        for handle in handles.values {
            (handle.contentView as? RecordingAreaHandleView)?.cancelDrag()
            handle.orderOut(nil)
        }
        handles.removeAll()
        window?.orderOut(nil)
        window = nil
        target = nil
        screen = nil
    }
}

private final class RecordingAreaHandlePanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
    // A corner on the screen edge must not be shifted below the menu bar.
    override func constrainFrameRect(_ frameRect: NSRect, to screen: NSScreen?) -> NSRect { frameRect }
}

@MainActor
final class RecordingAreaHandleView: NSView {
    var onDrag: ((CGPoint) -> Void)?
    private var grabOffset: CGPoint?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .crosshair) }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.clear.setFill()
        bounds.fill(using: .copy)
        let dot = NSBezierPath(ovalIn: bounds.insetBy(dx: 6, dy: 6))
        NSColor.white.setFill()
        dot.fill()
        NSColor.black.withAlphaComponent(0.7).setStroke()
        dot.lineWidth = 1.5
        dot.stroke()
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        grabOffset = CGPoint(x: point.x - bounds.midX, y: point.y - bounds.midY)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let grabOffset, let window else { return }
        let point = window.convertPoint(toScreen: event.locationInWindow)
        onDrag?(CGPoint(x: point.x - grabOffset.x, y: point.y - grabOffset.y))
    }

    override func mouseUp(with event: NSEvent) {
        mouseDragged(with: event)
        cancelDrag()
    }

    func cancelDrag() { grabOffset = nil }
}

@MainActor
final class RecordingAreaShadeView: NSView {
    var selection = CGRect.zero { didSet { needsDisplay = true } }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.clear.setFill()
        bounds.fill(using: .copy)
        let shade = NSBezierPath(rect: bounds)
        let selected = selection.intersection(bounds)
        if !selected.isNull { shade.appendRect(selected) }
        shade.windingRule = .evenOdd
        NSColor.black.withAlphaComponent(0.4).setFill()
        shade.fill()
        if !selected.isNull {
            NSColor.white.withAlphaComponent(0.9).setStroke()
            let border = NSBezierPath(rect: selected.insetBy(dx: -0.5, dy: -0.5))
            border.lineWidth = 1
            border.stroke()
        }
    }
}
