// ScreenRecorderPlugin.swift — macOS Flutter Plugin
//
// Pipeline:
//   Screen Region  →  ScreenCaptureKit (SCStream)  →  CVPixelBuffer (BGRA)
//                                                    ↓
//                                             VTCompressionSession (H.264)
//                                                    ↓
//   Microphone  →  AVCaptureSession (Float32 PCM)   ↓
//                         ↓                   AVAssetWriter → .mp4
//                   Float32 → Int16 PCM
//                         ↓
//                  AVAssetWriterInput (AAC)
//
// Method channel: "com.screenrecorder/recorder"  (matches Windows + Dart side)
// Methods handled:
//   checkPermissions    → Bool
//   requestPermissions  → void
//   startRecording      → void  (args: x, y, width, height, outputPath)
//   stopRecording       → String (output file path)
//   captureDesktopScreenshot → NOT NEEDED on macOS (returns notImplemented)
//   setFullscreen       → NOT NEEDED on macOS (returns success silently)
//
// Requires macOS 12.3+ for ScreenCaptureKit.
// Falls back to a graceful error on older macOS.
//
// Info.plist entries required in your macOS Runner:
//   NSMicrophoneUsageDescription  — "Record microphone audio"
//   (Screen recording permission is handled via System Preferences / TCC,
//    no plist key needed but the app must request it at runtime.)

import Cocoa
import FlutterMacOS
import AVFoundation
import VideoToolbox
import CoreMedia
import CoreVideo
import CoreGraphics
import ScreenCaptureKit   // macOS 12.3+

// ─── Log helper ──────────────────────────────────────────────────────────────
// All log lines are prefixed so the tester can grep them easily.
private func log(_ msg: String) {
    print("[ScreenRecorder] \(msg)")
}

// ─── Plugin registration entry point ─────────────────────────────────────────
public class ScreenRecorderPlugin: NSObject, FlutterPlugin {

    public static func register(with registrar: FlutterPluginRegistrar) {
        log("register() called — setting up method channel")
        let channel = FlutterMethodChannel(
            name: "com.screenrecorder/recorder",
            binaryMessenger: registrar.messenger)
        let instance = ScreenRecorderPlugin()
        registrar.addMethodCallDelegate(instance, channel: channel)
        log("✅ Method channel registered")
    }

