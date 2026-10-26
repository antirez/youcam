import Foundation
import CoreImage.CIFilterBuiltins

// Output-pixel coordinates, with the bottom-left origin used by Core Image.
// Opacity is independent of the preview unless the user opts to record it.
struct RecordingCameraOverlay: Equatable, Sendable {
    let frame: CGRect
    let cornerRadius: CGFloat
    let mirrored: Bool
    let opacity: CGFloat

    init(windowFrame: CGRect, screenFrame: CGRect, target: RecordingTarget,
         shape: CameraShape, mirrored: Bool, opacity: CGFloat = 1) {
        let local = RecordingTarget.localRect(windowFrame, screen: screenFrame)
        let scaleX = target.pixelSize.width / target.rect.width
        let scaleY = target.pixelSize.height / target.rect.height
        frame = CGRect(x: (local.minX - target.rect.minX) * scaleX,
                       y: (target.rect.maxY - local.maxY) * scaleY,
                       width: local.width * scaleX, height: local.height * scaleY)
        cornerRadius = (shape == .circle ? windowFrame.width / 2 : 14) * min(scaleX, scaleY)
        self.mirrored = mirrored
        self.opacity = opacity.isFinite ? min(1, max(0, opacity)) : 1
    }
}

enum RecordingCameraCompositor {
    static func composite(screen: CIImage, camera: CIImage, overlay: RecordingCameraOverlay) -> CIImage {
        let rect = overlay.frame
        var camera = camera
        if overlay.mirrored { camera = camera.oriented(.upMirrored) }
        let extent = camera.extent
        let scale = max(rect.width / extent.width, rect.height / extent.height)
        camera = camera.transformed(by: CGAffineTransform(translationX: -extent.minX, y: -extent.minY))
            .transformed(by: CGAffineTransform(scaleX: scale, y: scale))
            .transformed(by: CGAffineTransform(translationX: rect.midX - extent.width * scale / 2,
                                               y: rect.midY - extent.height * scale / 2))
            .cropped(to: rect)
        let mask = CIFilter.roundedRectangleGenerator()
        mask.extent = rect
        mask.radius = Float(overlay.cornerRadius)
        mask.color = CIColor(red: 1, green: 1, blue: 1, alpha: overlay.opacity)
        let blend = CIFilter.blendWithAlphaMask()
        blend.inputImage = camera
        blend.backgroundImage = screen
        blend.maskImage = mask.outputImage!.composited(over: CIImage(color: .clear)).cropped(to: screen.extent)
        return blend.outputImage!.cropped(to: screen.extent)
    }
}
