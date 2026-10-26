import AppKit
import AVFoundation

@MainActor
final class CameraPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }

    init(frame: NSRect) {
        super.init(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel],
                   backing: .buffered, defer: false)
        title = "YouCam"
        backgroundColor = .clear
        isOpaque = false
        hasShadow = false
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        hidesOnDeactivate = false
        isReleasedWhenClosed = false
        isMovableByWindowBackground = false
        animationBehavior = .none
    }
}

@MainActor
final class CameraPreviewView: NSView {
    let previewLayer = AVCaptureVideoPreviewLayer()
    private let clippingLayer = CALayer()
    private let placeholder = NSView()
    private let icon = NSImageView()
    private let heading = NSTextField(labelWithString: "Starting camera…")
    private let detail = NSTextField(wrappingLabelWithString: "Your camera will appear here.")
    private var tracking: NSTrackingArea?
    private var smoothedFace: DetectedFace?

    var shape: CameraShape = .circle {
        didSet {
            needsLayout = true
            applyCenterStageTransform(animated: true)
        }
    }
    var mirrored = true { didSet { updateMirroring() } }
    var onResize: ((CGFloat) -> Void)?
    var makeMenu: (() -> NSMenu)?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.clear.cgColor
        clippingLayer.masksToBounds = true
        clippingLayer.backgroundColor = NSColor(calibratedWhite: 0.09, alpha: 1).cgColor
        layer?.addSublayer(clippingLayer)
        previewLayer.videoGravity = .resizeAspectFill
        clippingLayer.addSublayer(previewLayer)

        placeholder.wantsLayer = true
        addSubview(placeholder)
        icon.image = NSImage(systemSymbolName: "video", accessibilityDescription: nil)
        icon.contentTintColor = NSColor.white.withAlphaComponent(0.7)
        icon.imageScaling = .scaleProportionallyUpOrDown
        heading.textColor = .white
        heading.font = .systemFont(ofSize: 15, weight: .semibold)
        heading.alignment = .center
        detail.textColor = NSColor.white.withAlphaComponent(0.65)
        detail.font = .systemFont(ofSize: 12)
        detail.alignment = .center
        detail.maximumNumberOfLines = 3
        for view in [icon, heading, detail] { placeholder.addSubview(view) }
        setAccessibilityElement(true)
        setAccessibilityRole(.image)
        setAccessibilityLabel("Camera overlay")
        setAccessibilityHelp("Drag to move. Scroll to resize. Right-click for camera controls.")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func updateFaceTracking(_ detected: DetectedFace?) {
        guard let detected else {
            if smoothedFace != nil {
                smoothedFace = nil
                applyCenterStageTransform(animated: true)
            }
            return
        }
        if let current = smoothedFace, current.isNativeCenterStageCamera == detected.isNativeCenterStageCamera {
            // Deadzone: ignore micro-movements (< 1.2% of frame) so the GPU rests when you sit still
            let dx = detected.center.x - current.center.x
            let dy = detected.center.y - current.center.y
            let dw = abs(detected.size.width - current.size.width)
            if hypot(dx, dy) < 0.012 && dw < 0.02 { return }

            let alpha: CGFloat = 0.22
            smoothedFace = DetectedFace(
                center: CGPoint(
                    x: current.center.x * (1.0 - alpha) + detected.center.x * alpha,
                    y: current.center.y * (1.0 - alpha) + detected.center.y * alpha
                ),
                size: CGSize(
                    width: current.size.width * (1.0 - alpha) + detected.size.width * alpha,
                    height: current.size.height * (1.0 - alpha) + detected.size.height * alpha
                ),
                isNativeCenterStageCamera: detected.isNativeCenterStageCamera
            )
        } else {
            smoothedFace = detected
        }
        applyCenterStageTransform(animated: true)
    }

