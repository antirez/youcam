import AVFoundation
import Foundation
import Vision

enum CameraState: Equatable, Sendable {
    case idle
    case requestingAccess
    case starting(String)
    case running(String)
    case noCamera
    case permissionDenied
    case failed(String)

    var menuTitle: String {
        switch self {
        case .idle: "Camera paused"
        case .requestingAccess: "Waiting for camera access…"
        case .starting(let name): "Starting \(name)…"
        case .running(let name): name
        case .noCamera: "No camera connected"
        case .permissionDenied: "Camera access required"
        case .failed: "Camera unavailable"
        }
    }

    var message: (title: String, detail: String)? {
        switch self {
        case .running: nil
        case .idle: ("Camera paused", "Show the camera from the menu bar.")
        case .requestingAccess: ("Allow camera access", "Choose Allow in the macOS prompt.")
        case .starting: ("Starting camera…", "Your camera will appear here.")
        case .noCamera: ("No camera connected", "Connect a camera to get started.")
        case .permissionDenied: ("Camera access needed", "Open Camera Settings from the menu bar.")
        case .failed(let detail): ("Camera unavailable", detail)
        }
    }
}

struct DetectedFace: Sendable, Equatable {
    let center: CGPoint
    let size: CGSize
    let isNativeCenterStageCamera: Bool
}

// All session mutations run on this serial queue. The preview layer may read
// the session on the main thread, as required by AVFoundation's preview API.
private final class CaptureDriver: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "app.youcam.capture", qos: .utility)
    private let videoOutput = AVCaptureVideoDataOutput()
    private let faceRequest = VNDetectFaceRectanglesRequest()
    private let sequenceHandler = VNSequenceRequestHandler()
    private var centerStageEnabled = false
    private var isNativeCenterStageCamera = false
    private var currentShape: CameraShape = .circle
    private var lastDetectionTime: CFAbsoluteTime = 0
    private var lastCenter: CGPoint?
    private var stableFramesCount = 0

    var onFaceUpdate: (@MainActor @Sendable (DetectedFace?) -> Void)?

    override init() {
        super.init()
        // Use native YUV 420v planar format for zero-conversion overhead from the camera ISP
        videoOutput.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: Int(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        ]
        videoOutput.alwaysDiscardsLateVideoFrames = true
        videoOutput.setSampleBufferDelegate(self, queue: queue)
    }

    func updateConfiguration(centerStage: Bool, shape: CameraShape) {
        queue.async { [self] in
            centerStageEnabled = centerStage
            currentShape = shape
            syncVideoOutputAttachment()
            if !shouldRunSoftwareTracking {
                lastCenter = nil
                stableFramesCount = 0
                let callback = onFaceUpdate
                Task { @MainActor in callback?(nil) }
            }
        }
    }

    private var shouldRunSoftwareTracking: Bool {
        return centerStageEnabled
    }

    private func syncVideoOutputAttachment() {
        guard session.isRunning else { return }
        let needsOutput = shouldRunSoftwareTracking
        let hasOutput = session.outputs.contains(videoOutput)
        if needsOutput && !hasOutput {
            session.beginConfiguration()
            if session.canAddOutput(videoOutput) { session.addOutput(videoOutput) }
            session.commitConfiguration()
        } else if !needsOutput && hasOutput {
            session.beginConfiguration()
            session.removeOutput(videoOutput)
            session.commitConfiguration()
        }
    }

    func start(deviceID: String, centerStage: Bool, shape: CameraShape, completion: @escaping @MainActor @Sendable (String?) -> Void) {
        queue.async { [self] in
            centerStageEnabled = centerStage
            currentShape = shape
            guard let device = AVCaptureDevice(uniqueID: deviceID) else {
                Task { @MainActor in completion("The camera was disconnected.") }
                return
            }

            isNativeCenterStageCamera = (device.deviceType == .continuityCamera) || device.activeFormat.isCenterStageSupported

            do {
                let input = try AVCaptureDeviceInput(device: device)
                session.beginConfiguration()
                session.inputs.forEach { session.removeInput($0) }
                session.outputs.forEach { session.removeOutput($0) }
                if session.canSetSessionPreset(.high) {
                    session.sessionPreset = .high
                }
                guard session.canAddInput(input) else {
                    session.commitConfiguration()
                    session.stopRunning()
                    Task { @MainActor in completion("This camera could not be opened. Try another camera.") }
                    return
                }
                session.addInput(input)
                if session.canSetSessionPreset(.hd1280x720) {
                    session.sessionPreset = .hd1280x720
                }
                if shouldRunSoftwareTracking, session.canAddOutput(videoOutput) {
                    session.addOutput(videoOutput)
                }
                session.commitConfiguration()

                // Lock frame rate to 30fps maximum to prevent 60fps Continuity Camera thermal drain
                if let range = device.activeFormat.videoSupportedFrameRateRanges.first {
                    let targetFPS = min(30.0, range.maxFrameRate)
                    if targetFPS >= range.minFrameRate {
                        try? device.lockForConfiguration()
                        device.activeVideoMinFrameDuration = CMTime(value: 1, timescale: CMTimeScale(targetFPS))
                        device.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: CMTimeScale(targetFPS))
                        device.unlockForConfiguration()
                    }
                }

                if !shouldRunSoftwareTracking {
                    let callback = onFaceUpdate
                    Task { @MainActor in callback?(nil) }
                }
                if !session.isRunning { session.startRunning() }
                let error = session.isRunning ? nil : "The camera could not start. Choose Retry Camera from the menu."
                Task { @MainActor in completion(error) }
            } catch {
                session.stopRunning()
                let message = error.localizedDescription
                Task { @MainActor in completion(message) }
            }
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard shouldRunSoftwareTracking else { return }
        let now = CFAbsoluteTimeGetCurrent()
        // Adaptive thermal throttling:
        // - Poll at 5 Hz (every 0.20s) while moving
        // - Drop to 2 Hz (every 0.50s) once head has been stationary for ~1 second
        let interval = stableFramesCount >= 5 ? 0.50 : 0.20
        guard now - lastDetectionTime >= interval else { return }
        lastDetectionTime = now

        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        try? sequenceHandler.perform([faceRequest], on: pixelBuffer, orientation: .up)

        let callback = onFaceUpdate
        if let face = faceRequest.results?.max(by: { $0.boundingBox.width * $0.boundingBox.height < $1.boundingBox.width * $1.boundingBox.height }) {
            let box = face.boundingBox
            let center = CGPoint(x: box.midX, y: box.midY)
            if let prev = lastCenter, hypot(center.x - prev.x, center.y - prev.y) < 0.015 {
                stableFramesCount = min(20, stableFramesCount + 1)
            } else {
                stableFramesCount = 0
                lastCenter = center
            }
            let detected = DetectedFace(
                center: center,
                size: box.size,
                isNativeCenterStageCamera: isNativeCenterStageCamera
            )
            Task { @MainActor in callback?(detected) }
        }
    }

    func stop() {
        queue.async { [self] in
            if session.isRunning { session.stopRunning() }
        }
    }

    func stopAndWait() {
        queue.sync {
            if session.isRunning { session.stopRunning() }
        }
    }
}

