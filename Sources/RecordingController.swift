import AppKit
import ScreenCaptureKit

private final class StreamEvents: NSObject, SCStreamDelegate, @unchecked Sendable {
    let didFail: @MainActor @Sendable (String) -> Void
    init(didFail: @escaping @MainActor @Sendable (String) -> Void) { self.didFail = didFail }
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        let message = error.localizedDescription
        Task { @MainActor in didFail(message) }
    }
}

enum RecordingPhase: Sendable {
    case idle, starting, recording, paused, finishing
    var busy: Bool { self != .idle }
}

@MainActor
final class RecordingController {
    let movie: RecordingWriter
    let audio: AudioController
    private var stream: SCStream?
    private var events: StreamEvents?
    private var cancelStart = false
    private var stopWaiters: [CheckedContinuation<Void, Never>] = []
    private var timer: Timer?
    private var recordingURL: URL?
    private(set) var target: RecordingTarget?
    private(set) var phase = RecordingPhase.idle
    private(set) var elapsed = 0.0
    private(set) var lastURL: URL?
    private(set) var lastError: String?
    var onChange: (() -> Void)?
    var onError: ((String) -> Void)?

    init(movie: RecordingWriter, audio: AudioController) {
        self.movie = movie
        self.audio = audio
        movie.onFailure = { [weak self] message in self?.failed(message) }
        audio.onFailure = { [weak self] message in self?.failed(message) }
        audio.onDeviceLost = { [weak self] in
            guard let self, self.phase.busy else { return }
            self.failed("The microphone was disconnected. The recording has been stopped to preserve your movie.")
        }
    }

    var timeLabel: String {
        let seconds = max(0, Int(elapsed))
        return String(format: "%02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60)
    }

    var status: String {
        switch phase {
        case .idle: lastError == nil ? (lastURL == nil ? "Ready to record" : "Saved to Desktop") : "Recording stopped"
        case .starting: "Starting recording…"
        case .recording: "Recording · \(timeLabel)"
        case .paused: "Paused · \(timeLabel)"
        case .finishing: "Saving recording…"
        }
    }

    func start(target: RecordingTarget) async {
        guard phase == .idle else { return }
        phase = .starting
        self.target = target
        elapsed = 0
        cancelStart = false
        lastError = nil
        onChange?()
        var createdURL: URL?
        do {
            guard CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess() else {
                throw RecordingFailure("Allow YouCam in System Settings → Privacy & Security → Screen Recording, then try again. macOS may ask you to reopen YouCam.")
            }
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            guard let display = content.displays.first(where: { $0.displayID == target.displayID }) else {
                throw RecordingFailure("The selected display is no longer connected. Select another recording area.")
            }
            let filter = try makeFilter(content: content, display: display)
            try checkCancellation()
            try await audio.acquire("recording")
            try checkCancellation()
            let desktop = try FileManager.default.url(for: .desktopDirectory, in: .userDomainMask,
                                                      appropriateFor: nil, create: true)
            let url = RecordingFiles.nextURL(in: desktop)
            let size = target.pixelSize
            try await movie.begin(url: url, size: size, audio: audio.hasAudio)
            createdURL = url
            recordingURL = url
            let configuration = SCStreamConfiguration()
            configuration.width = Int(size.width)
            configuration.height = Int(size.height)
            configuration.sourceRect = target.rect
            configuration.minimumFrameInterval = CMTime(value: 1, timescale: 30)
            configuration.queueDepth = 4
            configuration.pixelFormat = kCVPixelFormatType_32BGRA
            configuration.colorSpaceName = CGColorSpace.sRGB
            configuration.showsCursor = true
            configuration.capturesAudio = false
            configuration.ignoreShadowsDisplay = true
            let events = StreamEvents { [weak self] message in self?.failed(message) }
            self.events = events
            let capture = SCStream(filter: filter, configuration: configuration, delegate: events)
            try capture.addStreamOutput(movie, type: .screen, sampleHandlerQueue: movie.queue)
            stream = capture
            try checkCancellation()
            try await capture.startCapture()
            try checkCancellation()
            phase = .recording
            let timer = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in
                Task { @MainActor in
                    guard let self, self.phase == .recording || self.phase == .paused else { return }
                    self.elapsed = await self.movie.elapsed()
                    self.onChange?()
                }
            }
            RunLoop.main.add(timer, forMode: .common)
            self.timer = timer
            onChange?()
        } catch {
            if let stream { try? await stream.stopCapture() }
            if createdURL != nil {
                try? await movie.finish(at: hostSeconds())
                // A cancelled/failed start is not a successful recording.
                if let createdURL { try? FileManager.default.removeItem(at: createdURL) }
            }
            self.stream = nil
            events = nil
            recordingURL = nil
            audio.release("recording")
            if !(error is CancellationError) { lastError = error.localizedDescription }
            completeStop()
            if !(error is CancellationError) {
                onError?(error.localizedDescription)
            }
        }
    }

    private func checkCancellation() throws {
        if cancelStart {
            if let lastError { throw RecordingFailure(lastError) }
            throw CancellationError()
        }
    }

    private func makeFilter(content: SCShareableContent, display: SCDisplay) throws -> SCContentFilter {
        guard let application = content.applications.first(where: { $0.processID == ProcessInfo.processInfo.processIdentifier }) else {
            throw RecordingFailure("YouCam could not identify its control windows. Show the camera or recording panel and try again.")
        }
        // The camera is composited from its original frames by RecordingWriter.
        // Excluding its preview prevents preview transparency entering the movie.
        return SCContentFilter(display: display, excludingApplications: [application], exceptingWindows: [])
    }

    func togglePause() {
        switch phase {
        case .recording:
            movie.pause(at: hostSeconds())
            phase = .paused
        case .paused:
            movie.resume(at: hostSeconds())
            phase = .recording
        default: return
        }
        onChange?()
    }

    func stop() async {
        if phase == .starting {
            cancelStart = true
            await withCheckedContinuation { stopWaiters.append($0) }
            return
        }
        if phase == .finishing {
            await withCheckedContinuation { stopWaiters.append($0) }
            return
        }
        guard phase == .recording || phase == .paused else { return }
        let stoppedAt = hostSeconds()
        movie.pause(at: stoppedAt)
        phase = .finishing
        timer?.invalidate()
        timer = nil
        onChange?()
        if let stream { try? await stream.stopCapture() }
        self.stream = nil
        events = nil
        do {
            try await movie.finish(at: stoppedAt)
            lastURL = recordingURL
        } catch {
            lastError = error.localizedDescription
        }
        recordingURL = nil
        audio.release("recording")
        completeStop()
        if let lastError { onError?(lastError) }
    }

    private func failed(_ message: String) {
        if phase == .starting {
            lastError = message
            cancelStart = true
            return
        }
        guard phase == .recording || phase == .paused else { return }
        lastError = message
        Task { await stop() }
    }

    private func completeStop() {
        phase = .idle
        onChange?()
        let waiters = stopWaiters
        stopWaiters.removeAll()
        waiters.forEach { $0.resume() }
    }
}