    // One live recording at a time
    private var recorder: MacRecorder?

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        log("handle() method=\(call.method)")
        switch call.method {

        case "checkPermissions":
            checkPermissions(result: result)

        case "requestPermissions":
            requestPermissions(result: result)

        case "startRecording":
            guard let args = call.arguments as? [String: Any] else {
                log("❌ startRecording — bad args type: \(String(describing: call.arguments))")
                result(FlutterError(code: "INVALID_ARGS", message: "Expected a map", details: nil))
                return
            }
            startRecording(args: args, result: result)

        case "stopRecording":
            stopRecording(result: result)

        // These are Windows-only; return graceful no-ops on macOS
        case "captureDesktopScreenshot":
            log("captureDesktopScreenshot — not needed on macOS, returning nil")
            result(nil)

        case "setFullscreen":
            log("setFullscreen — no-op on macOS")
            result(nil)

        default:
            log("⚠️ unhandled method: \(call.method)")
            result(FlutterMethodNotImplemented)
        }
    }

    // ── Permission check ──────────────────────────────────────────────────────
    private func checkPermissions(result: @escaping FlutterResult) {
        if #available(macOS 12.3, *) {
            log("checkPermissions — checking SCK + mic")
            Task {
                let screenOK = await checkScreenCapturePermission()
                let micOK    = checkMicPermission()
                log("checkPermissions — screen=\(screenOK) mic=\(micOK)")
                DispatchQueue.main.async {
                    result(screenOK && micOK)
                }
            }
        } else {
            log("checkPermissions — macOS < 12.3, ScreenCaptureKit unavailable")
            result(FlutterError(
                code: "UNSUPPORTED_OS",
                message: "Screen recording requires macOS 12.3 or later",
                details: nil))
        }
    }

    @available(macOS 12.3, *)
    private func checkScreenCapturePermission() async -> Bool {
        do {
            // Calling getShareableContent triggers the TCC permission prompt
            // if not yet decided, and returns an error if denied.
            let _ = try await SCShareableContent.excludingDesktopWindows(
                false, onScreenWindowsOnly: false)
            log("  screen capture permission: GRANTED")
            return true
        } catch {
            log("  screen capture permission: DENIED or ERROR — \(error)")
            return false
        }
    }

    private func checkMicPermission() -> Bool {
        let status = AVCaptureDevice.authorizationStatus(for: .audio)
        log("  microphone permission status: \(status.rawValue)")
        return status == .authorized
    }

    // ── Permission request ────────────────────────────────────────────────────
    private func requestPermissions(result: @escaping FlutterResult) {
        log("requestPermissions — requesting mic + screen")
        // Microphone: standard AVFoundation prompt
        AVCaptureDevice.requestAccess(for: .audio) { granted in
            log("  mic permission response: \(granted)")
        }
        // Screen recording: open System Preferences pane so user can grant it
        // (macOS doesn't have a programmatic prompt for screen recording in SCKit)
        if #available(macOS 12.3, *) {
            Task {
                let ok = await self.checkScreenCapturePermission()
                if !ok {
                    log("  screen permission denied — opening System Preferences")
                    DispatchQueue.main.async {
                        // Open the Privacy & Security → Screen Recording pane
                        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
                            NSWorkspace.shared.open(url)
                        }
                    }
                }
                DispatchQueue.main.async { result(nil) }
            }
        } else {
            result(nil)
        }
    }

    // ── Start recording ───────────────────────────────────────────────────────
    private func startRecording(args: [String: Any], result: @escaping FlutterResult) {
        guard #available(macOS 12.3, *) else {
            result(FlutterError(
                code: "UNSUPPORTED_OS",
                message: "Screen recording requires macOS 12.3 or later",
                details: nil))
            return
        }

        if recorder != nil {
            log("❌ startRecording — already recording")
            result(FlutterError(code: "ALREADY_RECORDING",
                                message: "Recording already in progress", details: nil))
            return
        }

        // Parse args — same keys as Windows Dart side sends
        guard
            let x          = args["x"]          as? Int,
            let y          = args["y"]          as? Int,
            let width      = args["width"]      as? Int,
            let height     = args["height"]     as? Int,
            let outputPath = args["outputPath"] as? String,
            width > 0, height > 0, !outputPath.isEmpty
        else {
            log("❌ startRecording — invalid args: \(args)")
            result(FlutterError(code: "INVALID_ARGS",
                                message: "Invalid region or output path", details: nil))
            return
        }

        log("startRecording — region=(\(x),\(y) \(width)×\(height)) path=\(outputPath)")

        let rec = MacRecorder()
        recorder = rec

        Task {
            do {
                try await rec.start(x: x, y: y, width: width, height: height,
                                    outputPath: outputPath)
                log("✅ startRecording — recorder started")
                DispatchQueue.main.async { result(nil) }
            } catch {
                log("❌ startRecording failed: \(error)")
                self.recorder = nil
                DispatchQueue.main.async {
                    result(FlutterError(code: "START_FAILED",
                                        message: error.localizedDescription,
                                        details: "\(error)"))
                }
            }
        }
    }

    // ── Stop recording ────────────────────────────────────────────────────────
    private func stopRecording(result: @escaping FlutterResult) {
        guard let rec = recorder else {
            log("❌ stopRecording — not recording")
            result(FlutterError(code: "NOT_RECORDING",
                                message: "No active recording", details: nil))
            return
        }
        recorder = nil
        log("stopRecording — stopping…")

        Task {
            let path = await rec.stop()
            log("✅ stopRecording — finalized → \(path)")
            DispatchQueue.main.async { result(path) }
        }
    }
}

// ─── MacRecorder: the actual recording engine ─────────────────────────────────
@available(macOS 12.3, *)
private class MacRecorder: NSObject {

    // ── Config ────────────────────────────────────────────────────────────────
    private let kFPS: Int32   = 30
    private let kVideoBitrate = 4_000_000  // 4 Mbps, same as Windows