@MainActor
final class CameraController {
    private let driver = CaptureDriver()
    private let defaults: UserDefaults
    private let discovery = AVCaptureDevice.DiscoverySession(
        deviceTypes: [.builtInWideAngleCamera, .external, .continuityCamera],
        mediaType: .video,
        position: .unspecified
    )
    private var observers: [NSObjectProtocol] = []
    private var generation = 0
    private var wantsCamera = false
    private var requestingAccess = false
    private var configuredDeviceID: String?
    private var currentShape: CameraShape

    var onChange: (() -> Void)?
    var onFaceUpdate: ((DetectedFace?) -> Void)?
    private(set) var devices: [AVCaptureDevice] = []
    private(set) var selectedDeviceID: String?
    private(set) var centerStageEnabled: Bool
    private(set) var state: CameraState = .idle {
        didSet { onChange?() }
    }
    var session: AVCaptureSession { driver.session }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.centerStageEnabled = defaults.object(forKey: "centerStage") as? Bool ?? true
        self.currentShape = CameraShape(rawValue: defaults.string(forKey: "shape") ?? "") ?? .circle
        driver.onFaceUpdate = { [weak self] face in
            self?.onFaceUpdate?(face)
        }
        let center = NotificationCenter.default
        for name in [AVCaptureDevice.wasConnectedNotification, AVCaptureDevice.wasDisconnectedNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.refreshDevices() }
            })
        }
        observers.append(center.addObserver(forName: AVCaptureSession.runtimeErrorNotification,
                                            object: driver.session, queue: .main) { [weak self] notification in
            let message = (notification.userInfo?[AVCaptureSessionErrorKey] as? NSError)?.localizedDescription
                ?? "Choose Retry Camera from the menu."
            Task { @MainActor in
                guard let self, self.wantsCamera else { return }
                self.configuredDeviceID = nil
                self.state = .failed(message)
            }
        })
        observers.append(center.addObserver(forName: AVCaptureSession.wasInterruptedNotification,
                                            object: driver.session, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.wantsCamera else { return }
                self.state = .failed("The camera was interrupted. Close other camera apps or choose Retry Camera.")
            }
        })
        observers.append(center.addObserver(forName: AVCaptureSession.interruptionEndedNotification,
                                            object: driver.session, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.retry() }
        })
    }

    func setCenterStage(_ enabled: Bool) {
        centerStageEnabled = enabled
        defaults.set(enabled, forKey: "centerStage")
        driver.updateConfiguration(centerStage: enabled, shape: currentShape)
        if !enabled { onFaceUpdate?(nil) }
        onChange?()
    }

    func setShape(_ shape: CameraShape) {
        currentShape = shape
        driver.updateConfiguration(centerStage: centerStageEnabled, shape: shape)
    }

    func show() {
        wantsCamera = true
        checkAccess()
    }

    func hide() {
        wantsCamera = false
        generation += 1
        configuredDeviceID = nil
        driver.stop()
        state = .idle
    }

    func retry() {
        guard wantsCamera else { return }
        configuredDeviceID = nil
        checkAccess()
    }

    func checkAccess() {
        guard wantsCamera else { return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            refreshDevices()
        case .notDetermined:
            guard !requestingAccess else { return }
            requestingAccess = true
            state = .requestingAccess
            AVCaptureDevice.requestAccess(for: .video) { [weak self] _ in
                Task { @MainActor in
                    self?.requestingAccess = false
                    self?.checkAccess()
                }
            }
        case .denied, .restricted:
            generation += 1
            configuredDeviceID = nil
            driver.stop()
            state = .permissionDenied
        @unknown default:
            state = .failed("Camera access is unavailable.")
        }
    }

    func selectCamera(id: String) {
        defaults.set(id, forKey: "cameraID")
        selectedDeviceID = id
        configuredDeviceID = nil
        if wantsCamera { startSelectedCamera() }
        onChange?()
    }

    func refreshDevices() {
        devices = discovery.devices.sorted { $0.localizedName.localizedStandardCompare($1.localizedName) == .orderedAscending }
        let preferredID = defaults.string(forKey: "cameraID")
        let defaultID = AVCaptureDevice.default(for: .video)?.uniqueID
        selectedDeviceID = devices.first(where: { $0.uniqueID == preferredID })?.uniqueID
            ?? devices.first(where: { $0.uniqueID == selectedDeviceID })?.uniqueID
            ?? devices.first(where: { $0.uniqueID == defaultID })?.uniqueID
            ?? devices.first?.uniqueID

        if wantsCamera, AVCaptureDevice.authorizationStatus(for: .video) == .authorized {
            startSelectedCamera()
        }
        onChange?()
    }

    private func startSelectedCamera() {
        guard let id = selectedDeviceID,
              let device = devices.first(where: { $0.uniqueID == id }) else {
            generation += 1
            configuredDeviceID = nil
            driver.stop()
            state = .noCamera
            return
        }
        guard configuredDeviceID != id else { return }
        generation += 1
        let request = generation
        let name = device.localizedName
        configuredDeviceID = id
        state = .starting(name)
        driver.start(deviceID: id, centerStage: centerStageEnabled, shape: currentShape) { [weak self] error in
            guard let self, self.wantsCamera, self.generation == request else { return }
            if let error {
                self.configuredDeviceID = nil
                self.state = .failed(error)
            } else {
                self.state = .running(name)
            }
        }
    }

    func shutdown() {
        observers.forEach { NotificationCenter.default.removeObserver($0) }
        observers.removeAll()
        generation += 1
        wantsCamera = false
        driver.stopAndWait()
    }
}
