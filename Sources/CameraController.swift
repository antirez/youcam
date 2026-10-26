import AVFoundation
import Foundation

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

// All session mutations run on this serial queue. The preview layer may read
// the session on the main thread, as required by AVFoundation's preview API.
private final class CaptureDriver: @unchecked Sendable {
    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "app.youcam.capture", qos: .userInitiated)
    private let output = AVCaptureVideoDataOutput()
    private let movie: RecordingWriter

    init(movie: RecordingWriter) { self.movie = movie }

    func start(deviceID: String, completion: @escaping @MainActor @Sendable (String?) -> Void) {
        queue.async { [self] in
            guard let device = AVCaptureDevice(uniqueID: deviceID) else {
                Task { @MainActor in completion("The camera was disconnected.") }
                return
            }

            do {
                let input = try AVCaptureDeviceInput(device: device)
                session.beginConfiguration()
                session.inputs.forEach { session.removeInput($0) }
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
                if session.outputs.isEmpty {
                    guard session.canAddOutput(output) else {
                        session.commitConfiguration()
                        session.stopRunning()
                        Task { @MainActor in completion("This camera cannot supply recording frames. Try another camera.") }
                        return
                    }
                    session.addOutput(output)
                    output.alwaysDiscardsLateVideoFrames = true
                    output.setSampleBufferDelegate(movie, queue: movie.queue)
                }
                let formats = output.availableVideoPixelFormatTypes
                let format = [kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                              kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              kCVPixelFormatType_32BGRA].first(where: { formats.contains($0) })
                if let format { output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: format] }
                if let connection = output.connection(with: .video), connection.isVideoMirroringSupported {
                    connection.automaticallyAdjustsVideoMirroring = false
                    connection.isVideoMirrored = false
                }
                session.commitConfiguration()
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
    private let driver: CaptureDriver
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

    var onChange: (() -> Void)?
    private(set) var devices: [AVCaptureDevice] = []
    private(set) var selectedDeviceID: String?
    private(set) var state: CameraState = .idle {
        didSet { onChange?() }
    }
    var session: AVCaptureSession { driver.session }

    init(defaults: UserDefaults = .standard, movie: RecordingWriter) {
        driver = CaptureDriver(movie: movie)
        self.defaults = defaults
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
        driver.start(deviceID: id) { [weak self] error in
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