    /// Soft-knee deceleration curve: tracks 1:1 near the center, smoothly decelerates as it approaches
    /// `maxLimit`, and never exceeds `maxLimit` (preventing hard stops and black borders).
    private func softClamp(_ value: CGFloat, maxLimit: CGFloat) -> CGFloat {
        guard maxLimit > 0.5 else { return 0 }
        // tanh(x) smoothly asymptotes to +/- 1.0. Dividing by 0.96 ensures we can reach ~96% of the
        // physical edge with a gentle deceleration cushion in the outer 35% of travel.
        let normalized = value / maxLimit
        let softened = CGFloat(tanh(Double(normalized * 0.95)))
        return maxLimit * softened
    }

    private func applyCenterStageTransform(animated: Bool) {
        let maskBounds = clippingLayer.bounds
        guard maskBounds.width > 0, maskBounds.height > 0 else { return }

        // Give `previewLayer` the full 16:9 uncropped aspect canvas that completely covers `maskBounds`.
        // Because `previewLayer.bounds` is 16:9, AVFoundation never pre-clips the left/right sides of the
        // camera feed in Circle or Square mode!
        let cameraAspect: CGFloat = 16.0 / 9.0
        let maskAspect = maskBounds.width / maskBounds.height
        let canvasWidth: CGFloat
        let canvasHeight: CGFloat
        if maskAspect < cameraAspect {
            // Circle or Square (1:1): height matches mask, width extends 1.777x so full 16:9 sensor is present
            canvasHeight = maskBounds.height
            canvasWidth = maskBounds.height * cameraAspect
        } else {
            // Rectangle (16:9): matches mask
            canvasWidth = maskBounds.width
            canvasHeight = maskBounds.width / cameraAspect
        }

        CATransaction.begin()
        if animated {
            CATransaction.setAnimationDuration(0.24)
            CATransaction.setAnimationTimingFunction(CAMediaTimingFunction(name: .easeOut))
        } else {
            CATransaction.setDisableActions(true)
        }

        // Keep `previewLayer` geometry anchored at the center of `clippingLayer` with full 16:9 bounds
        previewLayer.bounds = CGRect(x: 0, y: 0, width: canvasWidth, height: canvasHeight)
        previewLayer.position = CGPoint(x: maskBounds.midX, y: maskBounds.midY)

        if let face = smoothedFace {
            // Compute adaptive zoom:
            let zoom: CGFloat
            if face.isNativeCenterStageCamera {
                // Continuity Camera: in Circle/Square we already have 77% horizontal overhang from the 16:9 canvas,
                // so we only need 1.06x for subtle vertical headroom. In Rectangle, 1.15x gives gentle pan room.
                zoom = (shape == .rectangle) ? 1.15 : 1.06
            } else {
                // Built-in Mac webcam:
                // In Circle/Square, the 16:9 canvas already provides huge horizontal pan range, so 1.10x is plenty
                // (and backs off to 1.02x if you lean in close). In Rectangle (16:9), use 1.28x.
                let baseZoom: CGFloat = (shape == .rectangle) ? 1.28 : 1.10
                let closenessPenalty = max(0, (face.size.height - 0.32) * 0.70)
                zoom = max(1.02, baseZoom - closenessPenalty)
            }

            // Visual coordinates (0..1) inside the 16:9 video canvas
            let visualX = mirrored ? (1.0 - face.center.x) : face.center.x
            let targetYBias: CGFloat = (shape == .circle) ? 0.02 : 0.04
            let visualY = min(0.92, face.center.y + face.size.height * targetYBias)

            // Total scaled dimensions of the 16:9 video canvas
            let scaledCanvasW = canvasWidth * zoom
            let scaledCanvasH = canvasHeight * zoom

            // Exact physical distance from the center of the mask to the edge of the scaled 16:9 canvas.
            // Because `previewLayer.bounds` is `canvasWidth x canvasHeight`, shifting up to `maxPanX` / `maxPanY`
            // is 100% guaranteed to have real video pixels and never expose `clippingLayer`'s black background.
            let maxPanX = max(0, (scaledCanvasW - maskBounds.width) / 2.0)
            let maxPanY = max(0, (scaledCanvasH - maskBounds.height) / 2.0)

            // Desired shift to place (visualX, visualY) at the center of the mask
            let desiredShiftX = (0.5 - visualX) * scaledCanvasW
            let desiredShiftY = (0.5 - visualY) * scaledCanvasH

            // Apply soft-knee deceleration so movement slows gracefully near the frame boundary
            let shiftX = softClamp(desiredShiftX, maxLimit: maxPanX)
            let shiftY = softClamp(desiredShiftY, maxLimit: maxPanY)

            var transform = CATransform3DIdentity
            transform = CATransform3DTranslate(transform, shiftX, shiftY, 0)
            transform = CATransform3DScale(transform, zoom, zoom, 1.0)
            previewLayer.transform = transform
        } else {
            previewLayer.transform = CATransform3DIdentity
        }

        CATransaction.commit()
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        clippingLayer.frame = bounds
        clippingLayer.cornerRadius = shape == .circle ? bounds.width / 2 : 14
        applyCenterStageTransform(animated: false)
        placeholder.frame = bounds
        let width = bounds.width * (shape == .circle ? 0.72 : 0.86)
        let left = (bounds.width - width) / 2
        let center = bounds.midY
        let compact = bounds.height < 160 || bounds.width < 200
        icon.isHidden = compact
        detail.isHidden = compact
        icon.frame = NSRect(x: bounds.midX - 17, y: center + 24, width: 34, height: 26)
        heading.font = .systemFont(ofSize: compact ? 11 : 15, weight: .semibold)
        heading.frame = NSRect(x: left, y: compact ? center - 10 : center - 5, width: width, height: 22)
        detail.frame = NSRect(x: left, y: center - 62, width: width, height: 48)
        CATransaction.commit()
    }