    // ── AVAssetWriter ─────────────────────────────────────────────────────────
    private var assetWriter:      AVAssetWriter?
    private var videoInput:       AVAssetWriterInput?
    private var audioInput:       AVAssetWriterInput?
    private var pixelBufferAdaptor: AVAssetWriterInputPixelBufferAdaptor?

    // ── ScreenCaptureKit ──────────────────────────────────────────────────────
    private var scStream:         SCStream?
    private var captureDisplay:   SCDisplay?

    // ── AVCapture (microphone) ─────────────────────────────────────────────
    private var captureSession:   AVCaptureSession?
    private var audioOutput:      AVCaptureAudioDataOutput?

    // ── Timing ────────────────────────────────────────────────────────────────
    // Both video and audio lock to the same startTime so A/V stays in sync.
    private var startTime:        CMTime = .invalid
    private let timingLock        = NSLock()

    // ── Region (physical pixels on the selected display) ──────────────────────
    private var capX: Int = 0
    private var capY: Int = 0
    private var capW: Int = 0
    private var capH: Int = 0

    // ── State ─────────────────────────────────────────────────────────────────
    private var outputPath: String = ""
    private var isRunning   = false
    private var frameCount  = 0
    private var audioCount  = 0

    // ── Dispatch queues ───────────────────────────────────────────────────────
    // Video and audio each get their own serial queue (mirrors Windows threads)
    private let videoQueue = DispatchQueue(label: "com.screenrecorder.video",
                                           qos: .userInteractive)
    private let audioQueue = DispatchQueue(label: "com.screenrecorder.audio",
                                           qos: .userInteractive)

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - start()
    // ─────────────────────────────────────────────────────────────────────────
    func start(x: Int, y: Int, width: Int, height: Int, outputPath: String) async throws {
        log("MacRecorder.start()")

        self.capX       = x
        self.capY       = y
        self.capW       = width % 2 == 0 ? width  : width  - 1   // must be even for H.264
        self.capH       = height % 2 == 0 ? height : height - 1
        self.outputPath = outputPath

        // 1. Enumerate shareable content and find the right display
        log("  step 1 — enumerating shareable content")
        let content = try await SCShareableContent.excludingDesktopWindows(
            false, onScreenWindowsOnly: false)
        log("  found \(content.displays.count) display(s), \(content.windows.count) window(s)")

        guard let display = findDisplay(for: content, logicalX: x, logicalY: y) else {
            log("❌ no display found containing origin (\(x),\(y))")
            throw RecorderError.noDisplay
        }
        captureDisplay = display
        log("  selected display id=\(display.displayID) size=\(display.width)×\(display.height)")

        // 2. Set up AVAssetWriter
        log("  step 2 — setting up AVAssetWriter at \(outputPath)")
        try setupAssetWriter()

        // 3. Set up microphone capture (non-fatal — falls back to video-only)
        log("  step 3 — setting up microphone")
        setupMicrophone()

        // 4. Set up SCStream for screen capture
        log("  step 4 — setting up SCStream")
        try await setupSCStream(display: display, content: content)

        // 5. Start everything
        log("  step 5 — starting asset writer + streams")
        guard assetWriter!.startWriting() else {
            let err = assetWriter?.error
            log("❌ AVAssetWriter.startWriting() failed: \(String(describing: err))")
            throw err ?? RecorderError.assetWriterFailed
        }
        log("  AVAssetWriter started, status=\(assetWriter!.status.rawValue)")

        // Start capture session (mic)
        captureSession?.startRunning()
        log("  AVCaptureSession started: \(captureSession?.isRunning == true)")

        // Start screen stream
        try await scStream!.startCapture()
        log("  SCStream started ✅")

        isRunning = true
        log("✅ MacRecorder running")
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - stop()
    // ─────────────────────────────────────────────────────────────────────────
    func stop() async -> String {
        log("MacRecorder.stop()")
        isRunning = false

        // Stop screen stream
        if let stream = scStream {
            do {
                try await stream.stopCapture()
                log("  SCStream stopped")
            } catch {
                log("  SCStream stopCapture error (non-fatal): \(error)")
            }
            scStream = nil
        }

        // Stop mic
        captureSession?.stopRunning()
        log("  AVCaptureSession stopped")
        captureSession = nil

        // Finalize asset writer
        if let writer = assetWriter {
            videoInput?.markAsFinished()
            audioInput?.markAsFinished()
            log("  inputs marked finished — finalizing writer (frames=\(frameCount) audio=\(audioCount))")
            await writer.finishWriting()
            let status = writer.status
            log("  AVAssetWriter finalized, status=\(status.rawValue) error=\(String(describing: writer.error))")
            assetWriter = nil
        }

        log("✅ Recording finalized → \(outputPath)")
        return outputPath
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - Display selection
    // ─────────────────────────────────────────────────────────────────────────
    // Flutter gives us logical coordinates. SCDisplay.frame is in points.
    // We find the display whose frame contains the capture origin.
    private func findDisplay(for content: SCShareableContent, logicalX: Int, logicalY: Int) -> SCDisplay? {
        log("  findDisplay for logical origin (\(logicalX), \(logicalY))")
        for d in content.displays {
            let f = d.frame
            log("    display id=\(d.displayID) frame=\(f)")
            if f.contains(CGPoint(x: logicalX, y: logicalY)) {
                return d
            }
        }
        // Fallback: return the first display if none matched (e.g. secondary monitors)
        log("  ⚠️ no exact match — using first display as fallback")
        return content.displays.first
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - AVAssetWriter setup
    // ─────────────────────────────────────────────────────────────────────────
    private func setupAssetWriter() throws {
        let url = URL(fileURLWithPath: outputPath)

        // Remove existing file if present (AVAssetWriter won't overwrite)
        try? FileManager.default.removeItem(at: url)

        // Create output directory if needed
        let dir = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: dir,
            withIntermediateDirectories: true, attributes: nil)
        log("  output dir ready: \(dir.path)")

        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        log("  AVAssetWriter created for \(url.path)")

        // ── Video input ───────────────────────────────────────────────────────
        // SCStream gives us BGRA frames. AVAssetWriter + H.264 prefers NV12 (420v),
        // but we let VideoToolbox handle the color conversion internally by
        // specifying BGRA as the input format and H.264 as output.
        let videoSettings: [String: Any] = [
            AVVideoCodecKey:                 AVVideoCodecType.h264,
            AVVideoWidthKey:                 capW,
            AVVideoHeightKey:                capH,
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey:               kVideoBitrate,
                AVVideoMaxKeyFrameIntervalKey:           kFPS,       // keyframe every second
                AVVideoExpectedSourceFrameRateKey:       kFPS,
                AVVideoProfileLevelKey:                  AVVideoProfileLevelH264HighAutoLevel,
                // Allow hardware acceleration on any GPU (avoids the Intel driver
                // crash we saw on Windows by going through Apple's VT framework)
                kVTCompressionPropertyKey_RealTime as String: true,
            ],
        ]
        log("  video settings: \(videoSettings)")

        let vidInput = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        vidInput.expectsMediaDataInRealTime = true

        // Pixel buffer adaptor: BGRA source pixels from SCStream
        let pbAttrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey           as String: capW,
            kCVPixelBufferHeightKey          as String: capH,
        ]
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(
            assetWriterInput: vidInput,
            sourcePixelBufferAttributes: pbAttrs)

        guard writer.canAdd(vidInput) else {
            log("❌ AVAssetWriter cannot add video input")
            throw RecorderError.assetWriterFailed
        }
        writer.add(vidInput)
        videoInput        = vidInput
        pixelBufferAdaptor = adaptor
        log("  ✅ video input added (\(capW)×\(capH) @ \(kFPS)fps)")

        // ── Audio input ───────────────────────────────────────────────────────
        // We encode to AAC at 128 kbps, matching Windows output
        let audioSettings: [String: Any] = [
            AVFormatIDKey:              kAudioFormatMPEG4AAC,
            AVSampleRateKey:            44100,
            AVNumberOfChannelsKey:      2,
            AVEncoderBitRateKey:        128_000,
        ]
        let audInput = AVAssetWriterInput(mediaType: .audio, outputSettings: audioSettings)
        audInput.expectsMediaDataInRealTime = true

        if writer.canAdd(audInput) {
            writer.add(audInput)
            audioInput = audInput
            log("  ✅ audio input added (AAC 44100Hz stereo 128kbps)")
        } else {
            log("  ⚠️ AVAssetWriter cannot add audio input — video-only mode")
        }

        assetWriter = writer
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - Microphone setup (AVCaptureSession)
    // ─────────────────────────────────────────────────────────────────────────
    private func setupMicrophone() {
        guard audioInput != nil else {
            log("  setupMicrophone — skipping (no audio input)")
            return
        }

        guard let micDevice = AVCaptureDevice.default(for: .audio) else {
            log("  ⚠️ no default mic device found — audio disabled")
            audioInput = nil
            return
        }
        log("  mic device: \(micDevice.localizedName)")

        do {
            let micInput = try AVCaptureDeviceInput(device: micDevice)
            let session  = AVCaptureSession()

            guard session.canAddInput(micInput) else {
                log("  ⚠️ session cannot add mic input")
                return
            }
            session.addInput(micInput)

            let output = AVCaptureAudioDataOutput()
            output.setSampleBufferDelegate(self, queue: audioQueue)

            guard session.canAddOutput(output) else {
                log("  ⚠️ session cannot add audio output")
                return
            }
            session.addOutput(output)

            captureSession = session
            audioOutput    = output
            log("  ✅ microphone capture session ready")
        } catch {
            log("  ⚠️ mic setup error: \(error) — audio disabled")
            audioInput = nil
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - ScreenCaptureKit setup
    // ─────────────────────────────────────────────────────────────────────────
    private func setupSCStream(display: SCDisplay, content: SCShareableContent) async throws {
        let cfg = SCStreamConfiguration()

        // Frame dimensions — must match what we told AVAssetWriter
        cfg.width  = capW
        cfg.height = capH

        // Crop to our region.
        // SCStreamConfiguration.sourceRect is in DISPLAY POINTS (not pixels).
        // On a Retina display the scale factor is typically 2.0, so we divide
        // physical pixel coords by the display scale factor.
        let scale  = displayScaleFactor(displayID: display.displayID)
        let srcRect = CGRect(
            x:      CGFloat(capX - Int(display.frame.minX)) / scale,
            y:      CGFloat(capY - Int(display.frame.minY)) / scale,
            width:  CGFloat(capW) / scale,
            height: CGFloat(capH) / scale)
        cfg.sourceRect = srcRect
        log("  SCStream sourceRect=\(srcRect) scale=\(scale)")

        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(kFPS))
        cfg.pixelFormat          = kCVPixelFormatType_32BGRA   // matches our BGRA pipeline
        cfg.showsCursor          = true
        cfg.capturesAudio        = false  // we use AVCapture for mic instead

        // Filter: capture only the selected display, exclude our own app window
        let filter = SCContentFilter(display: display,
                                     excludingApplications: [],
                                     exceptingWindows: [])
        let stream = SCStream(filter: filter, configuration: cfg, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: videoQueue)
        scStream = stream
        log("  ✅ SCStream configured")
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - Display scale factor helper
    // ─────────────────────────────────────────────────────────────────────────
    private func displayScaleFactor(displayID: CGDirectDisplayID) -> CGFloat {
        // Find the NSScreen matching this display ID and read its backingScaleFactor
        for screen in NSScreen.screens {
            let sid = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID
            if sid == displayID {
                log("  display \(displayID) backingScaleFactor=\(screen.backingScaleFactor)")
                return screen.backingScaleFactor
            }
        }
        log("  ⚠️ could not find NSScreen for displayID=\(displayID) — defaulting scale=2.0")
        return 2.0  // Safe default for Retina
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MARK: - Timing helper
    // ─────────────────────────────────────────────────────────────────────────
    // Both video and audio call this. The first caller anchors startTime;
    // subsequent callers compute relative timestamps from the same anchor.
    private func relativePTS(for sampleTime: CMTime) -> CMTime {
        timingLock.lock()
        defer { timingLock.unlock() }

        if startTime == .invalid {
            // First sample of either stream — anchor here and tell AVAssetWriter
            startTime = sampleTime
            assetWriter?.startSession(atSourceTime: sampleTime)
            log("  ⏱ session start anchored at \(sampleTime.seconds)s")
        }
        return CMTimeSubtract(sampleTime, startTime)
    }
}

// ─── SCStreamDelegate ─────────────────────────────────────────────────────────
@available(macOS 12.3, *)
extension MacRecorder: SCStreamDelegate {
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("❌ SCStream stopped with error: \(error)")
    }
}

// ─── SCStreamOutput (video frames) ────────────────────────────────────────────
@available(macOS 12.3, *)
extension MacRecorder: SCStreamOutput {

    func stream(_ stream: SCStream,
                didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {

        guard type == .screen, isRunning else { return }
        guard let vidInput = videoInput, vidInput.isReadyForMoreMediaData else {
            // Encoder is busy — drop this frame (same as Windows DXGI timeout path)
            return
        }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            log("⚠️ frame dropped — no pixel buffer")
            return
        }

        let sampleTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let pts        = relativePTS(for: sampleTime)

        // Append BGRA pixel buffer directly — VideoToolbox will compress to H.264
        let ok = pixelBufferAdaptor?.append(pixelBuffer, withPresentationTime: pts) ?? false
        if !ok {
            log("⚠️ pixelBufferAdaptor append failed at pts=\(pts.seconds)s status=\(assetWriter?.status.rawValue ?? -1)")
        }

        frameCount += 1
        if frameCount == 1 || frameCount % 150 == 0 {
            log("🎬 frame #\(frameCount) pts=\(String(format: "%.2f", pts.seconds))s")
        }
    }
}

// ─── AVCaptureAudioDataOutputSampleBufferDelegate (mic audio) ─────────────────
@available(macOS 12.3, *)
extension MacRecorder: AVCaptureAudioDataOutputSampleBufferDelegate {

    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {

        guard isRunning else { return }
        guard let audInput = audioInput, audInput.isReadyForMoreMediaData else { return }

        let sampleTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let pts        = relativePTS(for: sampleTime)

        // Restamp the buffer with our relative PTS before writing
        if let restamped = restamp(sampleBuffer: sampleBuffer, pts: pts) {
            let ok = audInput.append(restamped)
            if !ok {
                log("⚠️ audio append failed at pts=\(pts.seconds)s")
            }
        } else {
            // Fallback: try to append original buffer — AVAssetWriter may reject it
            // if timestamps don't align, but worth trying
            _ = audInput.append(sampleBuffer)
        }

        audioCount += 1
        if audioCount == 1 || audioCount % 300 == 0 {
            log("🎤 audio packet #\(audioCount) pts=\(String(format: "%.2f", pts.seconds))s")
        }
    }

    // Restamp a CMSampleBuffer with a new presentation time
    // (needed so audio PTS is relative to the same anchor as video PTS)
    private func restamp(sampleBuffer: CMSampleBuffer, pts: CMTime) -> CMSampleBuffer? {
        var timingInfo = CMSampleTimingInfo(
            duration:               CMSampleBufferGetDuration(sampleBuffer),
            presentationTimeStamp:  pts,
            decodeTimeStamp:        .invalid)

        var out: CMSampleBuffer?
        let status = CMSampleBufferCreateCopyWithNewTiming(
            allocator:          kCFAllocatorDefault,
            sampleBuffer:       sampleBuffer,
            sampleTimingEntryCount: 1,
            sampleTimingArray:  &timingInfo,
            sampleBufferOut:    &out)

        if status != noErr {
            log("⚠️ CMSampleBufferCreateCopyWithNewTiming failed: \(status)")
            return nil
        }
        return out
    }
}

// ─── Error types ──────────────────────────────────────────────────────────────
private enum RecorderError: LocalizedError {
    case noDisplay
    case assetWriterFailed

    var errorDescription: String? {
        switch self {
        case .noDisplay:        return "No display found for the given screen coordinates"
        case .assetWriterFailed: return "AVAssetWriter failed to start"
        }
    }
}