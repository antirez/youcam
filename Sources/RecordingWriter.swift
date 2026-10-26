import AVFoundation
import ScreenCaptureKit
import CoreImage

final class RecordingWriter: NSObject, SCStreamOutput, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let queue = DispatchQueue(label: "app.youcam.movie", qos: .userInitiated)
    private var writer: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?
    private var adaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var timeline = RecordingTimeline()
    private var lastVideoTime = -1.0
    private var lastAudioTime = -1.0
    private var lastImage: CVPixelBuffer?
    private var lastScreen: CVPixelBuffer?
    private var cameraImage: CVPixelBuffer?
    private var cameraOverlay: RecordingCameraOverlay?
    private let imageContext = CIContext(options: [.cacheIntermediates: false])
    private var failed = false
    private var accepting = false
    var onFailure: (@MainActor @Sendable (String) -> Void)?

    func begin(url: URL, size: CGSize, audio: Bool) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [self] in
                do {
                    let movie = try AVAssetWriter(outputURL: url, fileType: .mov)
                    let video = AVAssetWriterInput(mediaType: .video, outputSettings: [
                        AVVideoCodecKey: AVVideoCodecType.h264,
                        AVVideoWidthKey: Int(size.width), AVVideoHeightKey: Int(size.height),
                        AVVideoCompressionPropertiesKey: [
                            AVVideoAverageBitRateKey: min(40_000_000, max(4_000_000, Int(size.width * size.height * 5))),
                            AVVideoExpectedSourceFrameRateKey: 30,
                            AVVideoMaxKeyFrameIntervalKey: 60
                        ]
                    ])
                    video.expectsMediaDataInRealTime = true
                    guard movie.canAdd(video) else { throw RecordingFailure("This recording size is not supported.") }
                    movie.add(video)
                    let pixelAdaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: video,
                        sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                                                      kCVPixelBufferWidthKey as String: Int(size.width),
                                                      kCVPixelBufferHeightKey as String: Int(size.height),
                                                      kCVPixelBufferIOSurfacePropertiesKey as String: [:]])
                    var microphone: AVAssetWriterInput?
                    if audio {
                        let input = AVAssetWriterInput(mediaType: .audio, outputSettings: [
                            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48000,
                            AVNumberOfChannelsKey: 1, AVEncoderBitRateKey: 96000
                        ])
                        input.expectsMediaDataInRealTime = true
                        guard movie.canAdd(input) else { throw RecordingFailure("The microphone track could not be created.") }
                        movie.add(input)
                        microphone = input
                    }
                    guard movie.startWriting() else { throw movie.error ?? RecordingFailure("The movie could not be created.") }
                    movie.startSession(atSourceTime: .zero)
                    writer = movie
                    videoInput = video
                    audioInput = microphone
                    adaptor = pixelAdaptor
                    timeline = RecordingTimeline()
                    lastVideoTime = -1
                    lastAudioTime = -1
                    lastImage = nil
                    lastScreen = nil
                    cameraImage = nil
                    failed = false
                    accepting = true
                    continuation.resume()
                } catch { continuation.resume(throwing: error) }
            }
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sampleBuffer.isValid,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = attachments.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete else { return }
        consumeVideo(sampleBuffer)
    }

    // Called on queue by ScreenCaptureKit, also used by synthetic media tests.
    func consumeVideo(_ sample: CMSampleBuffer) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard accepting, let image = sample.imageBuffer else { return }
        let host = sample.presentationTimeStamp.seconds
        guard host.isFinite else { return }
        if timeline.origin == nil { timeline.origin = host }
        guard let time = timeline.presentationTime(for: host) else { return }
        lastScreen = image
        composeAndAppend(at: time)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        // Stamp arrival on the same host clock as screen/audio. Camera devices
        // can use a different capture clock for their sample timestamps.
        consumeCamera(sampleBuffer, at: hostSeconds())
    }

    // Camera frames also drive the movie while the desktop is static.
    func consumeCamera(_ sample: CMSampleBuffer, at hostTime: Double) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard accepting, cameraOverlay != nil, hostTime.isFinite,
              let time = timeline.presentationTime(for: hostTime), let image = sample.imageBuffer else { return }
        cameraImage = image
        guard time - lastVideoTime >= 1.0 / 60 else { return }
        composeAndAppend(at: time)
    }

    func setCameraOverlay(_ overlay: RecordingCameraOverlay?, at hostTime: Double = hostSeconds()) {
        queue.async { [self] in
            guard cameraOverlay != overlay else { return }
            cameraOverlay = overlay
            if overlay == nil { cameraImage = nil }
            guard accepting, hostTime.isFinite, let time = timeline.presentationTime(for: hostTime) else { return }
            composeAndAppend(at: time)
        }
    }

    private func composeAndAppend(at time: Double) {
        guard let screen = lastScreen, time > lastVideoTime, videoInput?.isReadyForMoreMediaData == true else { return }
        var image = screen
        if let overlay = cameraOverlay, let camera = cameraImage {
            guard let pool = adaptor?.pixelBufferPool else { return }
            var output: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &output) == kCVReturnSuccess,
                  let output else {
                reportFailure("The camera frame could not be composed. Check available memory.")
                return
            }
            let composite = RecordingCameraCompositor.composite(screen: CIImage(cvPixelBuffer: screen),
                camera: CIImage(cvPixelBuffer: camera), overlay: overlay)
            imageContext.render(composite, to: output, bounds: composite.extent,
                                colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
            image = output
        }
        lastImage = image
        appendImage(image, at: time)
    }

    private func appendImage(_ image: CVPixelBuffer, at seconds: Double) {
        guard seconds > lastVideoTime, videoInput?.isReadyForMoreMediaData == true else { return }
        if adaptor?.append(image, withPresentationTime: CMTime(seconds: seconds, preferredTimescale: 60_000)) == true {
            lastVideoTime = seconds
        } else { reportFailure() }
    }

    func appendAudio(_ sample: CMSampleBuffer) {
        let box = SampleBufferBox(sample)
        queue.async { [self] in
            guard accepting, let input = audioInput, input.isReadyForMoreMediaData,
                  let time = timeline.presentationTime(for: box.sample.presentationTimeStamp.seconds), time > lastAudioTime else { return }
            var timing = CMSampleTimingInfo()
            guard CMSampleBufferGetSampleTimingInfo(box.sample, at: 0, timingInfoOut: &timing) == noErr else { return }
            timing.presentationTimeStamp = CMTime(seconds: time, preferredTimescale: 48_000)
            timing.decodeTimeStamp = .invalid
            var retimed: CMSampleBuffer?
            guard CMSampleBufferCreateCopyWithNewTiming(allocator: kCFAllocatorDefault, sampleBuffer: box.sample,
                      sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleBufferOut: &retimed) == noErr,
                  let retimed else { return }
            if input.append(retimed) { lastAudioTime = time }
            else { reportFailure() }
        }
    }

    func pause(at time: Double) {
        queue.async { [self] in
            if let image = lastImage { appendImage(image, at: timeline.duration(at: time)) }
            timeline.pause(at: time)
        }
    }

    func resume(at time: Double) {
        queue.async { [self] in timeline.resume(at: time) }
    }

    func elapsed() async -> Double {
        await withCheckedContinuation { continuation in
            queue.async { [self] in continuation.resume(returning: timeline.duration(at: hostSeconds())) }
        }
    }

    func finish(at time: Double) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [self] in
                accepting = false
                guard let movie = writer else { continuation.resume(); return }
                guard movie.status == .writing else {
                    let error = movie.error ?? RecordingFailure("The movie writer stopped before the recording could be saved.")
                    reset()
                    continuation.resume(throwing: error)
                    return
                }
                guard timeline.origin != nil, lastVideoTime >= 0 else {
                    movie.cancelWriting()
                    reset()
                    continuation.resume(throwing: RecordingFailure("No screen frames were received. Check Screen Recording permission and try again."))
                    return
                }
                // ScreenCaptureKit can stop sending complete frames on a static
                // desktop. Extend the last frame to the actual Stop/Pause time.
                let end = max(lastVideoTime + 1.0 / 30, timeline.duration(at: time))
                if let image = lastImage { appendImage(image, at: end) }
                movie.endSession(atSourceTime: CMTime(seconds: end, preferredTimescale: 60_000))
                videoInput?.markAsFinished()
                audioInput?.markAsFinished()
                if movie.status == .failed {
                    let error = movie.error ?? RecordingFailure("The recording could not be saved.")
                    reset()
                    continuation.resume(throwing: error)
                    return
                }
                movie.finishWriting { [self] in
                    queue.async { [self] in
                        guard let movie = writer else {
                            continuation.resume(throwing: RecordingFailure("The recording writer was closed unexpectedly."))
                            return
                        }
                        let error = movie.status == .completed ? nil : movie.error ?? RecordingFailure("The recording could not be saved.")
                        reset()
                        if let error { continuation.resume(throwing: error) }
                        else { continuation.resume() }
                    }
                }
            }
        }
    }

    private func reportFailure() {
        guard !failed, writer?.status == .failed else { return }
        reportFailure(writer?.error?.localizedDescription ?? "The recording could not be written. Check available disk space.")
    }

    private func reportFailure(_ message: String) {
        guard !failed else { return }
        failed = true
        if let onFailure { Task { @MainActor in onFailure(message) } }
    }

    private func reset() {
        writer = nil
        videoInput = nil
        audioInput = nil
        adaptor = nil
        lastImage = nil
        lastScreen = nil
        cameraImage = nil
        accepting = false
    }
}
