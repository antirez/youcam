import AppKit

enum CameraTransparency {
    static let preferenceKey = "cameraPreviewTransparency"
    static let recordingPreferenceKey = "recordCameraTransparency"

    static func normalized(_ value: Double) -> Double {
        value.isFinite ? min(0.8, max(0, value)) : 0
    }
}

@MainActor
final class CameraTransparencyControl: NSView {
    let slider: NSSlider
    private let value = NSTextField(labelWithString: "")
    private let onChange: (Double) -> Void

    init(transparency: Double, onChange: @escaping (Double) -> Void) {
        self.onChange = onChange
        slider = NSSlider(value: transparency * 100, minValue: 0, maxValue: 80, target: nil, action: nil)
        super.init(frame: NSRect(x: 0, y: 0, width: 300, height: 64))
        let caption = NSTextField(labelWithString: "Transparency")
        caption.font = .systemFont(ofSize: 12, weight: .medium)
        caption.frame = NSRect(x: 14, y: 39, width: 205, height: 16)
        value.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        value.alignment = .right
        value.frame = NSRect(x: 226, y: 39, width: 60, height: 16)
        slider.frame = NSRect(x: 12, y: 5, width: 276, height: 26)
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(changeTransparency)
        slider.setAccessibilityLabel("Camera transparency in percent")
        slider.setAccessibilityHelp("Adjusts camera transparency.")
        for child in [caption, value, slider] { addSubview(child) }
        updateValue()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    @objc private func changeTransparency() {
        updateValue()
        onChange(slider.doubleValue / 100)
    }

    private func updateValue() { value.stringValue = String(format: "%.0f%%", slider.doubleValue) }

}

@MainActor
final class PersistentMenuToggle: NSButton {
    private let checkmark = NSImageView()
    private let onChange: (Bool) -> Void
    private var tracking: NSTrackingArea?
    private var hovered = false

    init(title: String, enabled: Bool, onChange: @escaping (Bool) -> Void) {
        self.onChange = onChange
        super.init(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        self.title = title
        setButtonType(.toggle)
        isBordered = false
        state = enabled ? .on : .off
        autoresizingMask = [.width]
        target = self
        action = #selector(toggleSelection)
        checkmark.image = NSImage(systemSymbolName: "checkmark", accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: 11, weight: .semibold))
        checkmark.imageScaling = .scaleProportionallyDown
        addSubview(checkmark)
        setAccessibilityRole(.checkBox)
        setAccessibilityLabel(title)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func draw(_ dirtyRect: NSRect) {
        guard let item = enclosingMenuItem else { return }
        let highlighted = hovered || isHighlighted
        if highlighted {
            NSColor.selectedContentBackgroundColor.setFill()
            NSBezierPath(roundedRect: bounds.insetBy(dx: 5, dy: 0), xRadius: 8, yRadius: 8).fill()
        }
        let color = highlighted ? NSColor.selectedMenuItemTextColor : NSColor.textColor
        let label = NSAttributedString(string: title, attributes: [
            .font: item.menu?.font ?? NSFont.menuFont(ofSize: 0), .foregroundColor: color
        ])
        let textLeft: CGFloat = 14
        label.draw(at: NSPoint(x: textLeft, y: floor((bounds.height - label.size().height) / 2)))
        checkmark.contentTintColor = color
        checkmark.isHidden = state != .on
        let size = checkmark.image?.size ?? NSSize(width: 11, height: 11)
        checkmark.frame = NSRect(x: textLeft + label.size().width + 8, y: (bounds.height - size.height) / 2 + 0.5,
                                width: size.width, height: size.height)
    }

    override func hitTest(_ point: NSPoint) -> NSView? { super.hitTest(point) == nil ? nil : self }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        tracking = area
    }

    override func mouseEntered(with event: NSEvent) { hovered = true; needsDisplay = true }
    override func mouseExited(with event: NSEvent) { hovered = false; needsDisplay = true }

    @objc private func toggleSelection() {
        needsDisplay = true
        onChange(state == .on)
    }
}
