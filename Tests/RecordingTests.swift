import AppKit
import AVFoundation

@MainActor
func runRecordingTests() async throws {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        guard value() else { fatalError("FAIL: \(message)") }
    }
    func near(_ value: Double, _ expected: Double, tolerance: Double = 0.001) -> Bool { abs(value - expected) < tolerance }

    var timeline = RecordingTimeline(origin: 100)
    expect(timeline.presentationTime(for: 99) == nil, "Audio arriving before the first video is discarded")
    timeline.pause(at: 101)
    expect(timeline.presentationTime(for: 101.5) == nil, "Paused samples are discarded")
    expect(near(timeline.duration(at: 110), 1), "The clock freezes while paused")
    timeline.resume(at: 106)
    expect(near(timeline.presentationTime(for: 106.5)!, 1.5), "Resume removes the pause from the timeline")
    expect(timeline.presentationTime(for: 102) == nil, "Late-arriving samples from a pause are discarded")
    timeline.pause(at: 108)
    timeline.resume(at: 110)
    expect(near(timeline.presentationTime(for: 111)!, 4), "Multiple pauses accumulate without gaps")
    var beforeFirstFrame = RecordingTimeline()
    beforeFirstFrame.pause(at: 90)
    beforeFirstFrame.resume(at: 95)
    beforeFirstFrame.origin = 100
    expect(near(beforeFirstFrame.presentationTime(for: 101)!, 1), "Pauses before the first frame cannot shift timestamps")

    let screen = CGRect(x: -1920, y: -300, width: 1920, height: 1080)
    let selected = CGRect(x: -1800, y: -200, width: 640, height: 360)
    expect(RecordingTarget.localRect(selected, screen: screen) == CGRect(x: 120, y: 620, width: 640, height: 360),
           "Region coordinates handle screens with negative origins and flipped vertical coordinates")
    let retina = RecordingTarget(displayID: 1, rect: CGRect(x: 0, y: 0, width: 501, height: 301), scale: 2, name: "Test")
    expect(retina.pixelSize == CGSize(width: 1002, height: 602), "Retina regions preserve pixel density")
    let huge = RecordingTarget(displayID: 1, rect: CGRect(x: 0, y: 0, width: 5000, height: 3000), scale: 2, name: "Test")
    expect(huge.pixelSize.width == 4096 && Int(huge.pixelSize.height) % 2 == 0, "Large displays produce encoder-compatible dimensions")

    var quiet: [Float] = [0, 0.1, -0.25, 0.5]
    let quietLevel = AudioGain.apply(&quiet, gain: 0.5)
    expect(quiet == [0, 0.05, -0.125, 0.25], "The input slider changes actual recorded samples")
    expect(!quietLevel.clipped && near(Double(quietLevel.peak), 0.25), "The meter reports the gain-adjusted signal")
    var loud: [Float] = [-0.8, 0.8]
    let loudLevel = AudioGain.apply(&loud, gain: 2)
    expect(loud == [-1, 1] && loudLevel.clipped, "Gain cannot wrap or overflow audio samples")
    var alreadyClipped: [Float] = [1]
    expect(AudioGain.apply(&alreadyClipped, gain: 0.1).clipped, "The meter warns about source clipping even after reducing gain")
    var invalid: [Float] = [.nan, .infinity]
    _ = AudioGain.apply(&invalid, gain: 1)
    expect(invalid == [0, 0], "Invalid input samples do not corrupt the audio encoder")

    let directory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent(".build/recording-tests")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let date = Date(timeIntervalSince1970: 1_790_066_001)
    let first = RecordingFiles.nextURL(in: directory, date: date)
    try Data("existing movie".utf8).write(to: first)
    let second = RecordingFiles.nextURL(in: directory, date: date)
    expect(first != second && second.pathExtension == "mov", "Automatic file naming never overwrites an existing recording")
    expect(!second.lastPathComponent.contains(":"), "Recording filenames are safe in Finder")
    try FileManager.default.removeItem(at: first)

    func frame(at time: Double, red: UInt8, green: UInt8, blue: UInt8) -> SampleBufferBox {
        var pixel: CVPixelBuffer?
        precondition(CVPixelBufferCreate(kCFAllocatorDefault, 320, 180, kCVPixelFormatType_32BGRA,
                        [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &pixel) == kCVReturnSuccess)
        let image = pixel!
        CVPixelBufferLockBaseAddress(image, [])
        let bytes = CVPixelBufferGetBaseAddress(image)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(image)
        for y in 0..<180 {
            for x in 0..<320 {
                let offset = y * stride + x * 4
                bytes[offset] = blue; bytes[offset + 1] = green; bytes[offset + 2] = red; bytes[offset + 3] = 255
            }
        }
        CVPixelBufferUnlockBaseAddress(image, [])
        var description: CMVideoFormatDescription?
        CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: image, formatDescriptionOut: &description)
        var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: 30),
                                       presentationTimeStamp: CMTime(seconds: time, preferredTimescale: 60000), decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        CMSampleBufferCreateReadyWithImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: image,
            formatDescription: description!, sampleTiming: &timing, sampleBufferOut: &sample)
        return SampleBufferBox(sample!)
    }
    func append(_ buffer: SampleBufferBox, to movie: RecordingWriter) async {
        await withCheckedContinuation { continuation in
            movie.queue.async { movie.consumeVideo(buffer.sample); continuation.resume() }
        }
    }

    let url = directory.appendingPathComponent("pause-resume.mov")
    try? FileManager.default.removeItem(at: url)
    let movie = RecordingWriter()
    try await movie.begin(url: url, size: CGSize(width: 320, height: 180), audio: true)
    for index in 0..<90 {
        let time = 100 + Double(index) / 30
        if index == 30 { movie.pause(at: 101) }
        if index == 60 { movie.resume(at: 102) }
        let inPause = index >= 30 && index < 60
        await append(frame(at: time, red: inPause ? 255 : 0, green: index < 30 ? 200 : 0, blue: index >= 60 ? 200 : 0), to: movie)
        let phaseStep = 2.0 * Double.pi * 440.0 / 48000.0
        let samples = (0..<1600).map { offset -> Float in
            Float(sin(Double(offset + index * 1600) * phaseStep) * 0.2)
        }
        movie.appendAudio(PCMBuffer.make(samples: samples, at: CMTime(seconds: time, preferredTimescale: 48000))!)
        try await Task.sleep(for: .milliseconds(5))
    }
    try await movie.finish(at: 103)
    let asset = AVURLAsset(url: url)
    let duration = try await asset.load(.duration).seconds
    expect(near(duration, 2, tolerance: 0.05), "MOV contains two seconds of content with the one-second pause removed")
    let videos = try await asset.loadTracks(withMediaType: .video)
    let audios = try await asset.loadTracks(withMediaType: .audio)
    expect(videos.count == 1 && audios.count == 1, "MOV has playable video and microphone tracks")
    let audioDuration = try await audios[0].load(.timeRange).duration.seconds
    expect(audioDuration > 1.8 && audioDuration < 2.1, "Audio stays aligned across pause and resume")
    let reader = try AVAssetReader(asset: asset)
    let output = AVAssetReaderTrackOutput(track: videos[0], outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
    reader.add(output)
    expect(reader.startReading(), "The saved movie decodes successfully")
    var decoded = 0
    var lastPTS = -1.0
    while let sample = output.copyNextSampleBuffer() {
        let pts = sample.presentationTimeStamp.seconds
        expect(pts > lastPTS, "Encoded video timestamps remain strictly increasing")
        lastPTS = pts
        let image = sample.imageBuffer!
        CVPixelBufferLockBaseAddress(image, .readOnly)
        let bytes = CVPixelBufferGetBaseAddress(image)!.assumingMemoryBound(to: UInt8.self)
        let offset = 90 * CVPixelBufferGetBytesPerRow(image) + 160 * 4
        expect(bytes[offset + 2] < 100, "No red frame from the paused interval is encoded")
        CVPixelBufferUnlockBaseAddress(image, .readOnly)
        decoded += 1
    }
    expect(decoded >= 20, "The encoder produces a useful sequence of frames")
    expect(reader.status == .completed, "The movie is fully readable")

    let staticURL = directory.appendingPathComponent("static-screen.mov")
    try? FileManager.default.removeItem(at: staticURL)
    try await movie.begin(url: staticURL, size: CGSize(width: 320, height: 180), audio: false)
    await append(frame(at: 200, red: 0, green: 200, blue: 0), to: movie)
    try await Task.sleep(for: .milliseconds(50))
    try await movie.finish(at: 202.5)
    let staticDuration = try await AVURLAsset(url: staticURL).load(.duration).seconds
    expect(near(staticDuration, 2.5, tolerance: 0.05), "A static screen is extended to the real stop time")
    let noAudio = try await AVURLAsset(url: staticURL).loadTracks(withMediaType: .audio)
    expect(noAudio.isEmpty, "No Audio creates a movie without an audio track")

    // Record synthetic desktop/camera sources: no camera or screen permission
    // is needed to prove the saved movie contains an opaque, moving camera.
    let cameraURL = directory.appendingPathComponent("opaque-camera.mov")
    try? FileManager.default.removeItem(at: cameraURL)
    let cameraTarget = RecordingTarget(displayID: 1, rect: CGRect(x: 0, y: 0, width: 320, height: 180), scale: 1, name: "Camera test")
    let overlay = RecordingCameraOverlay(windowFrame: CGRect(x: 100, y: 30, width: 120, height: 120),
        screenFrame: cameraTarget.rect, target: cameraTarget, shape: .circle, mirrored: false)
    movie.setCameraOverlay(overlay)
    try await movie.begin(url: cameraURL, size: cameraTarget.pixelSize, audio: false)
    await append(frame(at: 400, red: 0, green: 200, blue: 0), to: movie)
    func cameraFrame(at time: Double, blue: Bool = false) async {
        let sample = frame(at: time, red: blue ? 0 : 200, green: 0, blue: blue ? 200 : 0)
        await withCheckedContinuation { continuation in
            movie.queue.async { movie.consumeCamera(sample.sample, at: time); continuation.resume() }
        }
        try? await Task.sleep(for: .milliseconds(10))
    }
    for index in 0..<10 { await cameraFrame(at: 400.05 + Double(index) / 30) }
    movie.pause(at: 400.4)
    await cameraFrame(at: 400.45, blue: true)
    movie.resume(at: 400.6)
    await cameraFrame(at: 400.65)
    movie.setCameraOverlay(nil, at: 400.8)
    await cameraFrame(at: 400.85, blue: true)
    movie.setCameraOverlay(overlay, at: 400.9)
    await cameraFrame(at: 401)
    let translucentOverlay = RecordingCameraOverlay(windowFrame: CGRect(x: 100, y: 30, width: 120, height: 120),
        screenFrame: cameraTarget.rect, target: cameraTarget, shape: .circle, mirrored: false, opacity: 0.4)
    movie.setCameraOverlay(translucentOverlay, at: 401.3)
    for index in 0..<5 { await cameraFrame(at: 401.34 + Double(index) / 30) }
    movie.setCameraOverlay(overlay, at: 401.6)
    await cameraFrame(at: 401.65)
    try await movie.finish(at: 401.8)
    let cameraAsset = AVURLAsset(url: cameraURL)
    let cameraTrack = try await cameraAsset.loadTracks(withMediaType: .video)[0]
    let cameraReader = try AVAssetReader(asset: cameraAsset)
    let cameraOutput = AVAssetReaderTrackOutput(track: cameraTrack,
        outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
    cameraReader.add(cameraOutput)
    expect(cameraReader.startReading(), "The composited camera movie decodes")
    var opaqueFrames = 0
    var hiddenFrames = 0
    var translucentFrames = 0
    var restoredOpaqueFrames = 0
    while let sample = cameraOutput.copyNextSampleBuffer() {
        let image = sample.imageBuffer!
        CVPixelBufferLockBaseAddress(image, .readOnly)
        let bytes = CVPixelBufferGetBaseAddress(image)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(image)
        let center = 90 * stride + 160 * 4
        expect(bytes[center] < 20, "Paused or hidden camera frames never enter the movie")
        if bytes[center + 2] > 180 {
            opaqueFrames += 1
            expect(bytes[center + 1] < 20, "The saved camera is opaque; the green desktop cannot show through")
        }
        let time = sample.presentationTimeStamp.seconds
        if time > 1.11 && time < 1.39 {
            translucentFrames += 1
            expect(bytes[center + 2] > 100 && bytes[center + 2] < 170
                   && bytes[center + 1] > 120 && bytes[center + 1] < 190,
                   "Enabling recorded transparency blends the chosen camera opacity with the desktop")
        }
        if time > 1.41 {
            restoredOpaqueFrames += 1
            expect(bytes[center + 2] > 180 && bytes[center + 1] < 20,
                   "Disabling recorded transparency restores an opaque camera in the same movie")
        }
        if sample.presentationTimeStamp.seconds >= 0.6 && sample.presentationTimeStamp.seconds < 0.8 {
            hiddenFrames += 1
            expect(bytes[center + 1] > 180, "Hiding the camera restores the unobstructed screen")
        }
        let corner = 31 * stride + 101 * 4
        expect(bytes[corner + 1] > 180 && bytes[corner + 2] < 20,
               "The camera's circular corners preserve the desktop in the encoded movie")
        CVPixelBufferUnlockBaseAddress(image, .readOnly)
    }
    expect(opaqueFrames >= 6, "Camera frames keep recording when no new desktop frames arrive")
    expect(hiddenFrames > 0, "Visibility changes update a static desktop recording")
    expect(translucentFrames >= 3 && restoredOpaqueFrames > 0,
           "Recording transparency can be enabled and disabled without restarting capture")
    expect(cameraReader.status == .completed, "The camera movie is completely readable")
    let cameraDuration = try await cameraAsset.load(.duration).seconds
    expect(near(cameraDuration, 1.6, tolerance: 0.05), "Camera composition preserves pause/resume timing")
    print("Passed \(checks) recording checks, including encoding and decoding MOV video/audio, pause/resume, clipping, and static frames.")
}