    func display(_ state: CameraState) {
        if let message = state.message {
            heading.stringValue = message.title
            detail.stringValue = message.detail
            placeholder.isHidden = false
            previewLayer.isHidden = true
        } else {
            placeholder.isHidden = true
            previewLayer.isHidden = false
        }
        setAccessibilityValue(state.menuTitle)
        updateMirroring()
    }

    func updateMirroring() {
        guard let connection = previewLayer.connection, connection.isVideoMirroringSupported else { return }
        connection.automaticallyAdjustsVideoMirroring = false
        connection.isVideoMirrored = mirrored
        applyCenterStageTransform(animated: false)
    }

    private func containsVideo(_ point: NSPoint) -> Bool {
        if shape == .circle {
            let dx = point.x - bounds.midX
            let dy = point.y - bounds.midY
            return dx * dx + dy * dy <= pow(bounds.width / 2, 2)
        }
        return NSBezierPath(roundedRect: bounds, xRadius: 14, yRadius: 14).contains(point)
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        containsVideo(convert(point, from: superview)) ? self : nil
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        if containsVideo(convert(event.locationInWindow, from: nil)) { NSCursor.openHand.set() }
        else { NSCursor.arrow.set() }
    }

    override func mouseEntered(with event: NSEvent) { mouseMoved(with: event) }
    override func mouseExited(with event: NSEvent) { NSCursor.arrow.set() }

    override func mouseDown(with event: NSEvent) {
        guard let window else { return }
        if event.modifierFlags.contains(.control) {
            rightMouseDown(with: event)
            return
        }
        window.performDrag(with: event)
    }

    override func rightMouseDown(with event: NSEvent) {
        guard let menu = makeMenu?() else { return }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }

    override func scrollWheel(with event: NSEvent) {
        guard event.momentumPhase.isEmpty else { return }
        let delta = event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 1 : 8)
        onResize?(delta)
    }
}
